#include "map.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/socket.h>

int dx[4] = {-2, 2, 0, 0}; // su, giù, sinistra, destra
int dy[4] = {0, 0, -2, 2};
int itemRate = 10; // probabilità 1/itemRate per generare un item

// shuffle Fisher–Yates
void shuffle(int *arr, int n) {
    for (int i = n-1; i > 0; i--) {
        int j = rand() % (i+1);
        int tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

// DFS: non tocca bordi, genera corridoi interni
void dfs(char **map, int **visited, int row, int col, int width, int height) {
    if(row <=0 || row>=height-1 || col<=0 || col>=width-1)
        return;
    if(visited[row][col]) return;
    visited[row][col] = 1;

    // item o corridoio
    map[row][col] = (rand() % itemRate == 0) ? ITEM : PATH;

    int dir[4] = {0,1,2,3};
    shuffle(dir, 4);

    for (int i=0; i<4; i++) {
        int d = dir[i];
        int nx = row + dx[d];
        int ny = col + dy[d];

        // controlla che la cella di destinazione sia interna e non visitata
        if (nx>0 && nx<height-1 && ny>0 && ny<width-1 && !visited[nx][ny]) {
            int mx = row + dx[d]/2;
            int my = col + dy[d]/2;
            // muro intermedio sicuro
            if(mx>=0 && mx<height && my>=0 && my<width)
                map[mx][my] = PATH;
            dfs(map, visited, nx, ny, width, height);
        }
    }
}

// crea uscite sui bordi collegate ai corridoi interni
void addExits(char **map, int width, int height) {
    // lato sinistro
    while(1) {
        int i = 1 + rand() % (height-2);
        if(map[i][1]==PATH || map[i][1]==ITEM) { map[i][0]=PATH; break; }
    }
    // lato destro
    while(1) {
        int i = 1 + rand() % (height-2);
        if(map[i][width-2]==PATH || map[i][width-2]==ITEM) { map[i][width-1]=PATH; break; }
    }
    // lato superiore
    while(1) {
        int j = 1 + rand() % (width-2);
        if(map[1][j]==PATH || map[1][j]==ITEM) { map[0][j]=PATH; break; }
    }
    // lato inferiore
    while(1) {
        int j = 1 + rand() % (width-2);
        if(map[height-2][j]==PATH || map[height-2][j]==ITEM) { map[height-1][j]=PATH; break; }
    }
}

// genera la mappa
char **generateMap(int *width, int *height) {
    srand(time(NULL));

    // forza width e height dispari per evitare muri frammentati
    int w = (MINWIDTHMAP + rand() % (MAXWIDTHMAP - MINWIDTHMAP + 1)) | 1;
    int h = (MINHEIGHTMAP + rand() % (MAXHEIGHTMAP - MINHEIGHTMAP + 1)) | 1;

    *width = w;
    *height = h;

    char **map = malloc(h * sizeof(char*));
    int **visited = malloc(h * sizeof(int*));
    if(!map || !visited) return NULL;

    for(int i=0;i<h;i++) {
        map[i] = malloc(w * sizeof(char));
        visited[i] = malloc(w * sizeof(int));
        if(!map[i] || !visited[i]) {
            for(int j=0;j<i;j++) { free(map[j]); free(visited[j]); }
            free(map); free(visited);
            return NULL;
        }
        for(int j=0;j<w;j++) {
            map[i][j] = WALL;  // usa la costante WALL
            visited[i][j] = 0;
        }
    }

    // PARTENZA DAL CENTRO (pari/dispari coerente)
    int startX = h/2 | 1;
    int startY = w/2 | 1;

    dfs(map, visited, startX, startY, w, h);
    addExits(map, w, h);

    // libera visited
    for(int i=0;i<h;i++) free(visited[i]);
    free(visited);

    return map;
}

// libera la mappa
void freeMap(char **map, int height) {
    if(!map) return;
    for(int i=0;i<height;i++) free(map[i]);
    free(map);
}

// stampa la mappa
void printMap(char **map, int width, int height) {
    if(!map) return;
    for(int i=0;i<height;i++) {
        for(int j=0;j<width;j++)
            putchar(map[i][j]);
        putchar('\n');
    }
}

void printMapBlurred(char **map, int width, int height, int x, int y, int visited[height][width]) {
    if(!map) return;
    for(int i=0;i<height;i++) {
        for(int j=0;j<width;j++) {
            if(visited[i][j] == 1 || (abs(i - x) <= 1 && abs(j - y) <= 1)) {
                if(i==x && j==y)
                    putchar('X'); // posizione attuale
                else
                    putchar(map[i][j]);
            } else {
                putchar('?');
            }
        }
        putchar('\n');
    }
}

void printMapAdjacent(char **map, int width, int height, int x, int y) {
    if(!map) return;
    for(int i=x-1;i<=x+1;i++) {
        for(int j=y-1;j<=y+1;j++) {
            if(i>=0 && i<height && j>=0 && j<width) {
                if(i==x && j==y)
                    putchar('X');
                else
                    putchar(map[i][j]);
            } 
        }
        putchar('\n');
    }
}

char **receiveMap(int sockfd, int *width, int *height, int *x, int *y) {
    // ricevi dimensioni
    if(recv(sockfd, width, sizeof(int), 0) != sizeof(int)) perror("recv width");
    if(recv(sockfd, height, sizeof(int), 0) != sizeof(int)) perror("recv height");
    if(recv(sockfd, x, sizeof(int), 0) != sizeof(int)) perror("recv x");
    if(recv(sockfd, y, sizeof(int), 0) != sizeof(int)) perror("recv y");
    // alloca matrice
    char **map = malloc(*height * sizeof(char*));
    for(int i=0; i<*height; i++) {
        map[i] = malloc(*width * sizeof(char));
        int recvd = 0;
        while(recvd < *width) {
            int n = recv(sockfd, map[i] + recvd, *width - recvd, 0);
            if(n <= 0) { perror("recv row"); return NULL; }
            recvd += n;
        }
    }
    return map;
}
void sendMap(int sockfd, char **map, int width, int height, int x, int y) {
    // 1. invio dimensioni
    if(send(sockfd, &width, sizeof(int), 0) != sizeof(int)) perror("send width");
    if(send(sockfd, &height, sizeof(int), 0) != sizeof(int)) perror("send height");
    if(send(sockfd, &x, sizeof(int), 0) != sizeof(int)) perror("send x");
    if(send(sockfd, &y, sizeof(int), 0) != sizeof(int)) perror("send y");

    // 2. invio dati riga per riga
    for(int i=0; i<height; i++) {
        int sent = 0;
        while(sent < width) {
            int n = send(sockfd, map[i] + sent, width - sent, 0);
            if(n <= 0) { perror("send row"); return; }
            sent += n;
        }
    }
}

void adjVisit(int width, int height, int x, int y, int visited[height][width]) {
    if(x > 0) visited[x-1][y] = 1;
    if(x < height-1) visited[x+1][y] = 1;
    if(y > 0) visited[x][y-1] = 1;
    if(y < width-1) visited[x][y+1] = 1;
    // adiacenti diagonali
    if(x > 0 && y > 0) visited[x-1][y-1] = 1;
    if(x > 0 && y < width-1) visited[x-1][y+1] = 1;
    if(x < height-1 && y > 0) visited[x+1][y-1] = 1;
    if(x < height-1 && y < width-1) visited[x+1][y+1] = 1;
}
