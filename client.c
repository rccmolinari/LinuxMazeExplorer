#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include "map.h"

void sendCommand(int sockfd, const char * command) {
    if(command == NULL) return;
    if(strcmp(command, "exit") == 0) {
        close(sockfd);
        exit(0);
    }
    send(sockfd, command, strlen(command), 0);
}
int main(int argc, char* argv[]) {
    if(argc < 2) {
        printf("<program> + <ip address>\n");
        exit(1);
    }
    
    char * ipaddress = argv[1];
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
    
    printf("***CLIENT NOTIFICATION: INSERT USERNAME***\n"); 
    fflush(stdout);
    char username[256];
    int readedbyte = read(0, username, sizeof(username)-1);
    username[readedbyte] = '\0';
    send(sockfd, username, readedbyte, 0);
    
    int width, height, x, y;
    int effectiveCols, effectiveRows;
    char ** map = receiveMap(sockfd, &width, &height, &x, &y, &effectiveRows, &effectiveCols);
    
    int visited[height][width];
    for(int i=0; i<height; i++)
        for(int j=0; j<width; j++)
            visited[i][j] = 0;
    visited[x][y] = 1;
    
    printf("Gioco iniziato!\n");
    printMap(map, effectiveCols, effectiveRows, x, y);
    
    while (1) {
        char command[256];
        printf("\nComando (W/A/S/D): ");
        fflush(stdout);
        
        readedbyte = read(0, command, sizeof(command)-1);
        if(readedbyte <= 0) continue;
        command[readedbyte-1] = '\0';
        
        sendCommand(sockfd, command);
        
        // Ricevi risposta: può essere "WIN" (3 byte) o inizio mappa
        char response[16];
        int n = recv(sockfd, response, 3, MSG_PEEK);  // PEEK per non consumare
        if(n <= 0) {
            printf("Connessione chiusa\n");
            break;
        }
        
        response[n] = '\0';
        if(strcmp(response, "WIN") == 0) {
            recv(sockfd, response, 3, 0);  // Consuma il messaggio
            printf("\n🎉🎉🎉 VITTORIA! Sei riuscito ad uscire dalla mappa! 🎉🎉🎉\n");
            printf("Hai raccolto %d items durante il percorso!\n", 0);  // Se vuoi tracciare gli item
            break;
        }
        
        // Altrimenti ricevi la mappa normalmente
        map = receiveMap(sockfd, &width, &height, &x, &y, &effectiveRows, &effectiveCols);
        if(map == NULL) {
            printf("Connessione chiusa dal server\n");
            break;
        }
        
        visited[x][y] = 1;
        system("clear");
        adjVisit(width, height, x, y, visited);
        printMap(map, effectiveCols, effectiveRows, x, y);
    }
    
    freeMap(map, effectiveCols);
    close(sockfd);
    return 0;
}

