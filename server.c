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
struct data {
	int user;
	int log;
	char **map;
	int width;
	int height;
};


void writeScore(const char * username, int score) {
    pthread_mutex_lock(&scoreMutex);
    while(scoreChanging) {
        pthread_cond_wait(&scoreCond, &scoreMutex);
    }
    scoreChanging = 1;
    int scoreFile = open("score.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if(scoreFile < 0) { perror("score"); return; }
    write(scoreFile, username, strlen(username));
    write(scoreFile, " ", 1);
    write(scoreFile, &score, sizeof(int));
    write(scoreFile, "\n", 1);
    scoreChanging = 0;
    close(scoreFile);
    pthread_cond_signal(&scoreCond);
    pthread_mutex_unlock(&scoreMutex);
}
void authenticate(int user, int log, char * usernamesrc) {
    char username[256];
    char registration[11] = "NEW USER: ";
    registration[10] = '\0';
    int readedbyte = recv(user, username, sizeof(username)-1,0);
    if(readedbyte < 0) { perror("lettura nickname"); exit(1); }
    username[readedbyte-1] = '\0';
    write(log, registration, sizeof(registration)-1);
    write(log, username, readedbyte);
    write(log, "\n", 1);
    strcpy(usernamesrc, username);
}
void gaming(int user, char * username, int log, char ** map, int width, int height) {
    int startingX;
    int startingY;
    int collectedItems = 0;
    do {
        startingX = rand() % height;
        startingY = rand() % width;
    } while(map[startingX][startingY] != PATH);

    sendMap(user, map, width, height, startingX, startingY);
    char buffer[256];
    
    while(1) {
        int readedbyte = recv(user, buffer, sizeof(buffer)-1, 0);
        if(readedbyte <= 0) { 
            printf("Client disconnesso\n");
            fflush(stdout);
            break;
        }
        buffer[readedbyte] = '\0';
        
        if(strcmp(buffer, "exit") == 0) {
            printf("Client disconnesso\n");
            fflush(stdout);
            break;
        }

        write(log, "COMMAND: ", 9);
        write(log, buffer, readedbyte);
        write(log, "\n", 1);

        // Controlla vittoria PRIMA del movimento
        int hasWon = 0;
        if(strcmp(buffer, "W") == 0 && startingX == 0) {
            hasWon = 1;
        }
        else if(strcmp(buffer, "S") == 0 && startingX == height-1) {
            hasWon = 1;
        }
        else if(strcmp(buffer, "A") == 0 && startingY == 0) {
            hasWon = 1;
        }
        else if(strcmp(buffer, "D") == 0 && startingY == width-1) {
            hasWon = 1;
        }

        if(hasWon) {
            send(user, "WIN", 3, 0);
            write(log, "USER: ", 6);
            write(log, username, strlen(username));
            write(log, " WON!\n", 6);
            writeScore(username, collectedItems);
            printf("User %s ha vinto!\n", username);
            fflush(stdout);
            break;  // ESCE SENZA inviare altra mappa
        }

        // Movimento
        if(strcmp(buffer, "W") == 0) { 
            if(startingX > 0 && map[startingX-1][startingY] != WALL) 
                startingX--;
        }
        else if(strcmp(buffer, "S") == 0) { 
            if(startingX < height-1 && map[startingX+1][startingY] != WALL) 
                startingX++;
        }
        else if(strcmp(buffer, "A") == 0) { 
            if(startingY > 0 && map[startingX][startingY-1] != WALL) 
                startingY--;
        }
        else if(strcmp(buffer, "D") == 0) { 
            if(startingY < width-1 && map[startingX][startingY+1] != WALL) 
                startingY++;
        }

        // Gestione item
        pthread_mutex_lock(&mutex);
        while(mapChanging) {
            pthread_cond_wait(&cond, &mutex);
        }
        mapChanging = 1;
        if(map[startingX][startingY] == ITEM) {
            map[startingX][startingY] = PATH; 
            collectedItems++; 
            write(log, "USER: ", 6);
            write(log, username, strlen(username));
            write(log, " COLLECTED ITEM\n", 16);
        }
        mapChanging = 0;
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&mutex);
        
        sendMap(user, map, width, height, startingX, startingY);
    }
}




void *newUser(void* data) {
    int client = ((struct data*)data)->user;
	int log = ((struct data*)data)->log;
	char **map = ((struct data*)data)->map;
	int width = ((struct data*)data)->width;
	int height = ((struct data*)data)->height;
    char username[256];
    free(data);
    authenticate(client, log, username);
    pthread_mutex_lock(&lobbyMutex);
    nReady++;
    printf("Utenti pronti: %d/%d\n", nReady, nClients);
    if(nReady == nClients) {
        printf("Tutti i giocatori sono pronti, inizio partita!\n");
        pthread_cond_broadcast(&lobbyCond);
    }
    else {
        printf("In attesa che tutti i giocatori siano pronti...\n");
        pthread_cond_wait(&lobbyCond, &lobbyMutex);
    }  
    pthread_mutex_unlock(&lobbyMutex);
	gaming(client, username, log, map, width, height);
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
        nClients++;

        pthread_detach(tid);
        printf("Nuovo client connesso!\n");
        fflush(stdout);
    }

    freeMap(map, width);
    close(sockfd);
    return 0;
}
