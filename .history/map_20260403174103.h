#ifndef MAP_H
#define MAP_H

#define MAXWIDTHMAP 11
#define MINWIDTHMAP 11
#define MAXHEIGHTMAP 11
#define MINHEIGHTMAP 11
#define WALL '#'
#define PATH ' '
#define ITEM '+'

// Genera una mappa di dimensioni casuali (x = larghezza, y = altezza)
char **generateMap(int *width, int *height);
char ** receiveMap(int sockfd, int *width, int *height, int *x, int *y, int *effectiveRows, int *effectiveCols);
// Libera la memoria della mappa
void freeMap(char **map, int width);

// Stampa la mappa su stdout (per debug)

void sendBlurredMap(int sockfd, char **map, int width, int height, int x, int y, int** visited);
void sendAdjacentMap(int sockfd, char **map, int width, int height, int x, int y);
void adjVisit(int width, int height, int x, int y, int **visited);

void printMap(char **map, int width, int height, int x, int y);
#endif
