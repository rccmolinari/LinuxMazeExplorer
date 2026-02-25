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

/* --------------------------------------------------------------------------
 * Sincronizzazione
 *
 * socketMutex -> evita che main e thread leggano dal socket contemporaneamente
 * endMutex    -> protegge la variabile end (fine partita per timeout/uscita)
 * exitMutex   -> riservato per estensioni future
 * exitCond    -> condizione associata a exitMutex
 * -------------------------------------------------------------------------- */
pthread_mutex_t socketMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t endMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t exitMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t exitCond = PTHREAD_COND_INITIALIZER;
int exitGame = 0;
int end = 0;

/* --------------------------------------------------------------------------
 * Struttura argomenti per il thread di ascolto asincrono.
 * Contiene il socket e i puntatori alle variabili di stato della mappa,
 * aggiornate sia dal main che dal thread stesso.
 * -------------------------------------------------------------------------- */
struct thread_args {
    int sockfd;
    int *width;
    int *height;
    int *x;
    int *y;
    int *effectiveRows;
    int *effectiveCols;
    char ***map;
};

/* Lettura thread-safe di end */
int checkEnd() {
    pthread_mutex_lock(&endMutex);
    int r = end;
    pthread_mutex_unlock(&endMutex);
    return r;
}

/* --------------------------------------------------------------------------
 * sendCommand
 *
 * Invia un comando al server sul socket indicato.
 * Caso speciale: se il comando e' "exit" chiude il socket e termina
 * il processo senza aspettare risposta dal server.
 * -------------------------------------------------------------------------- */
void sendCommand(int sockfd, const char * command) {
    if(command == NULL) return;
    if(strcmp(command, "exit") == 0) {
        close(sockfd);
        exit(0);
    }
    if(strcmp(command, "list") == 0) {
        system("clear");
        int nUsr;
        int dimUsr;
        char username[256];
        send(sockfd, "list", 4, 0);
        write(STDOUT_FILENO, "LISTA UTENTI:\n", 14);
        recv(sockfd, &nUsr, sizeof(nUsr), 0);
        for(int i = 0; i < nUsr; i++) {
            recv(sockfd, &dimUsr, sizeof(dimUsr), 0);
            recv(sockfd, username, dimUsr, 0);
            username[dimUsr] = '\0';
            write(STDOUT_FILENO, username, dimUsr);
            write(STDOUT_FILENO, "\n", 1);
        }
        return; // evita la send doppia a fine funzione
    }
    send(sockfd, command, strlen(command), 0);
}

/* Gestore SIGUSR1: inviato dal thread al processo principale per forzarne
 * la terminazione dopo aver stampato il risultato finale. */
void handler(int signum) {
    exit(0);
}

/* --------------------------------------------------------------------------
 * silentWaitBlurredMap  [thread]
 *
 * Gira in background durante tutta la partita. Ad ogni iterazione controlla
 * se il server ha inviato qualcosa senza bloccare il main, usando
 * MSG_PEEK | MSG_DONTWAIT per sbirciare il primo byte senza consumarlo.
 *
 * Messaggi gestiti:
 *   'M' -> fine partita (timer scaduto); segnala al main tramite end=1
 *   'E' -> fine sessione; segue 'W' (vittoria) o 'L' (sconfitta),
 *          stampa il risultato e termina il processo via SIGUSR1
 *   'B' -> mappa aggiornata con nebbia in arrivo; ricevi e stampa subito
 *
 * Il mutex socketMutex e' necessario perche' il main usa lo stesso socket
 * per inviare comandi e ricevere la mappa aggiornata dopo ogni mossa.
 * -------------------------------------------------------------------------- */
void *silentWaitBlurredMap(void *arg) {
    struct thread_args *args = (struct thread_args *)arg;
    char type;

    while(1) {
        pthread_mutex_lock(&socketMutex);
        if(args->sockfd <= 0) {
            pthread_mutex_unlock(&socketMutex);
            break;
        }
        /* MSG_DONTWAIT impedisce al thread di bloccarsi se non c'e' nulla */
        int n = recv(args->sockfd, &type, sizeof(char), MSG_PEEK | MSG_DONTWAIT);
        if(n>0 && type == 'M') {
            recv(args->sockfd, &type, sizeof(char), 0); // consuma 'M'
            write(STDOUT_FILENO, "\nSEI USCITO DALLA MAPPA, ATTENDI I RISULTATI!***\n", 48);
            /* fine partita: segnala al main ed esci */
            pthread_mutex_lock(&endMutex);
            end = 1;
            pthread_mutex_unlock(&endMutex);
        }
        if(n > 0 && type == 'E') {
            /* fine sessione: consuma 'E', leggi W o L e stampa il risultato */
            char wol[2];
            recv(args->sockfd, &type, sizeof(char), 0);
            int n = recv(args->sockfd, wol, 1, 0);
            system("clear");
            if(n > 0 && wol[0] == 'W') {
                write(STDOUT_FILENO, "\nVITTORIA!\n", 12);
            } else if(n > 0 && wol[0] == 'L') {
                write(STDOUT_FILENO, "\nSCONFITTA!\n", 12);
            }
            send(args->sockfd, "x", 1, 0); // notifica al server che abbiamo ricevuto il risultato
            kill(getpid(), SIGUSR1); 
            close(args->sockfd);
            break;
        }
        if (n > 0 && type == 'B') {
            /* aggiornamento nebbia: ricevi la mappa e ridisegnala */
            char **blurredMap = receiveMap(args->sockfd, args->width, args->height, args->x, args->y, args->effectiveRows, args->effectiveCols);
            if (blurredMap) {
                system("clear");
                printf("\n--- AGGIORNAMENTO AMBIENTALE ---\n");
                printMap(blurredMap, *(args->effectiveCols), *(args->effectiveRows), *(args->x), *(args->y));
                printf("Comando (W/A/S/D): ");
                fflush(stdout);
                freeMap(blurredMap, *(args->effectiveRows));
            }
        }
        
        pthread_mutex_unlock(&socketMutex);
        
        /* pausa breve per non saturare la CPU e cedere il passo al main */
        usleep(200000); 
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * main
 *
 * Connette al server all'indirizzo IP passato come argomento (porta 8080),
 * invia il nome utente, riceve la mappa iniziale e avvia il thread di
 * ascolto asincrono. Poi entra nel loop principale: legge un comando da
 * stdin, lo invia al server e ridisegna la mappa con la risposta ricevuta.
 *
 * Il loop termina quando il thread segnala fine partita (end=1).
 * -------------------------------------------------------------------------- */
int main(int argc, char* argv[]) {
    signal(SIGUSR1, handler);
    char username[256];
    char password[256];
    if(argc < 2) {
        printf("<program> + <ip address>\n");
        exit(1);
    }
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    char *ipaddress = argv[1];
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
    int choice = -1;
    do {
        printf("Scegli cosa fare \n<1>REGISTRATI\n<2>LOGIN\n");
        char choiceBuf[16];
        int nb = read(STDIN_FILENO, choiceBuf, sizeof(choiceBuf) - 1);
        if (nb > 0) {
            choiceBuf[nb] = '\0';
            choice = atoi(choiceBuf);
        }
    } while(choice < 1 || choice > 2);
    int readedbyte = 0;
    char res;
    switch(choice) {
        case 1:
            send(sockfd, "R", 1, 0);
            write(STDOUT_FILENO, "INSERISCI USERNAME: ", 20);
            readedbyte = read(STDIN_FILENO, username, sizeof(username)-1);
            username[readedbyte] = '\0';
            send(sockfd, username, readedbyte, 0);
            write(STDOUT_FILENO, "INSERISCI PASSWORD: ", 20);
            readedbyte = read(STDIN_FILENO, password, sizeof(password)-1);
            password[readedbyte] = '\0';
            send(sockfd, password, readedbyte, 0);
            recv(sockfd, &res, 1, 0);
            if(res == 'Y') {
                printf("REGISTRAZIONE AVVENUTA CON SUCCESSO!\n");
            } else {
                printf("REGISTRAZIONE FALLITA: USERNAME GIA' ESISTENTE\n");
                close(sockfd);
                return 1;
            }
            write(STDOUT_FILENO, "INSERISCI USERNAME PER LOGIN: ", 30);
            int nread = read(STDIN_FILENO, username, sizeof(username)-1);
            username[nread] = '\0';
            send(sockfd, username, nread, 0);
            recv(sockfd, &res, 1, 0);
            if(res == 'Y') {
                printf("LOGIN AVVENUTO CON SUCCESSO!\n");
            } else {
                printf("LOGIN FALLITO: USERNAME ERRATO\n");
                close(sockfd);
                return 1;
            }
            break;
        case 2:
            send(sockfd, "L", 1, 0);
            write(STDOUT_FILENO, "INSERISCI USERNAME: ", 20);
            readedbyte = read(STDIN_FILENO, username, sizeof(username)-1);
            username[readedbyte] = '\0';
            send(sockfd, username, readedbyte, 0);
            recv(sockfd, &res, 1, 0);
            if(res == 'Y') {
                printf("LOGIN AVVENUTO CON SUCCESSO!\n");
            }
            else {
                printf("LOGIN FALLITO: USERNAME ERRATO\n");
                close(sockfd);
                return 1;
            }
            break;
    }
    
    int width, height, x, y;
    int effectiveCols, effectiveRows;
    
    pthread_mutex_lock(&socketMutex);
    char **map = receiveMap(sockfd, &width, &height, &x, &y, &effectiveRows, &effectiveCols);
    pthread_mutex_unlock(&socketMutex);

    int currentMapRows = effectiveRows; // righe effettive della map allocata corrente

    /* avvia il thread che ascolta in background gli aggiornamenti del server */
    pthread_t tid;
    struct thread_args t_args = {sockfd, &width, &height, &x, &y, &effectiveRows, &effectiveCols, &map};
    pthread_create(&tid, NULL, silentWaitBlurredMap, &t_args);
    pthread_detach(tid);
    
    printf("Gioco iniziato!\n");
    printMap(map, effectiveCols, effectiveRows, x, y);
    
    /* ----- LOOP PRINCIPALE ----- */
    while (!checkEnd()) {
        char command[256];
        printf("\nComando (W/A/S/D) | list | exit: ");
        fflush(stdout);
        
        readedbyte = read(0, command, sizeof(command)-1);
        if(readedbyte <= 0) continue;
        command[readedbyte-1] = '\0';
        
        pthread_mutex_lock(&socketMutex);
        sendCommand(sockfd, command);   
        if(strcmp(command, "list") != 0) {     
            char response[16];
            int n = recv(sockfd, response, 1, MSG_PEEK);
            
            if(n > 0) {
                response[n] = '\0';
                if(response[0] == 'M' || response[0] == 'E') {
                    pthread_mutex_unlock(&socketMutex);
                    break;
                }
                char **new_map = receiveMap(sockfd, &width, &height, &x, &y, &effectiveRows, &effectiveCols);
                if(new_map != NULL) {
                    freeMap(map, currentMapRows); // usa le righe della map precedente
                    map = new_map;
                    currentMapRows = effectiveRows; // aggiorna con le righe della nuova map
                }
            }
            pthread_mutex_unlock(&socketMutex);
            system("clear");
            printMap(map, effectiveCols, effectiveRows, x, y);
        } else {
            pthread_mutex_unlock(&socketMutex);
            printMap(map, effectiveCols, effectiveRows, x, y);
        }
    }
    
    /* attende che il thread completi la notifica del risultato finale */
    while(1) {
        sleep(1);
    }
    freeMap(map, effectiveRows);
    return 0;
}