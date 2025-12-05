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
static void handle_agendar(void){ puts("[Agendar] (stub) - implementar envio de pedido ao controlador"); }
static void handle_consultar(void){ puts("[Consultar] (stub) - implementar pedido de consulta"); }
static void handle_cancelar(void){ puts("[Cancelar] (stub) - implementar pedido de cancelamento"); }
static void handle_entrar(void){ puts("[Entrar] (stub) - implementar sinal de entrada no veículo"); }
static void handle_sair(void){ puts("[Sair] (stub) - implementar sinal de saída do veículo"); }

char CLIENT_FIFO_FINAL[100];

typedef struct {
    pid_t pid;
    char param2[TAM_USERNAME]; //username
    char comando[30];
    int temp;
    char msg[MAX_MSG];              
} MensagemT;

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

        if(strncasecmp(p, "agendar", 7) == 0){
            handle_agendar();
            continue;
        }
        if(strncasecmp(p, "cancelar", 8) == 0){
            handle_cancelar();
            continue;
        }
        if(strncasecmp(p, "consultar", 9) == 0){
            handle_consultar();
            continue;
        }
        if(strncasecmp(p, "entrar", 6) == 0){
            handle_entrar();
            continue;
        }
        if(strncasecmp(p, "sair", 4) == 0){
            handle_sair();
            continue;
        }
        if(p[0] == 'q' || strncasecmp(p, "terminar", 8) == 0){
            puts("A sair...");
            break;
        }

        puts("Opção inválida. Por favor escolha 1-5 ou 'q' para sair.");
    }

    return 0;
}