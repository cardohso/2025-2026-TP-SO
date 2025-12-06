#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>

#define SERVER_FIFO "FIFOSERVIDOR"
#define CLIENT_FIFO "FIFOCLIENTE%d"

#define MAX_MSG 300
#define TAM_USERNAME 20
#define MAX_CLIENTES 10

typedef struct {
    pid_t pid;
    char param2[TAM_USERNAME]; //username
    char comando[30];
    int temp;
    char msg[MAX_MSG];              
} MensagemT;

#endif // COMMON_H
