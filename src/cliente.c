#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>

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

int main(int argc, char *argv[]){
    (void) argc; (void) argv;
    char input[128];

    for(;;){
        print_menu();
        printf("Opção> ");
        if(!fgets(input, sizeof input, stdin)){
            putchar('\n');
            break;
        }

        /* Trim leading spaces */
        char *p = input;
        while(*p == ' ' || *p == '\t') p++;

        if(*p == '\n' || *p == '\0') continue; /* empty */

        /* accept either numbers or words (case-insensitive) */
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