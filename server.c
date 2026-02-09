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

// Mutex per la protezione delle risorse condivise
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t scoreMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t scoreCond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t lobbyMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t lobbyCond = PTHREAD_COND_INITIALIZER;

int mapChanging = 0;
int scoreChanging = 0;
int nReady = 0;
int nClients = 0;
int gameStarted = 0; // Flag per segnalare l'inizio effettivo

struct data {
    int user;
    int log;
    char **map;
    int width;
    int height;
    int x;          // Coordinata X attuale
    int y;          // Coordinata Y attuale
    int **visited;  // Matrice delle celle visitate
    pthread_mutex_t socketWriteMutex; // Protegge il socket dai conflitti tra thread gaming e asyncBlur
};

// Funzione eseguita dal thread asincrono per ogni client
void *asyncSendBlurredMap(void *arg) {
    struct data *d = (struct data *)arg;

    // Attesa sincronizzata: il thread asincrono parte solo quando la lobby è piena
    pthread_mutex_lock(&lobbyMutex);
    while (!gameStarted) {
        pthread_cond_wait(&lobbyCond, &lobbyMutex);
    }
    pthread_mutex_unlock(&lobbyMutex);

    while (1) {
        sleep(SECONDS_TO_BLUR);
        if(d->user <= 0) break; // Se il socket è chiuso, termina il thread
        // Usiamo un mutex specifico per il socket dell'utente per non mischiare
        // i byte della mappa adiacente con quelli della mappa sfocata
        pthread_mutex_lock(&(d->socketWriteMutex));
        // Passiamo i dati correnti (x, y e visited vengono aggiornati dal thread gaming)
        sendBlurredMap(d->user, d->map, d->width, d->height, d->x, d->y, d->visited);
        pthread_mutex_unlock(&(d->socketWriteMutex));
    }
    return NULL;
}

void writeScore(const char *username, int score) {
    pthread_mutex_lock(&scoreMutex);
    while (scoreChanging) {
        pthread_cond_wait(&scoreCond, &scoreMutex);
    }
    scoreChanging = 1;
    
    int scoreFile = open("score.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (scoreFile >= 0) {
        char buffer[512];
        int len = sprintf(buffer, "%s %d\n", username, score);
        write(scoreFile, buffer, len);
        close(scoreFile);
    }
    
    scoreChanging = 0;
    pthread_cond_signal(&scoreCond);
    pthread_mutex_unlock(&scoreMutex);
}

void authenticate(struct data *d, char *usernamesrc) {
    char username[256];
    char registration[] = "NEW USER: ";
    int readedbyte = recv(d->user, username, sizeof(username) - 1, 0);
    if (readedbyte <= 0) {
        return;
    }
    username[readedbyte] = '\0';
    // Rimuove eventuale newline
    username[strcspn(username, "\r\n")] = 0;

    write(d->log, registration, strlen(registration));
    write(d->log, username, strlen(username));
    write(d->log, "\n", 1);
    strcpy(usernamesrc, username);
}

void gaming(struct data *d, char *username) {
    int collectedItems = 0;

    // Posizionamento iniziale
    do {
        d->x = rand() % d->height;
        d->y = rand() % d->width;
    } while (d->map[d->x][d->y] != PATH);

    d->visited[d->x][d->y] = 1;
    adjVisit(d->width, d->height, d->x, d->y, d->visited);

    // Primo invio della mappa adiacente
    pthread_mutex_lock(&(d->socketWriteMutex));
    sendAdjacentMap(d->user, d->map, d->width, d->height, d->x, d->y);
    pthread_mutex_unlock(&(d->socketWriteMutex));

    char buffer[256];
    while (1) {
        int readedbyte = recv(d->user, buffer, sizeof(buffer) - 1, 0);
        if (readedbyte <= 0) break;
        buffer[readedbyte] = '\0';
        buffer[strcspn(buffer, "\r\n")] = 0;

        if (strcmp(buffer, "exit") == 0) break;

        write(d->log, "COMMAND: ", 9);
        write(d->log, buffer, strlen(buffer));
        write(d->log, "\n", 1);

        int nextX = d->x;
        int nextY = d->y;
        int win = 0;

        // Logica di movimento e controllo bordi per vittoria
        if (strcmp(buffer, "W") == 0) { if (d->x == 0) win = 1; else nextX--; }
        else if (strcmp(buffer, "S") == 0) { if (d->x == d->height - 1) win = 1; else nextX++; }
        else if (strcmp(buffer, "A") == 0) { if (d->y == 0) win = 1; else nextY--; }
        else if (strcmp(buffer, "D") == 0) { if (d->y == d->width - 1) win = 1; else nextY++; }

        if (win) {
            pthread_mutex_lock(&(d->socketWriteMutex));
            send(d->user, "WIN", 3, 0);
            pthread_mutex_unlock(&(d->socketWriteMutex));
            writeScore(username, collectedItems);
            break;
        }

        // Se il movimento è verso un percorso o un oggetto, aggiorna
        pthread_mutex_lock(&mutex);
        if (d->map[nextX][nextY] != WALL) {
            d->x = nextX;
            d->y = nextY;
            d->visited[d->x][d->y] = 1;
            adjVisit(d->width, d->height, d->x, d->y, d->visited);

            if (d->map[d->x][d->y] == ITEM) {
                d->map[d->x][d->y] = PATH;
                collectedItems++;
            }
        }
        pthread_mutex_unlock(&mutex);

        // Invio mappa aggiornata
        pthread_mutex_lock(&(d->socketWriteMutex));
        sendAdjacentMap(d->user, d->map, d->width, d->height, d->x, d->y);
        pthread_mutex_unlock(&(d->socketWriteMutex));
    }
}

void *newUser(void* arg) {
    struct data *d = (struct data*)arg;
    char username[256];

    authenticate(d, username);

    // Gestione Lobby
    pthread_mutex_lock(&lobbyMutex);
    nReady++;
    printf("Utenti pronti: %d/%d\n", nReady, nClients);
    if (nReady == nClients) {
        gameStarted = 1;
        pthread_cond_broadcast(&lobbyCond);
    } else {
        while (!gameStarted) {
            pthread_cond_wait(&lobbyCond, &lobbyMutex);
        }
    }
    pthread_mutex_unlock(&lobbyMutex);

    // Avvio del thread per la nebbia asincrona
    pthread_t blurTid;
    pthread_create(&blurTid, NULL, asyncSendBlurredMap, d);
    pthread_detach(blurTid);

    gaming(d, username);

    // Pulizia risorse per il client specifico
    close(d->user);
    close(d->log);
    for (int i = 0; i < d->height; i++) free(d->visited[i]);
    free(d->visited);
    pthread_mutex_destroy(&(d->socketWriteMutex));
    free(d);
    
    return NULL;
}

int main() {
    srand(time(NULL));
    int filelog_init = open("filelog.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    close(filelog_init);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(8080);
    srv.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("bind"); exit(1);
    }
    listen(sockfd, 100);

    int w, h;
    char **map = generateMap(&w, &h);
    printf("***SERVER ATTIVO***\nMappa %dx%d generata.\n", w, h);
    printMap(map, w, h, -1, -1); // Stampa la mappa completa per debug
    while (1) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int client_fd = accept(sockfd, (struct sockaddr*)&cli, &len);
        
        struct data *pdata = malloc(sizeof(struct data));
        pdata->user = client_fd;
        pdata->log = open("filelog.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
        pdata->map = map;
        pdata->width = w;
        pdata->height = h;
        pdata->x = 0;
        pdata->y = 0;
        pthread_mutex_init(&(pdata->socketWriteMutex), NULL);

        // Inizializzazione matrice visited
        pdata->visited = malloc(h * sizeof(int*));
        for (int i = 0; i < h; i++) {
            pdata->visited[i] = calloc(w, sizeof(int));
        }

        pthread_mutex_lock(&lobbyMutex);
        nClients++;
        pthread_mutex_unlock(&lobbyMutex);

        pthread_t tid;
        pthread_create(&tid, NULL, newUser, pdata);
        pthread_detach(tid);
        printf("Nuovo client connesso. Totale: %d\n", nClients);
    }

    return 0;
}