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
#include "map.h"

#define SECONDS_TO_BLUR 10
#define TIMER 5

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t scoreMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t scoreCond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t lobbyMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t lobbyCond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t timerMutex = PTHREAD_MUTEX_INITIALIZER;

int nReady = 0;
int nClients = 0;
int gameStarted = 0;
int timeUp = 0;

int wakeup_pipe[2];          // <<< NUOVO: pipe per svegliare il thread principale
pthread_t timerTid;

struct data {
    int user;
    int log;
    char **map;
    int width;
    int height;
    int x;
    int y;
    int **visited;
    pthread_mutex_t socketWriteMutex;
};

// THREAD ASINCRONO PER LA MAPPA SFUOCATA
void *asyncSendBlurredMap(void *arg) {
    struct data *d = (struct data *)arg;

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
    }
    return NULL;
}

void writeScore(const char *username, int score) {
    pthread_mutex_lock(&scoreMutex);
    int scoreFile = open("score.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (scoreFile >= 0) {
        char buffer[512];
        int len = sprintf(buffer, "%s %d\n", username, score);
        write(scoreFile, buffer, len);
        close(scoreFile);
    }
    pthread_mutex_unlock(&scoreMutex);
}

void timer() {
    sleep(TIMER);
    pthread_mutex_lock(&timerMutex);
    timeUp = 1;
    pthread_mutex_unlock(&timerMutex);
}

int isTimeUp() {
    pthread_mutex_lock(&timerMutex);
    int r = timeUp;
    pthread_mutex_unlock(&timerMutex);
    return r;
}

void authenticate(struct data *d, char *usernamesrc) {
    char username[256];
    int readedbyte = recv(d->user, username, sizeof(username)-1, 0);
    if (readedbyte <= 0) return;
    username[readedbyte] = '\0';
    username[strcspn(username, "\r\n")] = 0;
    strcpy(usernamesrc, username);

    char registration[] = "NEW USER: ";
    write(d->log, registration, strlen(registration));
    write(d->log, username, strlen(username));
    write(d->log, "\n", 1);
}

void gaming(struct data *d, char *username) {
    int collected = 0;

    do {
        d->x = rand() % d->height;
        d->y = rand() % d->width;
    } while (d->map[d->x][d->y] != PATH);

    d->visited[d->x][d->y] = 1;
    adjVisit(d->width, d->height, d->x, d->y, d->visited);

    pthread_mutex_lock(&(d->socketWriteMutex));
    sendAdjacentMap(d->user, d->map, d->width, d->height, d->x, d->y);
    pthread_mutex_unlock(&(d->socketWriteMutex));

    char buffer[256];
    while (!isTimeUp()) {
        int r = recv(d->user, buffer, sizeof(buffer)-1, 0);
        if (r <= 0) break;
        buffer[r] = '\0';
        buffer[strcspn(buffer, "\r\n")] = 0;

        if (!strcmp(buffer, "exit")) break;

        int nextX = d->x;
        int nextY = d->y;
        int win = 0;

        if (!strcmp(buffer, "W")) { if (d->x==0) win=1; else nextX--; }
        if (!strcmp(buffer, "S")) { if (d->x==d->height-1) win=1; else nextX++; }
        if (!strcmp(buffer, "A")) { if (d->y==0) win=1; else nextY--; }
        if (!strcmp(buffer, "D")) { if (d->y==d->width-1) win=1; else nextY++; }

        if (win) {
            pthread_mutex_lock(&(d->socketWriteMutex));
            send(d->user, "WIN", 3, 0);
            pthread_mutex_unlock(&(d->socketWriteMutex));
            writeScore(username, collected);
            break;
        }

        pthread_mutex_lock(&mutex);
        if (d->map[nextX][nextY] != WALL) {
            d->x = nextX;
            d->y = nextY;
            d->visited[d->x][d->y] = 1;
            adjVisit(d->width, d->height, d->x, d->y, d->visited);

            if (d->map[d->x][d->y] == ITEM) {
                d->map[d->x][d->y] = PATH;
                collected++;
            }
        }
        pthread_mutex_unlock(&mutex);

        pthread_mutex_lock(&(d->socketWriteMutex));
        sendAdjacentMap(d->user, d->map, d->width, d->height, d->x, d->y);
        pthread_mutex_unlock(&(d->socketWriteMutex));
    }
}

void *newUser(void *arg) {
    struct data *d = (struct data*)arg;
    char username[256];

    authenticate(d, username);

    pthread_mutex_lock(&lobbyMutex);
    nReady++;
    if (nReady == nClients) {
        gameStarted = 1;
        pthread_create(&timerTid, NULL, (void*)timer, NULL);
        pthread_cond_broadcast(&lobbyCond);
    } else {
        while (!gameStarted)
            pthread_cond_wait(&lobbyCond, &lobbyMutex);
    }
    pthread_mutex_unlock(&lobbyMutex);

    pthread_t blurTid;
    pthread_create(&blurTid, NULL, asyncSendBlurredMap, d);
    pthread_detach(blurTid);

    gaming(d, username);

    send(d->user, "E", 1, 0);
    close(d->user);
    close(d->log);

    for (int i=0;i<d->height;i++) free(d->visited[i]);
    free(d->visited);
    pthread_mutex_destroy(&(d->socketWriteMutex));
    free(d);
    nClients--;
    if(nClients == 0) {
        write(wakeup_pipe[1], "X", 1);
        close(wakeup_pipe[1]);
    }
    

    return NULL;
}

int main() {
    srand(time(NULL));

    pipe(wakeup_pipe); // <<< CREA PIPE PER SVEGLIARE SELECT

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(8080);
    srv.sin_addr.s_addr = INADDR_ANY;

    bind(sockfd, (struct sockaddr*)&srv, sizeof(srv));
    listen(sockfd, 100);

    int w,h;
    char **map = generateMap(&w,&h);
    printf("*** SERVER AVVIATO ***\n");

    while (1) {

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        FD_SET(wakeup_pipe[0], &rfds);

        int maxfd = (sockfd > wakeup_pipe[0]) ? sockfd : wakeup_pipe[0];

        select(maxfd+1, &rfds, NULL, NULL, NULL);

        if (FD_ISSET(wakeup_pipe[0], &rfds)) {
            char c;
            read(wakeup_pipe[0], &c, 1);
            printf("Chiusura server dal thread figlio.\n");
            close(wakeup_pipe[0]);
            break;
        }

        if (FD_ISSET(sockfd, &rfds)) {
            struct sockaddr_in cli;
            socklen_t clen = sizeof(cli);
            int cfd = accept(sockfd, (struct sockaddr*)&cli, &clen);

            struct data *d = malloc(sizeof(struct data));
            d->user = cfd;
            d->log = open("filelog.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
            d->map = map;
            d->width = w;
            d->height = h;

            pthread_mutex_init(&(d->socketWriteMutex), NULL);

            d->visited = malloc(h*sizeof(int*));
            for (int i=0;i<h;i++)
                d->visited[i] = calloc(w,sizeof(int));

            pthread_mutex_lock(&lobbyMutex);
            nClients++;
            pthread_mutex_unlock(&lobbyMutex);

            pthread_t tid;
            pthread_create(&tid, NULL, newUser, d);
            pthread_detach(tid);

            printf("Nuovo client connesso: %d totali\n", nClients);
        }
    }

    close(sockfd);
    return 0;
}
