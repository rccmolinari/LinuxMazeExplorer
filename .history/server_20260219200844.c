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

/* secondi tra un invio di nebbia e il successivo */
#define SECONDS_TO_BLUR 40

/* durata della partita in secondi */
#define TIMER 100

/*
 * mutex        -> protegge la mappa durante gli spostamenti
 * scoreMutex   -> scrittura serializzata su score.txt
 * scoreCond    -> usata con scoreChanging per evitare scritture sovrapposte
 * lobbyMutex   -> protegge nReady, gameStarted e compagnia
 * lobbyCond    -> i thread aspettano qui finche' la partita non parte
 * timerMutex   -> protegge timeUp
 * logMutex     -> evita che le righe di log si mescolino tra thread
 */
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t scoreMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t scoreCond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t lobbyMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t lobbyCond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t timerMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t logMutex = PTHREAD_MUTEX_INITIALIZER;

int nReady = 0;        /* client in lobby */
int nClients = 0;      /* client connessi in totale */
int gameStarted = 0;   /* flag: partita avviata */
int timeUp = 0;        /* flag: timer scaduto */
int scoreChanging = 0; /* 1 mentre qualcuno scrive score.txt */
int serverRunning = 1; /* diventa 0 con Ctrl+C */
int nExit = 0;
/* fd del log, aperto nel main e usato da tutti i thread */
int gLogFd = -1;

/* pipe per svegliare la select() del main a fine partita */
int wakeup_pipe[2];

pthread_t timerTid;

/*
 * Struttura per ogni client connesso.
 * Viene allocata nel main e passata al thread newUser.
 */
struct data {
    int user;              /* socket del client */
    char ip[INET_ADDRSTRLEN]; /* ip in formato stringa */
    char username[256];    /* nome utente */
    char **map;            /* mappa condivisa */
    int width;
    int height;
    int x;                 /* posizione corrente (riga) */
    int y;                 /* posizione corrente (colonna) */
    int **visited;         /* celle visitate, usate per la nebbia */
    int collectedItems;    /* oggetti raccolti */
    int exitFlag;          /* 1 se ha trovato l'uscita */
    int gameOver;          /* 1 quando la partita e' finita per qualsiasi motivo */
    pthread_mutex_t socketWriteMutex; /* protegge le send() */
};

/*
 * log_event
 * Scrive una riga su gLogFd nel formato [YYYY-MM-DD HH:MM:SS] <msg>.
 * Il logMutex evita che i messaggi di thread diversi si sovrappongano.
 */
static void log_event(const char *msg) {
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

/*
 * log_error
 * Scrive sul log l'errore corrente (errno) con il contesto indicato.
 */
static void log_error(const char *context) {
    char errbuf[256];
    char msg[512];
    int len = 0;
    strerror_r(errno, errbuf, sizeof(errbuf));
    memcpy(msg, "ERROR [", 7); len += 7;
    memcpy(msg + len, context, strlen(context)); len += strlen(context);
    memcpy(msg + len, "]: ", 3); len += 3;
    memcpy(msg + len, errbuf, strlen(errbuf)); len += strlen(errbuf);
    msg[len] = '\0';
    log_event(msg);
}

/*
 * printWinnerWithPipe
 * Calcola il vincitore leggendo score.txt tramite una pipeline shell
 * eseguita in un processo figlio. Il figlio redireziona stdout sulla pipe
 * e lancia sort | head | awk. Il padre legge il nome e lo copia in winner.
 * Vince prima chi ha trovato l'uscita, poi chi ha piu' oggetti.
 */
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
        /* figlio: stdout sulla pipe, poi esegue il comando */
        close(fd[0]);
        dup2(fd[1], STDOUT_FILENO);
        close(fd[1]);
        execlp("sh", "sh", "-c",
               "sort -k3,3nr -k2,2nr score.txt | head -n1 | awk '{print $1}'",
               NULL);
        exit(1);
    } else {
        /* padre: legge il risultato */
        close(fd[1]);
        char usr[256];
        int n = read(fd[0], usr, sizeof(usr) - 1);
        if (n > 0) {
            usr[n] = '\0';
            usr[strcspn(usr, "\r\n")] = 0;
            strcpy(winner, usr);
        } else {
            log_event("WINNER: score.txt vuoto, nessun vincitore");
            winner[0] = '\0';
        }
        close(fd[0]);
        wait(NULL); /* evita zombie */
    }
}

/* lettura thread-safe di timeUp */
int isTimeUp() {
    pthread_mutex_lock(&timerMutex);
    int r = timeUp;
    pthread_mutex_unlock(&timerMutex);
    return r;
}

/*
 * asyncSendBlurredMap  [thread]
 * Invia la mappa con nebbia ogni SECONDS_TO_BLUR secondi.
 * Si ferma quando gaming() imposta gameOver=1, che succede in tutti i
 * casi di uscita: timeout, uscita volontaria, bordo raggiunto, disconnessione.
 */
void *asyncSendBlurredMap(void *arg) {
    struct data *d = (struct data *)arg;

    /* aspetta che la partita inizi */
    pthread_mutex_lock(&lobbyMutex);
    while (!gameStarted)
        pthread_cond_wait(&lobbyCond, &lobbyMutex);
    pthread_mutex_unlock(&lobbyMutex);

    while (!d->gameOver) {
        sleep(SECONDS_TO_BLUR);
        if (d->gameOver) break;
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

/*
 * writeScore
 * Aggiunge una riga a score.txt: <username> <oggetti> <exit_flag>.
 * scoreChanging serializza gli accessi tra thread concorrenti.
 */
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

/*
 * timer  [thread]
 * Aspetta TIMER secondi poi imposta timeUp=1.
 * Parte solo quando tutti i giocatori sono in lobby.
 */
void timer() {
    log_event("TIMER: countdown avviato");
    sleep(TIMER);
    pthread_mutex_lock(&timerMutex);
    timeUp = 1;
    pthread_mutex_unlock(&timerMutex);
    log_event("TIMER: tempo scaduto, fine partita");
}

/*
 * authenticate
 * Legge il nome utente dal client e lo salva in usernamesrc e d->username.
 */
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

/*
 * gaming
 * Ciclo di gioco per un client. Spawn casuale su PATH, poi legge comandi
 * in loop: W/A/S/D per muoversi, exit per uscire. Se tocca il bordo ha
 * trovato l'uscita. Raccoglie gli item camminandoci sopra.
 * Al termine imposta gameOver=1 per fermare il thread nebbia.
 */
void gaming(struct data *d) {
    d->collectedItems = 0;
    d->exitFlag = 0;
    d->gameOver = 0;

    /* spawn casuale su una cella percorribile */
    do {
        d->x = rand() % d->height;
        d->y = rand() % d->width;
    } while (d->map[d->x][d->y] != PATH);

    d->visited[d->x][d->y] = 1;
    adjVisit(d->width, d->height, d->x, d->y, d->visited);

    char logmsg[512];
    snprintf(logmsg, sizeof(logmsg), "[%s@%s] GAME: spawn in (%d,%d)", d->username, d->ip, d->x, d->y);
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
            snprintf(logmsg, sizeof(logmsg), "[%s@%s] GAME: client disconnesso", d->username, d->ip);
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

        snprintf(logmsg, sizeof(logmsg), "[%s@%s] MOVE: '%.32s' (pos: %d,%d)", d->username, d->ip, buffer, d->x, d->y);
        log_event(logmsg);

        int nextX = d->x;
        int nextY = d->y;
        int win = 0;

        /* controlla se esce dai bordi */
        if      (!strcmp(buffer, "W")) { if (d->x == 0)             win = 1; else nextX--; }
        else if (!strcmp(buffer, "S")) { if (d->x == d->height - 1) win = 1; else nextX++; }
        else if (!strcmp(buffer, "A")) { if (d->y == 0)             win = 1; else nextY--; }
        else if (!strcmp(buffer, "D")) { if (d->y == d->width - 1)  win = 1; else nextY++; }

        if (win) {
            d->exitFlag = 1;
            snprintf(logmsg, sizeof(logmsg), "[%s@%s] GAME: uscita trovata", d->username, d->ip);
            log_event(logmsg);
            pthread_mutex_lock(&(d->socketWriteMutex));
            send(d->user, "M", 1, 0);
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
                d->map[d->x][d->y] = PATH; /* rimuove l'item dalla mappa */
                d->collectedItems++;
                snprintf(logmsg, sizeof(logmsg), "[%s@%s] ITEM: raccolto in (%d,%d), totale=%d",
                         d->username, d->ip, d->x, d->y, d->collectedItems);
                log_event(logmsg);
            }

            snprintf(logmsg, sizeof(logmsg), "[%s@%s] MOVE: nuova pos (%d,%d)", d->username, d->ip, d->x, d->y);
            log_event(logmsg);
        } else {
            snprintf(logmsg, sizeof(logmsg), "[%s@%s] MOVE: bloccato dal muro", d->username, d->ip);
            log_event(logmsg);
        }
        pthread_mutex_unlock(&mutex);

        pthread_mutex_lock(&(d->socketWriteMutex));
        sendAdjacentMap(d->user, d->map, d->width, d->height, d->x, d->y);
        pthread_mutex_unlock(&(d->socketWriteMutex));
    }

    /* segnala al thread nebbia che puo' fermarsi */
    d->gameOver = 1;

    snprintf(logmsg, sizeof(logmsg), "[%s@%s] GAME: sessione terminata", d->username, d->ip);
    log_event(logmsg);
}

/*
 * newUser  [thread]
 * Entry point per ogni client:
 *   1. authenticate  -> legge il nome utente
 *   2. lobby         -> aspetta che tutti siano pronti
 *   3. gaming        -> ciclo di gioco
 *   4. writeScore    -> salva il risultato
 *   5. ultimo thread -> calcola il vincitore e sveglia il main
 *   6. invia W o L
 *   7. cleanup
 */
void *newUser(void *arg) {
    struct data *d = (struct data *)arg;
    char username[256] = {0};
    char logmsg[512];

    authenticate(d, username);

    /* lobby: aspetta che tutti i client siano pronti */
    pthread_mutex_lock(&lobbyMutex);
    nReady++;

    snprintf(logmsg, sizeof(logmsg), "[%s@%s] LOBBY: in attesa (%d/%d)", username, d->ip, nReady, nClients);
    log_event(logmsg);

    if (nReady == nClients) {
        gameStarted = 1;
        log_event("LOBBY: tutti pronti, partita in avvio");
        pthread_create(&timerTid, NULL, (void *)timer, NULL);
        pthread_cond_broadcast(&lobbyCond);
    } else {
        while (!gameStarted)
            pthread_cond_wait(&lobbyCond, &lobbyMutex);
    }
    pthread_mutex_unlock(&lobbyMutex);

    snprintf(logmsg, sizeof(logmsg), "[%s@%s] LOBBY: partita avviata, inizio gioco", username, d->ip);
    log_event(logmsg);

    /* thread nebbia in background */
    pthread_t blurTid;
    pthread_create(&blurTid, NULL, asyncSendBlurredMap, d);
    pthread_detach(blurTid);

    gaming(d);
    pthread_mutex_lock(&lobbyMutex);
    nExit = nExit + 1;
    pthread_mutex_unlock(&lobbyMutex);
    writeScore(username, d);

    send(d->user, "E", 1, 0); /* avvisa il client che la partita e' finita */
    pthread_mutex_lock(&lobbyMutex);

    snprintf(logmsg, sizeof(logmsg), "[%s@%s] ENDGAME: terminato (%d ancora in gioco)", username, d->ip, nReady);
    log_event(logmsg);

    /*
     * mutex e cond locali per aspettare il calcolo del vincitore.
     * Solo l'ultimo thread (nReady==0) fa il calcolo e segnala agli altri.
     * Funziona perche' a quel punto gli altri thread sono fermi qui ad aspettare.
     */
    int winnerCalculated = 0;
    pthread_mutex_t winnerMutex;
    pthread_mutex_init(&winnerMutex, NULL);
    pthread_cond_t winnerCond = PTHREAD_COND_INITIALIZER;
    char winner[256] = {0};

    if (nExit == nClients) {
        log_event("ENDGAME: tutti finiti, calcolo vincitore");
        printWinnerWithPipe(winner);

        snprintf(logmsg, sizeof(logmsg), "ENDGAME: vincitore -> '%s'", winner);
        log_event(logmsg);

        pthread_mutex_lock(&winnerMutex);
        winnerCalculated = 1;
        pthread_cond_signal(&winnerCond);
        pthread_mutex_unlock(&winnerMutex);

        write(wakeup_pipe[1], "X", 1);
        close(wakeup_pipe[1]);
    }
    pthread_mutex_unlock(&lobbyMutex);

    pthread_mutex_lock(&winnerMutex);
    while (winnerCalculated == 0)
        pthread_cond_wait(&winnerCond, &winnerMutex);
    pthread_mutex_unlock(&winnerMutex);

    if (strcmp(winner, username) == 0) {
        snprintf(logmsg, sizeof(logmsg), "[%s@%s] RESULT: vincitore", username, d->ip);
        log_event(logmsg);
        send(d->user, "W", 1, 0);
    } else {
        snprintf(logmsg, sizeof(logmsg), "[%s@%s] RESULT: sconfitto", username, d->ip);
        log_event(logmsg);
        send(d->user, "L", 1, 0);
    }

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

/* SIGINT: imposta serverRunning=0 per uscire dal loop */
void sigint_handler(int signum) {
    serverRunning = 0;
}

/*
 * reset_state
 * Riporta le variabili globali ai valori iniziali tra una partita e l'altra.
 */
static void reset_state() {
    pthread_mutex_lock(&lobbyMutex);
    nReady = 0;
    nClients = 0;
    gameStarted = 0;
    pthread_mutex_unlock(&lobbyMutex);

    pthread_mutex_lock(&timerMutex);
    timeUp = 0;
    pthread_mutex_unlock(&timerMutex);

    pthread_mutex_lock(&scoreMutex);
    scoreChanging = 0;
    pthread_mutex_unlock(&scoreMutex);
}

/*
 * main
 * Apre il log, crea il socket su 8080 e gestisce una partita alla volta
 * in un loop esterno. Tra una partita e l'altra resetta lo stato e rigenera
 * la mappa. Si spegne solo con Ctrl+C.
 */
int main() {
    srand(time(NULL));
    signal(SIGINT, sigint_handler);

    gLogFd = open("filelog.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (gLogFd < 0) {
        const char *err = "FATAL: impossibile aprire filelog.txt\n";
        write(STDERR_FILENO, err, strlen(err));
        exit(1);
    }

    log_event("SERVER: avvio in corso");

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        log_error("socket");
        exit(1);
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(8080);
    srv.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        log_error("bind");
        exit(1);
    }
    listen(sockfd, 100);

    log_event("SERVER: in ascolto sulla porta 8080");

    /* loop esterno: una iterazione = una partita */
    while (serverRunning) {

        close(open("score.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644));
        if (pipe(wakeup_pipe) == -1) {
            log_error("pipe wakeup_pipe");
            break;
        }

        int w, h;
        char **map = generateMap(&w, &h);
        log_event("SERVER: nuova partita pronta, in attesa di giocatori");

        /* loop interno: accetta client finche' la partita non finisce */
        while (serverRunning) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sockfd, &rfds);
            FD_SET(wakeup_pipe[0], &rfds);

            int maxfd = (sockfd > wakeup_pipe[0]) ? sockfd : wakeup_pipe[0];

            if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
                if (!serverRunning) break;
                log_error("select");
                break;
            }

            /* fine partita segnalata dalla pipe */
            if (FD_ISSET(wakeup_pipe[0], &rfds)) {
                char c;
                read(wakeup_pipe[0], &c, 1);
                log_event("SERVER: partita terminata, reset in corso");
                close(wakeup_pipe[0]);
                break;
            }

            if (FD_ISSET(sockfd, &rfds)) {
                struct sockaddr_in cli;
                socklen_t clen = sizeof(cli);
                int cfd = accept(sockfd, (struct sockaddr *)&cli, &clen);
                if (cfd < 0) {
                    if (!serverRunning) break;
                    log_error("accept");
                    continue;
                }

                struct data *d = malloc(sizeof(struct data));
                d->user = cfd;
                strncpy(d->ip, inet_ntoa(cli.sin_addr), INET_ADDRSTRLEN - 1);
                d->ip[INET_ADDRSTRLEN - 1] = '\0';
                d->map = map;
                d->width = w;
                d->height = h;
                d->collectedItems = 0;
                d->exitFlag = 0;
                d->gameOver = 0;

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
                snprintf(logmsg, sizeof(logmsg), "SERVER: connessione da %s (client #%d)",
                         inet_ntoa(cli.sin_addr), nClients);
                log_event(logmsg);
            }
        }

        reset_state();
    }

    log_event("SERVER: spegnimento in corso");
    close(sockfd);
    close(gLogFd);
    return 0;
}