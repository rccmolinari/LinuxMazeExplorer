#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <errno.h>

#define BUFFER_SIZE 512

int sockfd;
pthread_mutex_t sockMutex = PTHREAD_MUTEX_INITIALIZER;

/* Thread per leggere messaggi dal server */
void *readerThread(void *arg){
    char buf[BUFFER_SIZE];
    while(1){
        int r = recv(sockfd, buf, sizeof(buf)-1, 0);
        if(r<=0){ break; }
        buf[r]='\0';

        /* ogni messaggio è singolo carattere o mappa */
        if(strcmp(buf,"E")==0){
            printf("[Server] Partita terminata, attendo risultato...\n");
        } else if(strcmp(buf,"W")==0){
            printf("[Server] Hai vinto!\n");
            break;
        } else if(strcmp(buf,"L")==0){
            printf("[Server] Hai perso!\n");
            break;
        } else if(strcmp(buf,"M")==0){
            printf("[Server] Uscita dalla mappa trovata!\n");
        } else {
            /* può essere mappa sfocata o vicina */
            printf("%s\n",buf);
        }
    }
    return NULL;
}

int main(){
    struct sockaddr_in serv;
    char username[256];

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd<0){ perror("socket"); exit(1); }

    serv.sin_family = AF_INET;
    serv.sin_port = htons(8080);
    serv.sin_addr.s_addr = inet_addr("127.0.0.1");

    if(connect(sockfd,(struct sockaddr*)&serv,sizeof(serv))<0){
        perror("connect"); exit(1);
    }

    printf("Inserisci username: ");
    fgets(username,sizeof(username),stdin);
    username[strcspn(username,"\r\n")]=0;

    send(sockfd,username,strlen(username),0);

    pthread_t tid;
    pthread_create(&tid,NULL,readerThread,NULL);

    char cmd[16];
    while(1){
        printf("Comando (W/A/S/D/exit): ");
        if(!fgets(cmd,sizeof(cmd),stdin)) break;
        cmd[strcspn(cmd,"\r\n")]=0;
        if(strlen(cmd)==0) continue;

        pthread_mutex_lock(&sockMutex);
        send(sockfd,cmd,strlen(cmd),0);
        pthread_mutex_unlock(&sockMutex);

        if(strcmp(cmd,"exit")==0) break;
    }

    pthread_join(tid,NULL);
    close(sockfd);
    printf("Connessione chiusa.\n");
    return 0;
}
