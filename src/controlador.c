#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#define SERVER_FIFO "FIFOSERVIDOR"

void cleanup(int signo) {
    printf("\nClosing server and cleaning up resources...\n");
    unlink(SERVER_FIFO);
    exit(0);
}

int main() {
    if (mkfifo(SERVER_FIFO, 0666) == -1) {
        if (errno != EEXIST) {
            perror("Failed to create server FIFO");
            exit(EXIT_FAILURE);
        }
    }

    struct sigaction sa;
    sa.sa_handler = cleanup;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    printf("Server is running. Waiting for clients...\n");

    int fd_servidor = open(SERVER_FIFO, O_RDONLY);
    if (fd_servidor == -1) {
        perror("Failed to open server FIFO for reading");
        exit(EXIT_FAILURE);
    }
    
    // Dummy loop to keep server running
    while(1) {
        sleep(1);
    }

    close(fd_servidor);
    unlink(SERVER_FIFO);

    return 0;
}
