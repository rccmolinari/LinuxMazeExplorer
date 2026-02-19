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
 * Intervallo in secondi tra un invio di nebbia e il successivo.
 * Valori più alti danno più tempo al giocatore prima che la mappa si oscuri.
 */
#define SECONDS_TO_BLUR 10

/*
 * Durata della partita in secondi. Allo scadere il server notifica
 * tutti i client e la sessione si chiude.
 */
#define TIMER 30

/* --------------------------------------------------------------------------
 * Sincronizzazione
 *
 * mutex          -> protegge la mappa condivisa durante gli spostamenti
 * scoreMutex     -> garantisce scrittura atomica su score.txt
 * scoreCond      -> usata assieme a scoreChanging per serializzare gli accessi
 * lobbyMutex     -> protegge le variabili di lobby (nReady, gameStarted, ecc.)
 * lobbyCond      -> usata per far attendere i thread finche' la partita non parte
 * timerMutex     -> protegge la variabile timeUp
 * logMutex       -> garantisce che le righe di log non si mescolino tra thread
 * -------------------------------------------------------------------------- */
pthread_mutex_t mutex      = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t scoreMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  scoreCond  = PTHREAD_COND_INITIALIZER;
pthread_mutex_t lobbyMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  lobbyCond  = PTHREAD_COND_INITIALIZER;
pthread_mutex_t timerMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t logMutex   = PTHREAD_MUTEX_INITIALIZER;

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
 * Ogni thread newUser riceve un puntatore a questa struttura e la usa
 * per tutta la durata della sessione.
 * -------------------------------------------------------------------------- */
struct data {
    int    user;           /* file descriptor del socket del client           */
    char   ip[INET_ADDRSTRLEN]; /* indirizzo IP del client in formato stringa */
    char   username[256];  /* nome utente, popolato dopo l'autenticazione     */
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
 * Scrive una riga su gLogFd nel formato [YYYY-MM-DD HH:MM:SS] <msg>.
 * Usa solo write() — niente printf o fprintf. Il logMutex garantisce
 * che i messaggi di thread diversi non si mescolino tra loro.
 * -------------------------------------------------------------------------- */
    void log_event(const char *msg) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "[%Y-%m-%d %H:%M:%S] ", t);
    pthread_mutex_lock(&logMutex);
    write(gLogFd, ts, strlen(ts));
    write(gLogFd, msg, strlen(msg));
    write(gLogFd, "\n", 1);
    pthread_mutex_unlock(&logMutex);
}

/* --------------------------------------------------------------------------
 * log_error
 *
 * Scrive sul log il messaggio di errore associato all'errno corrente,
 * preceduto dal contesto indicato. Rimpiazza perror() per restare
 * coerenti con il vincolo di usare solo write().
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
 * Calcola il vincitore leggendo score.txt con una pipeline shell
 * eseguita in un processo figlio (fork + execlp).
 * Il figlio redireziona stdout sulla pipe e lancia:
 *   sort -k3,3nr -k2,2nr score.txt | head -n1 | awk '{print $1}'
 * Il padre legge il risultato e lo copia in winner.
 * Priorita': prima chi ha trovato l'uscita, poi chi ha piu' oggetti.
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

        // Ordinamento per exitFlag decrescente, chi esce prima vince
        execlp("sh", "sh", "-c",
               "awk -F';' '{print NR \";\" $0}' score.txt | sort -t';' -k4,4nr -k1,1n | head -n1 | awk -F';' '{print $2}'",
               NULL);
        exit(1); /* execlp fallito */
    } else {
        /* padre: legge il risultato dal lato lettura della pipe */
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
 * Gira per tutta la durata della partita. Ogni SECONDS_TO_BLUR secondi
 * invia al client la mappa con la nebbia aggiornata attorno alla posizione
 * corrente del giocatore. Si ferma se il socket non e' piu' valido.
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

        char blurlog[512];
        snprintf(blurlog, sizeof(blurlog), "[%s@%s] BLUR: mappa sfocata inviata", d->username, d->ip);
        log_event(blurlog);
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * writeScore
 *
 * Aggiunge una riga a score.txt nel formato:
 *   <username> <oggetti_raccolti> <exit_flag>
 *
 * Gli accessi sono serializzati con scoreMutex + scoreChanging per evitare
 * che due thread scrivano contemporaneamente e corrompano il file.
 * -------------------------------------------------------------------------- */
void writeScore(char *username, struct data *d) {
    pthread_mutex_lock(&scoreMutex);

    while (scoreChanging)
        pthread_cond_wait(&scoreCond, &scoreMutex);
    scoreChanging = 1;

    int scoreFile = open("score.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (scoreFile >= 0) {
        char buffer[512];
        // format: username;collectedItems;exitFlag
        int len = snprintf(buffer, sizeof(buffer), "%s;%d;%d\n",
                           username, d->collectedItems, d->exitFlag);
        write(scoreFile, buffer, len);
        close(scoreFile);

        char logmsg[512];
        snprintf(logmsg, sizeof(logmsg), "[%s@%s] SCORE: oggetti=%d exit=%d",
                 username, d->ip, d->collectedItems, d->exitFlag);
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
 * Aspetta TIMER secondi poi imposta timeUp=1. Viene lanciata dall'ultimo
 * giocatore che entra in lobby, in modo che il conto alla rovescia parta
 * solo quando tutti sono connessi.
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
 * Legge il nome utente inviato dal client come primo messaggio dopo la
 * connessione. Lo salva sia in usernamesrc che in d->username, cosi'
 * tutte le funzioni successive possono includerlo nei log.
 * -------------------------------------------------------------------------- */
void authenticate(struct data *d, char *usernamesrc) {
    char username[256];
    int readedbyte = recv(d->user, username, sizeof(username) - 1, 0);
    if (readedbyte <= 0) {
        char logmsg[512];
        snprintf(logmsg, sizeof(logmsg), "[%s] AUTH: ricezione username fallita, client disconnesso", d->ip);
        log_event(logmsg);
        return;
    }

    username[readedbyte] = '\0';
    username[strcspn(username, "\r\n")] = 0;
    strcpy(usernamesrc, username);
    strncpy(d->username, username, sizeof(d->username) - 1);
    d->username[sizeof(d->username) - 1] = '\0';

    char logmsg[512];
    snprintf(logmsg, sizeof(logmsg), "[%s@%s] AUTH: utente connesso", username, d->ip);
    log_event(logmsg);
}

/* --------------------------------------------------------------------------
 * gaming
 *
 * Ciclo di gioco per un client. Assegna una posizione di spawn casuale su
 * una cella PATH, poi legge i comandi (W/A/S/D/exit) in un loop:
 * - se il giocatore tocca il bordo della mappa, ha trovato l'uscita
 * - se cammina su un ITEM, lo raccoglie e incrementa il contatore
 * - se il muro blocca il movimento, la posizione non cambia
 * Il loop si interrompe per timeout, uscita volontaria o disconnessione.
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
    snprintf(logmsg, sizeof(logmsg), "[%s@%s] GAME: spawn assegnato in (%d,%d)", d->username, d->ip, d->x, d->y);
    log_event(logmsg);

    pthread_mutex_lock(&(d->socketWriteMutex));
    sendAdjacentMap(d->user, d->map, d->width, d->height, d->x, d->y);
    pthread_mutex_unlock(&(d->socketWriteMutex));

    snprintf(logmsg, sizeof(logmsg), "[%s@%s] GAME: mappa iniziale inviata, attesa comandi", d->username, d->ip);
    log_event(logmsg);

    char buffer[256];
    while (!isTimeUp()) {
        int r = recv(d->user, buffer, sizeof(buffer) - 1, 0);
        if (r <= 0) {
            snprintf(logmsg, sizeof(logmsg), "[%s@%s] GAME: client disconnesso durante la partita", d->username, d->ip);
            log_event(logmsg);
            break;
        }
        buffer[r] = '\0';
        buffer[strcspn(buffer, "\r\n")] = 0;

        if (!strcmp(buffer, "exit")) {
            snprintf(logmsg, sizeof(logmsg), "[%s@%s] GAME: uscita volontaria", d->username, d->ip);
            log_event(logmsg);
            break;
        }

        snprintf(logmsg, sizeof(logmsg),
                 "[%s@%s] MOVE: '%.32s' (pos: %d,%d)", d->username, d->ip, buffer, d->x, d->y);
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
            snprintf(logmsg, sizeof(logmsg), "[%s@%s] GAME: uscita dalla mappa trovata", d->username, d->ip);
            log_event(logmsg);
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
                         "[%s@%s] ITEM: raccolto in (%d,%d), totale=%d", d->username, d->ip, d->x, d->y, d->collectedItems);
                log_event(logmsg);
            }

            snprintf(logmsg, sizeof(logmsg),
                     "[%s@%s] MOVE: nuova pos (%d,%d)", d->username, d->ip, d->x, d->y);
            log_event(logmsg);
        } else {
            snprintf(logmsg, sizeof(logmsg), "[%s@%s] MOVE: movimento bloccato (muro)", d->username, d->ip);
            log_event(logmsg);
        }
        pthread_mutex_unlock(&mutex);

        pthread_mutex_lock(&(d->socketWriteMutex));
        sendAdjacentMap(d->user, d->map, d->width, d->height, d->x, d->y);
        pthread_mutex_unlock(&(d->socketWriteMutex));
    }

    snprintf(logmsg, sizeof(logmsg), "[%s@%s] GAME: sessione terminata", d->username, d->ip);
    log_event(logmsg);
}

/* --------------------------------------------------------------------------
 * newUser  [thread]
 *
 * Entry point per ogni client. Il flusso e':
 *   1. authenticate()  -> legge il nome utente
 *   2. lobby           -> aspetta che tutti i giocatori siano pronti
 *   3. gaming()        -> ciclo di gioco
 *   4. writeScore()    -> salva il risultato su score.txt
 *   5. se e' l'ultimo thread a finire, calcola il vincitore
 *   6. invia W o L al client
 *   7. libera la memoria e chiude il socket
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
             "[%s@%s] LOBBY: in attesa (%d/%d)", username, d->ip, nReady, nClients);
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

    snprintf(logmsg, sizeof(logmsg), "[%s@%s] LOBBY: partita avviata, inizio gioco", username, d->ip);
    log_event(logmsg);

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
             "[%s@%s] ENDGAME: partita terminata (%d ancora in gioco)", username, d->ip, nReady);
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
    write(1, username, 100); /* per separare il log dal prompt del terminale */
    /* notifica W (vinto) o L (perso) al client */
    if (strcmp(winner, username) == 0) {
        snprintf(logmsg, sizeof(logmsg), "[%s@%s] RESULT: vincitore", username, d->ip);
        log_event(logmsg);
        send(d->user, "W", 1, 0);
    } else {
        snprintf(logmsg, sizeof(logmsg), "[%s@%s] RESULT: sconfitto", username, d->ip);
        log_event(logmsg);
        send(d->user, "L", 1, 0);
    }

    /* ----- CLEANUP ----- */
    snprintf(logmsg, sizeof(logmsg), "[%s@%s] CLEANUP: connessione chiusa", username, d->ip);
    log_event(logmsg);
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
 * Apre il log, azzera score.txt, crea il socket TCP sulla porta 8080 e
 * genera la mappa. Poi entra nel loop di select() che accetta nuovi client
 * finche' l'ultimo thread attivo non segnala la fine della partita
 * scrivendo sulla wakeup_pipe.
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
            strncpy(d->ip, inet_ntoa(cli.sin_addr), INET_ADDRSTRLEN - 1);
            d->ip[INET_ADDRSTRLEN - 1] = '\0';
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