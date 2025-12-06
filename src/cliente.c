#define _XOPEN_SOURCE 700
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

#include "../include/common.h"

#define SERVER_FIFO "FIFOSERVIDOR"
#define CLIENT_FIFO "FIFOCLIENTE%d"

#define MAX_MSG 300
#define TAM_USERNAME 20
#define MAX_CLIENTES 10

/* Helper: print menu */
static void print_menu(void){
    puts("\n=== Cliente (menu) ===");
    puts("1) Agendar serviço - agendar <hora> <local> <distancia>");
    puts("2) Cancelar serviço - cancelar <id>");
    puts("3) Consultar serviços - consultar");
    puts("4) Entrar no veículo - entrar <destino>");
    puts("5) Sair do veículo");
    puts("q) Sair(terminar programa) - terminar");
}

/* Stub handlers - replace with IPC/send message code */
static void handle_agendar(int fd, const char *username, const char *cmd){
    MensagemT msg;
    msg.pid = getpid();
    strncpy(msg.param2, username, TAM_USERNAME);
    strcpy(msg.comando, "agendar");

    char hora[30], local[100], distancia[30];
    if (sscanf(cmd, "agendar %s %s %s", hora, local, distancia) != 3) {
        puts("Formato inválido. Use: agendar <hora> <local> <distancia>");
        return;
    }
    snprintf(msg.msg, MAX_MSG, "%s %s %s", hora, local, distancia);

    write(fd, &msg, sizeof(MensagemT));
    puts("[Agendar] Pedido de agendamento enviado.");
}
static void handle_consultar(int fd, const char *username){
    MensagemT msg;
    msg.pid = getpid();
    strncpy(msg.param2, username, TAM_USERNAME);
    strcpy(msg.comando, "consultar");
    msg.msg[0] = '\0'; 

    write(fd, &msg, sizeof(MensagemT));
    puts("[Consultar] Pedido de consulta enviado.");
}
static void handle_cancelar(int fd, const char *username, const char *cmd){
    MensagemT msg;
    msg.pid = getpid();
    strncpy(msg.param2, username, TAM_USERNAME);
    strcpy(msg.comando, "cancelar");

    if (sscanf(cmd, "cancelar %s", msg.msg) != 1) {
        puts("Formato inválido. Use: cancelar <id>");
        return;
    }

    write(fd, &msg, sizeof(MensagemT));
    puts("[Cancelar] Pedido de cancelamento enviado.");
}
static void handle_entrar(int fd, const char *username, const char *cmd){
    MensagemT msg;
    msg.pid = getpid();
    strncpy(msg.param2, username, TAM_USERNAME);
    strcpy(msg.comando, "entrar");

    if (sscanf(cmd, "entrar %s", msg.msg) != 1) {
        puts("Formato inválido. Use: entrar <destino>");
        return;
    }

    write(fd, &msg, sizeof(MensagemT));
    puts("[Entrar] Pedido de entrada no veículo enviado.");
}
static void handle_sair(int fd, const char *username){
    MensagemT msg;
    msg.pid = getpid();
    strncpy(msg.param2, username, TAM_USERNAME);
    strcpy(msg.comando, "sair");
    msg.msg[0] = '\0';

    write(fd, &msg, sizeof(MensagemT));
    puts("[Sair] Pedido de saída do veículo enviado.");
}

char CLIENT_FIFO_FINAL[100];

void handler_sigint(int sig, siginfo_t *info, void *s) {
    printf("\nClosing client...\n");
    unlink(CLIENT_FIFO_FINAL); // usar pipes para tirar do manager
    exit(0);
}

int main(int argc, char *argv[]){
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <username>\n", argv[0]);
        return 1;
    }
    printf("My PID [%d]\n", getpid());

    struct sigaction sa;                
    sa.sa_sigaction = handler_sigint; 
    sa.sa_flags = SA_RESTART | SA_SIGINFO;
    sigaction(SIGINT, &sa, NULL);

    // Criar o FIFO do cliente
    sprintf(CLIENT_FIFO_FINAL, CLIENT_FIFO, getpid());
    if (mkfifo(CLIENT_FIFO_FINAL, 0777) == -1 && errno != EEXIST) {
        perror("Erro ao criar o FIFO do cliente");
        return 1;
    }

    // Abrir o FIFO do servidor para enviar o login
    int fdServidor = open(SERVER_FIFO, O_WRONLY);
    if (fdServidor == -1) {
        perror("Erro ao abrir o FIFO do servidor");
        unlink(CLIENT_FIFO_FINAL);
        return 1;
    }

    char input[128];

    for(;;){
        print_menu();
        printf("Opção> ");
        if(!fgets(input, sizeof input, stdin)){
            putchar('\n');
            break;
        }

        char *p = input;
        while(*p == ' ' || *p == '\t') p++;

        if(*p == '\n' || *p == '\0') continue; /* empty */

        if(strncasecmp(p, "agendar", 7) == 0 || p){
            handle_agendar(fdServidor, argv[1], p);
            continue;
        }
        if(strncasecmp(p, "cancelar", 8) == 0){
            handle_cancelar(fdServidor, argv[1], p);
            continue;
        }
        if(strncasecmp(p, "consultar", 9) == 0){
            handle_consultar(fdServidor, argv[1]);
            continue;
        }
        if(strncasecmp(p, "entrar", 6) == 0){
            handle_entrar(fdServidor, argv[1], p);
            continue;
        }
        if(strncasecmp(p, "sair", 4) == 0){
            handle_sair(fdServidor, argv[1]);
            continue;
        }
        if(p[0] == 'q' || strncasecmp(p, "terminar", 8) == 0){
            puts("A sair...");
            break;
        }

        puts("Opção inválida. Tente novamente.");
    }

    return 0;
}