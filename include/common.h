#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>

#define SERVER_FIFO "FIFOSERVIDOR"
#define CLIENT_FIFO "FIFOCLIENTE%d"
#define LOCK_FILE "/tmp/controlador.lock"

#define MAX_MSG 300
#define TAM_USERNAME 20
#define MAX_CLIENTES 10
#define NVEICULOS 10

// Message Structure for IPC between processes
typedef struct {
    pid_t pid;
    char param2[TAM_USERNAME]; // Used for Username
    char comando[30];          // Ex: login, agendar, cancelar, terminar
    char msg[MAX_MSG];         // Command arguments or server response content
} MensagemT;

#endif // COMMON_H