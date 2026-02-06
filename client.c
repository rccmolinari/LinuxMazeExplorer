#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include "map.h"

void sendCommand(int sockfd, const char * command) {
	if(command == NULL) return;
	if(strcmp(command, "exit") == 0) {
		close(sockfd);
		exit(0);
	}
	send(sockfd, command, strlen(command), 0);
}
int main(int argc, char* argv[]) {
	if(argc < 2) {
		printf("<program> + <ip address>\n");
		exit(1);
	}
	char * ipaddress = argv[1];
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
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
	printf("***CLIENT NOTIFICATION: INSERT USERNAME***\n"); 
	fflush(stdout);
	char username[256];
	int readedbyte = read(0, username, sizeof(username)-1);
	username[readedbyte] = '\0';
	send(sockfd, username, readedbyte, 0);
	int width;
	int height;
	int x;
	int y;
	char ** map =receiveMap(sockfd, &width, &height, &x, &y);
	int visited[height][width];
	for(int i=0;i<height;i++) {
		for(int j=0;j<width;j++) {
			visited[i][j] = 0;
		}
	}
	visited[x][y] = 1;
	printf("Gioco iniziato!\n");
	printMapAdjacent(map, width, height, x, y);
	while(1) {
		char command[256];
		int readedbyte = read(0, command, sizeof(command)-1);
		command[readedbyte-1] = '\0';
		sendCommand(sockfd, command);
		map = receiveMap(sockfd, &width, &height, &x, &y);
		if(map == NULL) {
			printf("Errore nella ricezione della mappa\n");
			break;
		}
		visited[x][y] = 1;
		adjVisit(width, height, x, y, visited);
		printMapBlurred(map, width, height, x, y, visited);
	}
	freeMap(map, height);
	close(sockfd);
	return 0;
	}
