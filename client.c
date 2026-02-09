#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <pthread.h>
#include "map.h"

// Mutex per evitare che Main e Thread leggano contemporaneamente dallo stesso socket
pthread_mutex_t socketMutex = PTHREAD_MUTEX_INITIALIZER;

// Struttura per passare i dati al thread
struct thread_args {
    int sockfd;
    int *width;
    int *height;
    int *x;
    int *y;
};

void sendCommand(int sockfd, const char * command) {
    if(command == NULL) return;
    if(strcmp(command, "exit") == 0) {
        close(sockfd);
        exit(0);
    }
    send(sockfd, command, strlen(command), 0);
}

void *silentWaitBlurredMap(void *arg) {
    struct thread_args *args = (struct thread_args *)arg;
    char type;
    int effCols, effRows;

    while(1) {
        // Proviamo a bloccare il socket per controllare se c'è una mappa blurrata
        pthread_mutex_lock(&socketMutex);
        if(args->sockfd <= 0) {
            pthread_mutex_unlock(&socketMutex);
            break; // Se il socket è chiuso, termina il thread
        }
        // MSG_DONTWAIT impedisce al thread di bloccarsi qui se non c'è nulla
        int n = recv(args->sockfd, &type, sizeof(char), MSG_PEEK | MSG_DONTWAIT);
        
        if (n > 0 && type == 'B') {
            // Se è tipo 'B', consumiamo il messaggio e stampiamo
            char **blurredMap = receiveMap(args->sockfd, args->width, args->height, args->x, args->y, &effRows, &effCols);
            if (blurredMap) {
                // system("clear"); // Opzionale: pulisce lo schermo ad ogni evento
                system("clear");
                printf("\n--- AGGIORNAMENTO AMBIENTALE ---\n");
                printMap(blurredMap, effCols, effRows, *(args->x), *(args->y));
                printf("Comando (W/A/S/D): ");
                fflush(stdout);
                freeMap(blurredMap, effRows); // CORRETTO: usa le righe, non le colonne
            }
        }
        
        pthread_mutex_unlock(&socketMutex);
        
        // Pausa obbligatoria per non saturare la CPU e lasciare spazio al Main
        usleep(200000); 
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    if(argc < 2) {
        printf("<program> + <ip address>\n");
        exit(1);
    }
    
    char *ipaddress = argv[1];
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }
    
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(8080);
    inet_pton(AF_INET, ipaddress, &srv.sin_addr);
    
    if (connect(sockfd, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("connect");
        return 1;
    }
    
    printf("***CLIENT: INSERISCI USERNAME***\n"); 
    char username[256];
    int readedbyte = read(0, username, sizeof(username)-1);
    username[readedbyte] = '\0';
    send(sockfd, username, readedbyte, 0);
    
    int width, height, x, y;
    int effectiveCols, effectiveRows;
    
    // Primo aggiornamento mappa (protetto da mutex)
    pthread_mutex_lock(&socketMutex);
    char **map = receiveMap(sockfd, &width, &height, &x, &y, &effectiveRows, &effectiveCols);
    pthread_mutex_unlock(&socketMutex);

    // Preparazione thread per aggiornamenti "Blurred" asincroni
    pthread_t tid;
    struct thread_args t_args = {sockfd, &width, &height, &x, &y};
    pthread_create(&tid, NULL, silentWaitBlurredMap, &t_args);
    pthread_detach(tid);
    
    printf("Gioco iniziato!\n");
    printMap(map, effectiveCols, effectiveRows, x, y);
    
    while (1) {
        char command[256];
        printf("\nComando (W/A/S/D): ");
        fflush(stdout);
        
        readedbyte = read(0, command, sizeof(command)-1);
        if(readedbyte <= 0) continue;
        command[readedbyte-1] = '\0';
        
        // Invio comando (protetto da mutex)
        pthread_mutex_lock(&socketMutex);
        sendCommand(sockfd, command);
        
        char response[16];
        int n = recv(sockfd, response, 3, MSG_PEEK);
        
        if(n > 0) {
            response[n] = '\0';
            if(strcmp(response, "WIN") == 0) {
                recv(sockfd, response, 3, 0);
                printf("\n🎉 VITTORIA! 🎉\n");
                pthread_mutex_unlock(&socketMutex);
                break;
            }
            
            // Ricezione normale della mappa
            char **new_map = receiveMap(sockfd, &width, &height, &x, &y, &effectiveRows, &effectiveCols);
            if(new_map != NULL) {
                freeMap(map, effectiveRows); // Libera la vecchia
                map = new_map;
            }
        }
        pthread_mutex_unlock(&socketMutex);
        system("clear");
        printMap(map, effectiveCols, effectiveRows, x, y);
    }
    
    freeMap(map, effectiveRows);
    close(sockfd);
    return 0;
}