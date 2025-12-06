#define _XOPEN_SOURCE 700

#include <signal.h>
#include <pthread.h>
#include "../include/common.h"

void cleanup(int signo) {
    printf("\nClosing server and cleaning up resources...\n");
    unlink(SERVER_FIFO);
    exit(0);
}

void process_message(const MensagemT *msg) {
    printf("Received command '%s' from client %d (%s)\n", msg->comando, msg->pid, msg->param2);

    if (strcmp(msg->comando, "agendar") == 0) {
        printf("  Details: %s\n", msg->msg);
        // Placeholder for agendar logic
    } else if (strcmp(msg->comando, "consultar") == 0) {
        // Placeholder for consultar logic
    } else if (strcmp(msg->comando, "cancelar") == 0) {
        printf("  Details: %s\n", msg->msg);
        // Placeholder for cancelar logic
    } else if (strcmp(msg->comando, "entrar") == 0) {
        printf("  Details: %s\n", msg->msg);
        // Placeholder for entrar logic
    } else if (strcmp(msg->comando, "sair") == 0) {
        // Placeholder for sair logic
    } else {
        printf("  Unknown command.\n");
    }
}

void *thread_process_message(void *arg) {
    pthread_detach(pthread_self());
    MensagemT *msg = (MensagemT *)arg;
    process_message(msg);
    free(msg);
    return NULL;
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

    while (1) {
        MensagemT *msg = malloc(sizeof(MensagemT));
        if (!msg) {
            perror("malloc");
            continue;
        }

        ssize_t bytes_read = read(fd_servidor, msg, sizeof(MensagemT));
        if (bytes_read > 0) {
            pthread_t thread_id;
            if (pthread_create(&thread_id, NULL, thread_process_message, msg) != 0) {
                perror("pthread_create");
                free(msg);
            }
        } else if (bytes_read == 0) {
            free(msg);
            close(fd_servidor);
            fd_servidor = open(SERVER_FIFO, O_RDONLY);
        } else {
            free(msg);
        }
    }

    close(fd_servidor);
    unlink(SERVER_FIFO);

    return 0;
}
