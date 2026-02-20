if(n > 0 && type == 'E') {
    recv(args->sockfd, &type, 1, 0); // consuma E

    char wol;
    int r = recv(args->sockfd, &wol, 1, 0); // legge W o L

    system("clear");
    if(r > 0 && wol == 'W') {
        printf("\nVITTORIA!\n");
    } else if(r > 0 && wol == 'L') {
        printf("\nSCONFITTA!\n");
    } else {
        printf("\nCONNESSIONE PERSA PRIMA DEL RISULTATO\n");
    }

    pthread_mutex_lock(&endMutex);
    end = 1;
    pthread_mutex_unlock(&endMutex);

    pthread_mutex_unlock(&socketMutex); // rilascia PRIMA di kill
    kill(getpid(), SIGUSR1);
    close(args->sockfd);
    break;
}