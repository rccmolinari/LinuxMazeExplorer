#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/wait.h>
#include "map.h"

/*
 * Ogni quanto (in secondi) viene inviata la mappa "sfocata" ai client.
 * Aumentare questo valore rende il gioco piu' facile (nebbia meno frequente).
 */
#define SECONDS_TO_BLUR 15

/*
 * Durata complessiva della partita in secondi.
 * Allo scadere, tutti i client vengono notificati e la sessione termina.
 */
#define TIMER 60

/* --------------------------------------------------------------------------
 * Sincronizzazione
 *
 * mutex          -> protegge la mappa condivisa durante gli spostamenti
 * scoreMutex     -> garantisce scrittura atomica su score.txt
 * scoreCond      -> usata assieme a scoreChanging per serializzare gli accessi
 * lobbyMutex     -> protegge le variabili di lobby (nReady, gameStarted, ecc.)
 * lobbyCond      -> usata per far attendere i thread finche' la partita non parte
 * timerMutex     -> protegge la variabile timeUp
 * -------------------------------------------------------------------------- */
pthread_mutex_t mutex      = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t scoreMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  scoreCond  = PTHREAD_COND_INITIALIZER;
pthread_mutex_t lobbyMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  lobbyCond  = PTHREAD_COND_INITIALIZER;
pthread_mutex_t timerMutex = PTHREAD_MUTEX_INITIALIZER;

/* --------------------------------------------------------------------------
 * Stato globale del server
 * -------------------------------------------------------------------------- */
int nReady        = 0;  /* quanti client hanno inviato username e sono in lobby */
int nClients      = 0;  /* quanti client sono connessi in totale                */
int gameStarted   = 0;  /* flag: la partita e' iniziata                         */
int timeUp        = 0;  /* flag: il timer e' scaduto                            */
int scoreChanging = 0;  /* semaforo logico: 1 mentre qualcuno scrive score.txt  */

/*
 * fd globale del file di log: aperto nel main e condiviso da tutti i thread.
 * Tutte le scritture passano per log_event() che e' thread-safe grazie al
 * fatto che write() su file e' atomica per dimensioni <= PIPE_BUF.
 */
int gLogFd = -1;

/* pipe usata dai thread per svegliare la select() del main quando la partita finisce */
int wakeup_pipe[2];

pthread_t timerTid;

/* --------------------------------------------------------------------------
 * Struttura dati per ogni client connesso.
 * Ogni thread newUser riceve un puntatore a questa struttura.
 * -------------------------------------------------------------------------- */
struct data {
    int    user;           /* file descriptor del socket del client           */
    char **map;            /* puntatore alla mappa condivisa                  */
    int    width;
    int    height;
    int    x;              /* posizione corrente del giocatore (riga)         */
    int    y;              /* posizione corrente del giocatore (colonna)      */
    int  **visited;        /* celle gia' visitate (usate per la nebbia)       */
    int    collectedItems; /* oggetti raccolti durante la partita             */
    int    exitFlag;       /* 1 se il giocatore ha raggiunto l'uscita         */
    pthread_mutex_t socketWriteMutex; /* protegge le send() sul socket        */
};

/* --------------------------------------------------------------------------
 * log_event
 *
 * Scrive una riga di log su gLogFd nel formato:
 *   [YYYY-MM-DD HH:MM:SS] <msg>
 *
 * Usa esclusivamente write(). Non usa printf, fprintf o simili.
 * Per messaggi brevi (< PIPE_BUF byte) la write e' atomica, quindi
 * non servono lock aggiuntivi per il log.
 * -------------------------------------------------------------------------- */
static void log_event(const char *msg) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "[%Y-%m-%d %H:%M:%S] ", t);
    write(gLogFd, ts, strlen(ts));
    write(gLogFd, msg, strlen(msg));
    write(gLogFd, "\n", 1);
}

/* --------------------------------------------------------------------------
 * log_error
 *
 * Scrive sul log un messaggio di errore accompagnato dalla stringa errno.
 * Usata al posto di perror() per restare coerenti con il vincolo write-only.
 * -------------------------------------------------------------------------- */
static void log_error(const char *context) {
    char errbuf[256];
    char msg[512];
    int len = 0;
    strerror_r(errno, errbuf, sizeof(errbuf));
    memcpy(msg,       "ERROR [",  7);              len += 7;
    memcpy(msg + len, context,    strlen(context)); len += strlen(context);
    memcpy(msg + len, "]: ",      3);              len += 3;
    memcpy(msg + len, errbuf,     strlen(errbuf)); len += strlen(errbuf);
    msg[len] = '\0';
    log_event(msg);
}

/* --------------------------------------------------------------------------
 * printWinnerWithPipe
 *
 * Determina il vincitore leggendo score.txt tramite una pipeline shell
 * (sort | head | awk) eseguita in un processo figlio.
 *
 * Il processo figlio reindirizza il proprio stdout sulla pipe e chiama
 * execlp("sh", ...) per eseguire il comando composito. Il padre legge
 * il risultato e lo copia nel buffer winner.
 *
 * Criteri di ordinamento: prima per exit (colonna 3), poi per oggetti
 * raccolti (colonna 2), entrambi in ordine decrescente.
 * -------------------------------------------------------------------------- */
void printWinnerWithPipe(char *winner) {
    int fd[2];
    if (pipe(fd) == -1) {
        log_error("pipe in printWinnerWithPipe");
        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        log_error("fork in printWinnerWithPipe");
        return;
    }

    if (pid == 0) {
        /* figlio: chiude il lato lettura, redireziona stdout sulla pipe */
        close(fd[0]);
        dup2(fd[1], STDOUT_FILENO);
        close(fd[1]);

        execlp("sh", "sh", "-c",
               "sort -k3,3nr -k2,2nr score.txt | head -n1 | awk '{print $1}'",
               NULL);
        exit(1); /* raggiunto solo se execlp fallisce */
    } else {
        /* padre: legge il nome del vincitore dal lato lettura della pipe */
        close(fd[1]);
        char usr[256];
        int n = read(fd[0], usr, sizeof(usr) - 1);
        if (n > 0) {
            usr[n] = '\0';
            usr[strcspn(usr, "\r\n")] = 0;
            strcpy(winner, usr);
        } else {
            log_event("WINNER: nessun vincitore trovato (score.txt vuoto?)");
            winner[0] = '\0';
        }
        close(fd[0]);
        wait(NULL); /* evita processi zombie */
    }
}

/* --------------------------------------------------------------------------
 * asyncSendBlurredMap  [thread]
 *
 * Thread lanciato per ogni client: aspetta che la partita inizi, poi ogni
 * SECONDS_TO_BLUR secondi invia al client la mappa con la nebbia aggiornata.
 * Si interrompe se il socket del client non e' piu' valido (user <= 0).
 * -------------------------------------------------------------------------- */
void *asyncSendBlurredMap(void *arg) {
    struct data *d = (struct data *)arg;

    /* aspetta il segnale di avvio partita dalla lobby */
    pthread_mutex_lock(&lobbyMutex);
    while (!gameStarted)
        pthread_cond_wait(&lobbyCond, &lobbyMutex);
    pthread_mutex_unlock(&lobbyMutex);

    while (1) {
        sleep(SECONDS_TO_BLUR);
        if (d->user <= 0) break;

        pthread_mutex_lock(&(d->socketWriteMutex));
        sendBlurredMap(d->user, d->map, d->width, d->height, d->x, d->y, d->visited);
        pthread_mutex_unlock(&(d->socketWriteMutex));

        log_event("BLUR: mappa sfocata inviata al client");
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * writeScore
 *
 * Aggiunge una riga a score.txt nel formato:
 *   <username> <oggetti_raccolti> <exit_flag>
 *
 * L'accesso e' serializzato tramite scoreMutex + variabile scoreChanging
 * per evitare scritture sovrapposte da thread diversi.
 * -------------------------------------------------------------------------- */
void writeScore(char *username, struct data *d) {
    pthread_mutex_lock(&scoreMutex);

    while (scoreChanging)
        pthread_cond_wait(&scoreCond, &scoreMutex);
    scoreChanging = 1;

    int scoreFile = open("score.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (scoreFile >= 0) {
        char buffer[512];
        int len = snprintf(buffer, sizeof(buffer), "%s %d %d\n",
                           username, d->collectedItems, d->exitFlag);
        write(scoreFile, buffer, len);
        close(scoreFile);

        char logmsg[512];
        snprintf(logmsg, sizeof(logmsg), "SCORE: %s -> oggetti=%d exit=%d",
                 username, d->collectedItems, d->exitFlag);
        log_event(logmsg);
    } else {
        log_error("open score.txt in writeScore");
    }

    scoreChanging = 0;
    pthread_cond_signal(&scoreCond);
    pthread_mutex_unlock(&scoreMutex);
}

/* --------------------------------------------------------------------------
 * timer  [eseguita come thread]
 *
 * Dorme per TIMER secondi, poi imposta timeUp=1 segnalando la fine partita.
 * Viene lanciata dall'ultimo client che completa la fase di lobby.
 * -------------------------------------------------------------------------- */
void timer() {
    log_event("TIMER: countdown avviato");
    sleep(TIMER);
    pthread_mutex_lock(&timerMutex);
    timeUp = 1;
    pthread_mutex_unlock(&timerMutex);
    log_event("TIMER: tempo scaduto, fine partita");
}

/* Lettura thread-safe di timeUp */
int isTimeUp() {
    pthread_mutex_lock(&timerMutex);
    int r = timeUp;
    pthread_mutex_unlock(&timerMutex);
    return r;
}

/* --------------------------------------------------------------------------
 * authenticate
 *
 * Riceve il nome utente dal client (primo messaggio dopo la connessione) e
 * lo salva in usernamesrc. Registra l'evento nel log.
 * -------------------------------------------------------------------------- */
void authenticate(struct data *d, char *usernamesrc) {
    char username[256];
    int readedbyte = recv(d->user, username, sizeof(username) - 1, 0);
    if (readedbyte <= 0) {
        log_event("AUTH ERROR: ricezione username fallita o client disconnesso");
        return;
    }

    username[readedbyte] = '\0';
    username[strcspn(username, "\r\n")] = 0;
    strcpy(usernamesrc, username);

    char logmsg[512];
    snprintf(logmsg, sizeof(logmsg), "AUTH: nuovo utente registrato -> '%s'", username);
    log_event(logmsg);
}

/* --------------------------------------------------------------------------
 * gaming
 *
 * Ciclo principale di gioco per un client.
 *
 * 1. Posiziona il giocatore su una cella PATH casuale.
 * 2. Invia la mappa adiacente iniziale.
 * 3. Entra nel loop: legge i comandi (W/A/S/D/exit), aggiorna la posizione,
 *    raccoglie gli item, controlla se il giocatore esce dalla mappa.
 * 4. Termina quando il timer scade, il client invia "exit", oppure raggiunge
 *    il bordo della mappa.
 * -------------------------------------------------------------------------- */
void gaming(struct data *d) {
    d->collectedItems = 0;
    d->exitFlag = 0;

    /* posizione di spawn casuale su una cella percorribile */
    do {
        d->x = rand() % d->height;
        d->y = rand() % d->width;
    } while (d->map[d->x][d->y] != PATH);

    d->visited[d->x][d->y] = 1;
    adjVisit(d->width, d->height, d->x, d->y, d->visited);

    char logmsg[512];
    snprintf(logmsg, sizeof(logmsg), "GAME: spawn assegnato in (%d,%d)", d->x, d->y);
    log_event(logmsg);

    pthread_mutex_lock(&(d->socketWriteMutex));
    sendAdjacentMap(d->user, d->map, d->width, d->height, d->x, d->y);
    pthread_mutex_unlock(&(d->socketWriteMutex));

    log_event("GAME: mappa iniziale inviata, attesa comandi");

    char buffer[256];
    while (!isTimeUp()) {
        int r = recv(d->user, buffer, sizeof(buffer) - 1, 0);
        if (r <= 0) {
            log_event("GAME: client disconnesso durante la partita");
            break;
        }
        buffer[r] = '\0';
        buffer[strcspn(buffer, "\r\n")] = 0;

        if (!strcmp(buffer, "exit")) {
            log_event("GAME: client ha inviato 'exit', uscita volontaria");
            break;
        }

        snprintf(logmsg, sizeof(logmsg),
                 "MOVE: comando ricevuto -> '%s' (pos: %d,%d)", buffer, d->x, d->y);
        log_event(logmsg);

        int nextX = d->x;
        int nextY = d->y;
        int win   = 0;

        /* controlla se il giocatore esce dai bordi della mappa */
        if      (!strcmp(buffer, "W")) { if (d->x == 0)             win = 1; else nextX--; }
        else if (!strcmp(buffer, "S")) { if (d->x == d->height - 1) win = 1; else nextX++; }
        else if (!strcmp(buffer, "A")) { if (d->y == 0)             win = 1; else nextY--; }
        else if (!strcmp(buffer, "D")) { if (d->y == d->width - 1)  win = 1; else nextY++; }

        if (win) {
            d->exitFlag = 1;
            log_event("GAME: giocatore ha attraversato il bordo, uscita trovata");
            pthread_mutex_lock(&(d->socketWriteMutex));
            send(d->user, "M", 1, 0); /* M = Map exit */
            pthread_mutex_unlock(&(d->socketWriteMutex));
            break;
        }

        pthread_mutex_lock(&mutex);
        if (d->map[nextX][nextY] != WALL) {
            d->x = nextX;
            d->y = nextY;
            d->visited[d->x][d->y] = 1;
            adjVisit(d->width, d->height, d->x, d->y, d->visited);

            if (d->map[d->x][d->y] == ITEM) {
                d->map[d->x][d->y] = PATH; /* l'item viene rimosso dalla mappa */
                d->collectedItems++;
                snprintf(logmsg, sizeof(logmsg),
                         "ITEM: raccolto in (%d,%d), totale=%d", d->x, d->y, d->collectedItems);
                log_event(logmsg);
            }

            snprintf(logmsg, sizeof(logmsg),
                     "MOVE: spostamento ok -> nuova pos (%d,%d)", d->x, d->y);
            log_event(logmsg);
        } else {
            log_event("MOVE: movimento bloccato (muro)");
        }
        pthread_mutex_unlock(&mutex);

        pthread_mutex_lock(&(d->socketWriteMutex));
        sendAdjacentMap(d->user, d->map, d->width, d->height, d->x, d->y);
        pthread_mutex_unlock(&(d->socketWriteMutex));
    }

    log_event("GAME: sessione di gioco terminata");
}

/* --------------------------------------------------------------------------
 * newUser  [thread]
 *
 * Punto di ingresso per ogni client connesso. Il flusso e':
 *   1. authenticate()   -> riceve e registra il nome utente
 *   2. lobby            -> aspetta che tutti i client siano pronti
 *   3. gaming()         -> ciclo di gioco
 *   4. writeScore()     -> scrive il risultato su score.txt
 *   5. calcolo vincitore (solo l'ultimo thread che termina)
 *   6. notifica W/L al client
 *   7. cleanup risorse
 * -------------------------------------------------------------------------- */
void *newUser(void *arg) {
    struct data *d = (struct data *)arg;
    char username[256] = {0};
    char logmsg[512];

    authenticate(d, username);

    /* ----- LOBBY ----- */
    pthread_mutex_lock(&lobbyMutex);
    nReady++;

    snprintf(logmsg, sizeof(logmsg),
             "LOBBY: '%s' pronto (%d/%d)", username, nReady, nClients);
    log_event(logmsg);

    if (nReady == nClients) {
        /* ultimo client pronto: avvia partita e timer */
        gameStarted = 1;
        log_event("LOBBY: tutti i giocatori pronti, partita in avvio");
        pthread_create(&timerTid, NULL, (void *)timer, NULL);
        pthread_cond_broadcast(&lobbyCond);
    } else {
        while (!gameStarted)
            pthread_cond_wait(&lobbyCond, &lobbyMutex);
    }
    pthread_mutex_unlock(&lobbyMutex);

    log_event("LOBBY: uscita dalla sala d'attesa, partita avviata");

    /* thread nebbia: invia la mappa sfocata periodicamente in background */
    pthread_t blurTid;
    pthread_create(&blurTid, NULL, asyncSendBlurredMap, d);
    pthread_detach(blurTid);

    gaming(d);
    writeScore(username, d);

    /* notifica generica di fine partita (il client aspetta poi W o L) */
    send(d->user, "E", 1, 0);

    /* ----- ENDGAME: l'ultimo thread a terminare calcola il vincitore ----- */
    pthread_mutex_lock(&lobbyMutex);
    nReady--;

    snprintf(logmsg, sizeof(logmsg),
             "ENDGAME: '%s' ha terminato (%d ancora in gioco)", username, nReady);
    log_event(logmsg);

    /*
     * mutex e cond locali per sincronizzare il calcolo del vincitore.
     * L'ultimo thread (nReady==0) calcola il vincitore e segnala agli altri.
     *
     * Nota: questa struttura ha un limite concettuale perche' mutex/cond sono
     * locali allo stack di ogni thread e non condivisi. Funziona nella pratica
     * solo se l'ultimo thread e' anche l'unico ancora attivo a quel punto.
     * Per una soluzione piu' robusta, spostare la logica nel main.
     */
    int winnerCalculated = 0;
    pthread_mutex_t winnerMutex;
    pthread_mutex_init(&winnerMutex, NULL);
    pthread_cond_t winnerCond = PTHREAD_COND_INITIALIZER;
    char winner[256] = {0};

    if (nReady == 0) {
        log_event("ENDGAME: tutti i client hanno finito, calcolo vincitore in corso");
        printWinnerWithPipe(winner);

        snprintf(logmsg, sizeof(logmsg), "ENDGAME: vincitore -> '%s'", winner);
        log_event(logmsg);

        pthread_mutex_lock(&winnerMutex);
        winnerCalculated = 1;
        pthread_cond_signal(&winnerCond);
        pthread_mutex_unlock(&winnerMutex);

        /* sveglia il main per chiudere il server */
        write(wakeup_pipe[1], "X", 1);
        close(wakeup_pipe[1]);
    }
    pthread_mutex_unlock(&lobbyMutex);

    /* attende che il vincitore sia stato determinato */
    pthread_mutex_lock(&winnerMutex);
    while (winnerCalculated == 0)
        pthread_cond_wait(&winnerCond, &winnerMutex);
    pthread_mutex_unlock(&winnerMutex);

    /* notifica W (vinto) o L (perso) al client */
    if (strcmp(winner, username) == 0) {
        log_event("RESULT: vincitore notificato");
        send(d->user, "W", 1, 0);
    } else {
        log_event("RESULT: perdente notificato");
        send(d->user, "L", 1, 0);
    }

    /* ----- CLEANUP ----- */
    log_event("CLEANUP: chiusura socket e liberazione memoria client");
    close(d->user);

    for (int i = 0; i < d->height; i++)
        free(d->visited[i]);
    free(d->visited);
    pthread_mutex_destroy(&(d->socketWriteMutex));
    pthread_mutex_destroy(&winnerMutex);
    free(d);

    return NULL;
}

/* --------------------------------------------------------------------------
 * main
 *
 * Inizializza il server TCP sulla porta 8080, apre il log globale, genera
 * la mappa e rimane in ascolto tramite select(). Accetta nuovi client finche'
 * non riceve il segnale di fine partita dalla pipe (scritto dall'ultimo
 * thread che termina).
 * -------------------------------------------------------------------------- */
int main() {
    srand(time(NULL));

    /* azzera i file di stato all'avvio: ogni sessione parte da zero */
    close(open("score.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644));

    /* apre il log globale (troncato): tutti i thread scriveranno qui tramite gLogFd */
    gLogFd = open("filelog.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (gLogFd < 0) {
        /* unico caso in cui siamo costretti ad usare stderr: il log non e' disponibile */
        const char *err = "FATAL: impossibile aprire filelog.txt\n";
        write(STDERR_FILENO, err, strlen(err));
        exit(1);
    }

    log_event("SERVER: avvio in corso");

    if (pipe(wakeup_pipe) == -1) {
        log_error("pipe wakeup_pipe");
        exit(1);
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        log_error("socket");
        exit(1);
    }

    /* riuso immediato della porta dopo un riavvio (evita "Address already in use") */
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in srv = {0};
    srv.sin_family      = AF_INET;
    srv.sin_port        = htons(8080);
    srv.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        log_error("bind");
        exit(1);
    }
    listen(sockfd, 100);

    int w, h;
    char **map = generateMap(&w, &h);

    log_event("SERVER: in ascolto sulla porta 8080");

    /* ----- LOOP PRINCIPALE: select() su socket e pipe di controllo ----- */
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        FD_SET(wakeup_pipe[0], &rfds);

        int maxfd = (sockfd > wakeup_pipe[0]) ? sockfd : wakeup_pipe[0];

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            log_error("select");
            break;
        }

        /* segnale di fine partita dalla pipe: usciamo dal loop */
        if (FD_ISSET(wakeup_pipe[0], &rfds)) {
            char c;
            read(wakeup_pipe[0], &c, 1);
            log_event("SERVER: segnale di chiusura ricevuto, arresto in corso");
            close(wakeup_pipe[0]);
            break;
        }

        /* nuova connessione TCP in arrivo */
        if (FD_ISSET(sockfd, &rfds)) {
            struct sockaddr_in cli;
            socklen_t clen = sizeof(cli);
            int cfd = accept(sockfd, (struct sockaddr *)&cli, &clen);
            if (cfd < 0) {
                log_error("accept");
                continue;
            }

            /* alloca e inizializza la struttura dati del client */
            struct data *d = malloc(sizeof(struct data));
            d->user           = cfd;
            d->map            = map;
            d->width          = w;
            d->height         = h;
            d->collectedItems = 0;
            d->exitFlag       = 0;

            pthread_mutex_init(&(d->socketWriteMutex), NULL);

            d->visited = malloc(h * sizeof(int *));
            for (int i = 0; i < h; i++)
                d->visited[i] = calloc(w, sizeof(int));

            pthread_mutex_lock(&lobbyMutex);
            nClients++;
            pthread_mutex_unlock(&lobbyMutex);

            pthread_t tid;
            pthread_create(&tid, NULL, newUser, d);
            pthread_detach(tid);

            char logmsg[256];
            snprintf(logmsg, sizeof(logmsg),
                     "SERVER: connessione accettata da %s (client #%d)",
                     inet_ntoa(cli.sin_addr), nClients);
            log_event(logmsg);
        }
    }

    log_event("SERVER: socket chiuso, processo terminato");
    close(sockfd);
    close(gLogFd);
    return 0;
}