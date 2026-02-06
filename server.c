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

struct data {
	int user;
	int log;
	char **map;
	int width;
	int height;
};


void authenticate(int user, int log) {
    char username[256];
    char registration[11] = "NEW USER: ";
    registration[10] = '\0';
    int readedbyte = recv(user, username, sizeof(username)-1,0);
    if(readedbyte < 0) { perror("lettura nickname"); exit(1); }
    username[readedbyte] = '\0';
    write(log, registration, sizeof(registration)-1);
    write(log, username, readedbyte);
    write(log, "\n", 1);
}
void gaming(int user, int log, char ** map, int width, int height) {
    int startingX;
    int startingY;

    do {
        startingX = rand() % height;
        startingY = rand() % width;
    } while(map[startingX][startingY] != PATH);

	sendMap(user, map, width, height, startingX, startingY);
	char buffer[256];
	while(1) {
		int readedbyte = recv(user, buffer, sizeof(buffer)-1,0);
		if(readedbyte < 0) { perror("lettura comando"); exit(1); }
		buffer[readedbyte] = '\0';
		if(strcmp(buffer, "exit") == 0) {
            printf("Client disconnesso\n");
            fflush(stdout);
            break;
        }
        if(strcmp(buffer, "W") == 0) { if(startingX > 0 && map[startingX-1][startingY] != WALL) startingX--; /* muovi su */ }
        else if(strcmp(buffer, "S") == 0) { if(startingX < height-1 && map[startingX+1][startingY] != WALL) startingX++; /* muovi giù */ }
        else if(strcmp(buffer, "A") == 0) { if(startingY > 0 && map[startingX][startingY-1] != WALL) startingY--; /* muovi sinistra */ }
        else if(strcmp(buffer, "D") == 0) { if(startingY < width-1 && map[startingX][startingY+1] != WALL) startingY++; /* muovi destra */ }
		else printf("Comando sconosciuto\n");
        write(log, "COMMAND: ", 9);
		write(log, buffer, readedbyte);
		write(log, "\n", 1);
        sendMap(user, map, width, height, startingX, startingY);
	}
}

void *newUser(void* data) {
    int client = ((struct data*)data)->user;
	int log = ((struct data*)data)->log;
	char **map = ((struct data*)data)->map;
	int width = ((struct data*)data)->width;
	int height = ((struct data*)data)->height;
    free(data);
    authenticate(client, log);
	gaming(client, log, map, width, height);
    close(client); // chiude il socket quando finisce il thread
    close(log);
    return NULL; // necessario per pthread_create
}

int main() {
    int filelog = open("filelog.txt", O_WRONLY | O_CREAT, 0644);
    if(filelog < 0) { perror("log"); return 1;}
    close(filelog);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(8080);
    srv.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr*) &srv, sizeof(srv)) < 0) {
        perror("bind");
        return 1;
    }

    printf("***SERVER ATTIVO***\n");
    fflush(stdout);

    if (listen(sockfd, 100) < 0) {
        perror("listen");
        return 1;
    }

    // Generiamo una mappa iniziale
    int width, height;
    char **map = generateMap(&width, &height);
    printf("Mappa generata %dx%d:\n", width, height);
    printMap(map, width, height);

    while(1) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int client = accept(sockfd, (struct sockaddr*)&cli, &len);
        if(client < 0) {
            perror("errore accept");
            continue;
        }

        pthread_t tid;
        struct data *pdata = malloc(sizeof(struct data));
		if(!pdata) {perror("malloc"); close(client); continue; }
		pdata->user = client;
		pdata->log = open("filelog.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
		if(pdata->log < 0) { perror("log"); free(pdata);
			close(client); continue; }
		pdata->map = map;
		pdata->width = width;
		pdata->height = height;
        if(pthread_create(&tid, NULL, newUser, pdata) != 0) {
            perror("Errore creazione thread");
            free(pdata);
            close(client);
            continue;
        }
		

        pthread_detach(tid);
        printf("Nuovo client connesso!\n");
        fflush(stdout);
    }

    freeMap(map, width);
    close(sockfd);
    return 0;
}
