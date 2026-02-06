#ifndef MAP_H
#define MAP_H

#define MAXWIDTHMAP 50
#define MINWIDTHMAP 40
#define MAXHEIGHTMAP 15
#define MINHEIGHTMAP 10
#define WALL '#'
#define PATH ' '
#define ITEM '+'

// Genera una mappa di dimensioni casuali (x = larghezza, y = altezza)
char **generateMap(int *width, int *height);
char **receiveMap(int sockfd, int *width, int *height, int *x, int *y);
void sendMap(int sockfd, char **map, int width, int height, int x, int y);
// Libera la memoria della mappa
void freeMap(char **map, int width);

// Stampa la mappa su stdout (per debug)
void printMap(char **map, int width, int height);

void printMapBlurred(char **map, int width, int height, int x, int y, int visited[height][width]);

void printMapAdjacent(char **map, int width, int height, int x, int y);

void adjVisit(int width, int height, int x, int y, int visited[height][width]);
#endif
