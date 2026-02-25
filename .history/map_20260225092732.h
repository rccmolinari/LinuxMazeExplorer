#ifndef MAP_H
#define MAP_H

#define MAXWIDTHMAP 3
#define MINWIDTHMAP 3
#define MAXHEIGHTMAP 3
#define MINHEIGHTMAP 3
#define WALL '#'
#define PATH ' '
#define ITEM '+'

/* ── Codici colore ANSI ───────────────────────────────────────────────────── */
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"

/* testo */
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_WHITE   "\033[37m"

/* sfondo */
#define ANSI_BG_BLACK  "\033[40m"
#define ANSI_BG_RED    "\033[41m"
#define ANSI_BG_GREEN  "\033[42m"
#define ANSI_BG_BLUE   "\033[44m"

// Genera una mappa di dimensioni casuali (x = larghezza, y = altezza)