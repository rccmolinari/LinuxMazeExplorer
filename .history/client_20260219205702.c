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
    int effCols, effRows;

    while(1) {
        pthread_mutex_lock(&socketMutex);
        if(args->sockfd <= 0) {
            pthread_mutex_unlock(&socketMutex);
            break;
        }
        /* MSG_DONTWAIT impedisce al thread di bloccarsi se non c'e' nulla */
        int n = recv(args->sockfd, &type, sizeof(char), MSG_PEEK | MSG_DONTWAIT);
        if(n>0 && type == 'M') {
            printf("\n SEI USCITO DALLA MAPPA, ATTENDI I RISULTATI!\n");
            /* fine partita: segnala al main ed esci */
            pthread_mutex_lock(&endMutex);
            end = 1;
            pthread_mutex_unlock(&endMutex);
            break;
        }
        if(n > 0 && type == 'E') {
            /* fine sessione: consuma 'E', leggi W o L e stampa il risultato */
            char wol[2];
            recv(args->sockfd, &type, sizeof(char), 0);
            int n = recv(args->sockfd, wol, 1, 0);
            system("clear");
            if(n > 0 && wol[0] == 'W') {
                printf("\nVITTORIA!\n");
            } else {
                printf("\nSCONFITTA!\n");
            }
            kill(getpid(), SIGUSR1); 
            close(args->sockfd);
            break;
        }
        if (n > 0 && type == 'B') {
            /* aggiornamento nebbia: ricevi la mappa e ridisegnala */
            char **blurredMap = receiveMap(args->sockfd, args->width, args->height, args->x, args->y, &effRows, &effCols);
            if (blurredMap) {
                system("clear");
                printf("\n--- AGGIORNAMENTO AMBIENTALE ---\n");
                printMap(blurredMap, effCols, effRows, *(args->x), *(args->y));
                printf("Comando (W/A/S/D): ");
                fflush(stdout);
                freeMap(blurredMap, effRows);
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
    
    printf("***CLIENT: INSERISCI USERNAME***\n"); 
    char username[256];
    int readedbyte = read(0, username, sizeof(username)-1);
    username[readedbyte] = '\0';
    send(sockfd, username, readedbyte, 0);
    
    int width, height, x, y;
    int effectiveCols, effectiveRows;
    
    /* prima ricezione mappa; il thread non e' ancora attivo ma usiamo
     * il mutex per coerenza con tutto il resto del codice */
    pthread_mutex_lock(&socketMutex);
    char **map = receiveMap(sockfd, &width, &height, &x, &y, &effectiveRows, &effectiveCols);
    pthread_mutex_unlock(&socketMutex);

    /* avvia il thread che ascolta in background gli aggiornamenti del server */
    pthread_t tid;
    struct thread_args t_args = {sockfd, &width, &height, &x, &y};
    pthread_create(&tid, NULL, silentWaitBlurredMap, &t_args);
    pthread_detach(tid);
    
    printf("Gioco iniziato!\n");
    printMap(map, effectiveCols, effectiveRows, x, y);
    
    /* ----- LOOP PRINCIPALE ----- */
    while (!checkEnd()) {
        char command[256];
        printf("\nComando (W/A/S/D) | exit: ");
        fflush(stdout);
        
        readedbyte = read(0, command, sizeof(command)-1);
        if(readedbyte <= 0) continue;
        command[readedbyte-1] = '\0';
        
        /* invio comando e ricezione mappa aggiornata, protetti da mutex */
        pthread_mutex_lock(&socketMutex);
        sendCommand(sockfd, command);
        
        char response[16];
        int n = recv(sockfd, response, 3, MSG_PEEK);
        
        if(n > 0) {
            response[n] = '\0';
            if(response[0] == 'M' || response[0] == 'E') {
                /* uscita trovata: il thread la gestira' e segnalerà la fine partita */
                pthread_mutex_unlock(&socketMutex);
                continue;
            }
            char **new_map = receiveMap(sockfd, &width, &height, &x, &y, &effectiveRows, &effectiveCols);
            if(new_map != NULL) {
                freeMap(map, effectiveRows);
                map = new_map;
            }
        }
        pthread_mutex_unlock(&socketMutex);
        system("clear");
        printMap(map, effectiveCols, effectiveRows, x, y);
    }
    
    freeMap(map, effectiveRows);
    /* attende che il thread completi la notifica del risultato finale */
    while(1) {
        sleep(1);
    }
    return 0;
}