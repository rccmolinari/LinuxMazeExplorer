#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include "map.h"

/* mutex per proteggere l'invio dei comandi e la variabile end */
pthread_mutex_t socketMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t endMutex = PTHREAD_MUTEX_INITIALIZER;
int end = 0; // 1 = fine partita

struct thread_args {
    int sockfd;
    int *width;
    int *height;
    int *x;
    int *y;
    char ***map; // puntatore alla mappa condivisa
};

/* lettura thread-safe di end */
int checkEnd() {
    pthread_mutex_lock(&endMutex);
    int r = end;
    pthread_mutex_unlock(&endMutex);
    return r;
}

/* invio comando al server */
void sendCommand(int sockfd, const char *command) {
    if(!command) return;
    if(strcmp(command, "exit") == 0) {
        close(sockfd);
        exit(0);
    }
    send(sockfd, command, strlen(command), 0);
}

/* thread che legge tutto dal server */
void *listenerThread(void *arg) {
    struct thread_args *targs = (struct thread_args*)arg;
    int effRows, effCols;
    char buf[1];

    while(1) {
        int n = recv(targs->sockfd, buf, 1, 0);
        if(n <= 0) break;

        char type = buf[0];

        if(type == 'M') {
            // consumato 'M'
            printf("\nSEI USCITO DALLA MAPPA, ATTENDI I RISULTATI!\n");
            pthread_mutex_lock(&endMutex);
            end = 1;
            pthread_mutex_unlock(&endMutex);
        }
        else if(type == 'E') {
            // consumato 'E', poi leggi W/L
            char result;
            recv(targs->sockfd, &result, 1, 0);
            system("clear");
            if(result == 'W') printf("\nVITTORIA!\n");
            else printf("\nSCONFITTA!\n");
            close(targs->sockfd);
            exit(0);
        }
        else if(type == 'B') {
            // aggiornamento mappa sfocata
            char **newMap = receiveMap(targs->sockfd, targs->width, targs->height,
                                       targs->x, targs->y, &effRows, &effCols);
            if(newMap) {
                system("clear");
                printf("\n--- AGGIORNAMENTO AMBIENTALE ---\n");
                printMap(newMap, effCols, effRows, *(targs->x), *(targs->y));

                // aggiorna mappa condivisa
                if(*(targs->map)) freeMap(*(targs->map), effRows);
                *(targs->map) = newMap;

                printf("Comando (W/A/S/D): ");
                fflush(stdout);
            }
        }
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    if(argc < 2) {
        printf("Uso: %s <IP_SERVER>\n", argv[0]);
        return 1;
    }

    signal(SIGINT, SIG_IGN);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(8080);
    inet_pton(AF_INET, argv[1], &srv.sin_addr);

    if(connect(sockfd, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("connect");
        return 1;
    }

    printf("***CLIENT: INSERISCI USERNAME***\n");
    char username[256];
    int n = read(0, username, sizeof(username)-1);
    username[n-1] = '\0'; // rimuovi \n
    send(sockfd, username, strlen(username), 0);

    int width, height, x, y, effRows, effCols;
    char **map = receiveMap(sockfd, &width, &height, &x, &y, &effRows, &effCols);

    system("clear");
    printf("Gioco iniziato!\n");
    printMap(map, effCols, effRows, x, y);

    // crea thread listener
    pthread_t tid;
    struct thread_args targs = {sockfd, &width, &height, &x, &y, &map};
    pthread_create(&tid, NULL, listenerThread, &targs);
    pthread_detach(tid);

    // loop principale: invio comandi
    while(!checkEnd()) {
        char cmd[16];
        printf("\nComando (W/A/S/D) | exit: ");
        fflush(stdout);
        int r = read(0, cmd, sizeof(cmd)-1);
        if(r <= 0) continue;
        cmd[r-1] = '\0';

        pthread_mutex_lock(&socketMutex);
        sendCommand(sockfd, cmd);
        pthread_mutex_unlock(&socketMutex);
    }

    // pulizia finale
    if(map) freeMap(map, effRows);
    while(1) sleep(1);
    return 0;
}
