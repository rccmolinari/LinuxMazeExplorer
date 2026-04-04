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
int terminate = 0;
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

/* --------------------------------------------------------------------------
 * Utility di stampa
 * -------------------------------------------------------------------------- */
static void printSeparator(char c, int n) {
    for (int i = 0; i < n; i++) putchar(c);
    putchar('\n');
}

static void printMapUI(char **map, int cols, int rows, int x, int y, const char *title) {
    if (!map) return;
    int w = cols + 2; /* larghezza bordo: mappa + 2 caratteri '|' */

    printf("\n");
    printSeparator('=', w + 2);
    printf("  %s\n", title);
    printSeparator('=', w + 2);

    /* bordo superiore */
    printf("  +");
    for (int j = 0; j < cols; j++) putchar('-');
    printf("+\n");

    for (int i = 0; i < rows; i++) {
        printf("  |");
        for (int j = 0; j < cols; j++) {
            //if (i == x && j == y)
            //    putchar('X');
            //else
                putchar(map[i][j]);
        }
        printf("|\n");
    }

    /* bordo inferiore */
    printf("  +");
    for (int j = 0; j < cols; j++) putchar('-');
    printf("+\n");

    printf("  Legenda: X=tu  +=item  ?=nebbia  #=muro\n");
}

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

        printf("\n+----------------------+\n");
        printf(  "|  GIOCATORI ONLINE    |\n");
        printf(  "+----------------------+\n");

        recv(sockfd, &nUsr, sizeof(nUsr), 0);
            for (int i = 0; i < nUsr; i++) {
                recv(sockfd, &dimUsr, sizeof(dimUsr), 0);
                recv(sockfd, username, dimUsr, 0);
                username[dimUsr] = '\0';
                printf("  %d. %.*s\n", i + 1, dimUsr, username);
            }
        printf("+----------------------+\n");
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
        pthread_mutex_unlock(&socketMutex);
        if(n>0 && type == 'M') {
            pthread_mutex_lock(&socketMutex);
            recv(args->sockfd, &type, sizeof(char), 0); // consuma 'M'
            pthread_mutex_unlock(&socketMutex);
            printf("\n======================================\n");
            printf("  HAI RAGGIUNTO L'USCITA!\n");
            printf("  Attendi il risultato finale...\n");
            printf("======================================\n");
            fflush(stdout);
            /* fine partita: segnala al main ed esci */
            pthread_mutex_lock(&endMutex);
            end = 1;
            pthread_mutex_unlock(&endMutex);
        }
        if(n > 0 && type == 'E') {
            /* fine sessione: consuma 'E', leggi W o L e stampa il risultato */
            char wol[2];
            pthread_mutex_lock(&socketMutex);
            recv(args->sockfd, &type, sizeof(char), 0);
            int n = recv(args->sockfd, wol, 1, 0);
            pthread_mutex_unlock(&socketMutex);
            system("clear");
            if(n > 0 && wol[0] == 'W') {
                printf("\n======================================\n");
                printf("           VITTORIA!\n");
                printf("======================================\n\n");
            } else if(n > 0 && wol[0] == 'L') {
                printf("\n======================================\n");
                printf("           SCONFITTA\n");
                printf("  Andrà meglio la prossima volta...\n");
                printf("======================================\n\n");
            }
            fflush(stdout);
            end = 1;
            send(args->sockfd, "x", 1, 0); // notifica al server che abbiamo ricevuto il risultato
            sleep(1); 
            kill(getpid(), SIGUSR1); 
            close(args->sockfd);
            break;
        }
        if (n > 0 && type == 'B') {
            int localRows, localCols;
            pthread_mutex_lock(&socketMutex);
            char **blurredMap = receiveMap(args->sockfd, args->width, args->height, args->x, args->y, &localRows, &localCols);
            pthread_mutex_unlock(&socketMutex);
            if (blurredMap && !end) {
                /* aggiorna le dimensioni condivise col main */
                *(args->effectiveRows) = localRows;
                *(args->effectiveCols) = localCols;
                system("clear");
                printMapUI(blurredMap, localCols, localRows, *(args->x), *(args->y), "MAPPA AGGIORNATA (blurrata)");
                printf("\n  Comando [W/A/S/D] | list | exit > ");
                fflush(stdout);
                freeMap(blurredMap, localRows); /* usa la copia locale, non il puntatore condiviso */
            }
        }
        
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
    signal(SIGHUP, handler);
    char username[256];
    char password[256];
    if(argc < 3) {
        printf("Uso: %s <indirizzo_ip> <porta>\n", argv[0]);
        exit(1);
    }
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    char *ipaddress = argv[1];
    int port = atoi(argv[2]);
    if (sockfd < 0) { perror("socket"); return 1; }
    
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    inet_pton(AF_INET, ipaddress, &srv.sin_addr);
    
    if (connect(sockfd, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        printf("Errore: impossibile connettersi a %s\n", argv[1]);
        return 1;
    }
    char c;
    recv(sockfd, &c, 1, 0);

    if(c == 'R') {
        printf("Errore: il server ha rifiutato la connessione (partita gia' iniziata)\n");
        close(sockfd);
        return 1;
    }
    else if(c == 'A') {
        printf("Connessione accettata dal server!\n");
    }

    system("clear");
    printf("\n======================================\n");
    printf("      LINUX MAZE EXPLORER\n");
    printf("======================================\n\n");

    int choice = -1;
    do {
        printf("  [1] Registrati\n");
        printf("  [2] Login\n");
        printf("  [3] Giocatori connessi\n");
        printf("  Scelta: ");
        fflush(stdout);
    
        char choiceBuf[16];
        int nb = read(STDIN_FILENO, choiceBuf, sizeof(choiceBuf) - 1);
        if (nb > 0) { choiceBuf[nb] = '\0'; choice = atoi(choiceBuf); }
    
        if (choice == 3) {
            send(sockfd, "C", 1, 0);
            int count = 0;
            recv(sockfd, &count, sizeof(count), 0);
            printf("\n  Giocatori attualmente connessi: %d\n\n", count);
            choice = -1;   /* ripresenta il menu */
        }
    } while (choice < 1 || choice > 2);
    int readedbyte = 0;
    char res;
    switch(choice) {
        case 1:
            send(sockfd, "R", 1, 0);
            printf("  Username: ");
            fflush(stdout);
            readedbyte = read(STDIN_FILENO, username, sizeof(username)-1);
            username[readedbyte] = '\0';
            send(sockfd, username, readedbyte, 0);
            printf("  Password: ");
            fflush(stdout);
            readedbyte = read(STDIN_FILENO, password, sizeof(password)-1);
            password[readedbyte] = '\0';
            send(sockfd, password, readedbyte, 0);
            recv(sockfd, &res, 1, 0);
            if(res == 'Y') {
                printf("  [OK] Registrazione avvenuta con successo!\n");
            } else {
                printf("  [ERRORE] Username gia' esistente.\n");
                close(sockfd);
                return 1;
            }
            printf("  Username per il login: ");
            fflush(stdout);
            int nread = read(STDIN_FILENO, username, sizeof(username)-1);
            username[nread] = '\0';
            send(sockfd, username, nread, 0);
            recv(sockfd, &res, 1, 0);
            if(res == 'Y') {
                printf("  [OK] Login avvenuto con successo!\n");
            } else {
                printf("  [ERRORE] Login fallito: username errato.\n");
                close(sockfd);
                return 1;
            }
            break;
        case 2:
            send(sockfd, "L", 1, 0);
            printf("  Username: ");
            fflush(stdout);
            readedbyte = read(STDIN_FILENO, username, sizeof(username)-1);
            username[readedbyte] = '\0';
            send(sockfd, username, readedbyte, 0);
            recv(sockfd, &res, 1, 0);
            if(res == 'Y') {
                printf("  [OK] Login avvenuto con successo!\n");
            }
            else {
                printf("  [ERRORE] Login fallito: username errato.\n");
                close(sockfd);
                return 1;
            }
            break;
    }
    
    printf("\n  In attesa che tutti i giocatori siano pronti...\n");
    fflush(stdout);

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
    
    system("clear");
    printMapUI(map, effectiveCols, effectiveRows, x, y, "LABIRINTO");
    
    /* ----- LOOP PRINCIPALE ----- */
    while (!checkEnd()) {
        char command[256];
        printf("\n  Comando [W/A/S/D] | list | exit > ");
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
            printMapUI(map, effectiveCols, effectiveRows, x, y, "LABIRINTO");
        } else {
            pthread_mutex_unlock(&socketMutex);
        }
    }
    
    /* attende che il thread completi la notifica del risultato finale */
    while(1) {
        sleep(1);
    }
    freeMap(map, effectiveRows);
    return 0;
}