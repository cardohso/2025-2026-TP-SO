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

// Global variables for pipe management
char CLIENT_FIFO_FINAL[100];
char vehicle_fifo_name[100] = ""; // FIFO name for direct communication with vehicle
pthread_t receive_thread_id; 
int fdServidor = -1; // Global FD for the Server's FIFO

/* Helper: print menu */
static void print_menu(void){
    puts("\n=== Client (Menu) ===");
    puts("1) Schedule service - agendar <hour> <location> <distance>");
    puts("2) Cancel service - cancelar <id>");
    puts("3) Consult services - consultar");
    puts("4) Enter vehicle - entrar <destination>");
    puts("5) Exit vehicle - sair");
    puts("q) Quit program - terminar");
}

// --- COMMAND HANDLERS (Write to Server FIFO) ---

/* 1) Schedule service */
static void handle_agendar(int fd, const char *username, const char *cmd){
    MensagemT msg;
    msg.pid = getpid();
    strncpy(msg.param2, username, TAM_USERNAME);
    strcpy(msg.comando, "agendar");

    // Correct parsing: requires command + 3 arguments
    char temp_cmd[30], hora[30], local[100], distancia[30];
    if (sscanf(cmd, "%s %s %s %s", temp_cmd, hora, local, distancia) != 4) {
        puts("Invalid format. Use: agendar <hour> <location> <distance>");
        return;
    }
    
    snprintf(msg.msg, MAX_MSG, "%s %s %s", hora, local, distancia);
    msg.msg[MAX_MSG-1] = '\0'; 

    write(fd, &msg, sizeof(MensagemT));
    puts("[Schedule] Scheduling request sent.");
}

/* 3) Consult services */
static void handle_consultar(int fd, const char *username){
    MensagemT msg;
    msg.pid = getpid();
    strncpy(msg.param2, username, TAM_USERNAME);
    strcpy(msg.comando, "consultar");
    msg.msg[0] = '\0'; 

    write(fd, &msg, sizeof(MensagemT));
    puts("[Consult] Consultation request sent.");
}

/* 2) Cancel service */
static void handle_cancelar(int fd, const char *username, const char *cmd){
    MensagemT msg;
    msg.pid = getpid();
    strncpy(msg.param2, username, TAM_USERNAME);
    strcpy(msg.comando, "cancelar");

    char temp_cmd[30], id_str[30];
    if (sscanf(cmd, "%s %s", temp_cmd, id_str) != 2) {
        puts("Invalid format. Use: cancelar <id>");
        return;
    }
    
    strncpy(msg.msg, id_str, MAX_MSG);
    msg.msg[MAX_MSG-1] = '\0';

    write(fd, &msg, sizeof(MensagemT));
    puts("[Cancel] Cancellation request sent.");
}

/* 4) Enter vehicle */
static void handle_entrar(int fd, const char *username, const char *cmd){
    MensagemT msg;
    msg.pid = getpid();
    strncpy(msg.param2, username, TAM_USERNAME);
    strcpy(msg.comando, "entrar");

    char temp_cmd[30], destino[MAX_MSG];
    if (sscanf(cmd, "%s %s", temp_cmd, destino) != 2) {
        puts("Invalid format. Use: entrar <destination>");
        return;
    }

    strncpy(msg.msg, destino, MAX_MSG);
    msg.msg[MAX_MSG-1] = '\0';

    write(fd, &msg, sizeof(MensagemT));
    puts("[Enter] Vehicle entry request sent.");
}

/* 5) Exit vehicle */
static void handle_sair(int fd, const char *username){
    MensagemT msg;
    msg.pid = getpid();
    strncpy(msg.param2, username, TAM_USERNAME);
    strcpy(msg.comando, "sair");
    msg.msg[0] = '\0';

    write(fd, &msg, sizeof(MensagemT));
    puts("[Exit] Vehicle exit request sent.");
}

// --- RECEIVING THREAD (Reads from Client FIFO) ---

void *receive_thread(void *arg) {
    // Open its own FIFO for Read/Write (O_RDWR) to prevent open() from blocking, 
    // which is the standard technique used to manage FIFOs for a permanent process.
    int fd_cliente = open(CLIENT_FIFO_FINAL, O_RDWR);
    if (fd_cliente == -1) {
        perror("Error opening client FIFO for reading/writing");
        return NULL;
    }

    MensagemT msg;
    while (1) {
        // This is a blocking read, allowing the thread to wait without active polling.
        ssize_t bytes_read = read(fd_cliente, &msg, sizeof(MensagemT));
        
        if (bytes_read > 0) {
            // Extract vehicle FIFO name if message contains VEHICLE_READY
            if (strstr(msg.msg, "VEHICLE_READY:") != NULL) {
                char *fifo_ptr = strstr(msg.msg, "FIFOVEICULO");
                if (fifo_ptr) {
                    sscanf(fifo_ptr, "%s", vehicle_fifo_name);
                    // Remove any trailing punctuation (period, etc)
                    char *period = strchr(vehicle_fifo_name, '.');
                    if (period) *period = '\0';
                    printf("[INFO] Stored vehicle FIFO: %s\n", vehicle_fifo_name);
                }
            }

            printf("\n[Server/Vehicle]: %s\n", msg.msg);

            // Exit if login failed or if server forces disconnection
            if (strncmp(msg.msg, "Login failed", 12) == 0) {
                printf("Press ENTER to exit...\n");
                fflush(stdout);
                sleep(2); // Give user time to see the message
                close(fd_cliente);
                unlink(CLIENT_FIFO_FINAL);
                if (fdServidor != -1) close(fdServidor);
                _exit(1); // Force immediate exit
            }
            
            // Exit if server is shutting down
            if (strstr(msg.msg, "shutting down") != NULL || strstr(msg.msg, "Goodbye") != NULL) {
                printf("Exiting due to server shutdown...\n");
                fflush(stdout);
                sleep(1);
                close(fd_cliente);
                unlink(CLIENT_FIFO_FINAL);
                if (fdServidor != -1) close(fdServidor);
                _exit(0);
            }

            // Re-print prompt to guide the user after interruption
            printf("Opção> ");
            fflush(stdout);
        } else if (bytes_read == 0) {
            // EOF: The writing end was closed (Controller terminated).
            printf("\n[ATTENTION] Server connection lost (FIFO closed).\n");
            printf("Exiting...\n");
            close(fd_cliente);
            unlink(CLIENT_FIFO_FINAL);
            if (fdServidor != -1) close(fdServidor);
            _exit(0);
        } else if (bytes_read == -1 && errno != EINTR) {
            perror("Error reading client FIFO");
            break;
        }
    }
    close(fd_cliente);
    return NULL;
}

// --- CLEANUP AND EXIT FUNCTIONS ---

void handler_sigint(int sig, siginfo_t *info, void *s) {
    printf("\nClosing client via SIGINT...\n");

    // Send 'terminar' message to the server if possible
    if (fdServidor != -1) {
        MensagemT logout_msg;
        logout_msg.pid = getpid();
        strcpy(logout_msg.comando, "terminar"); 
        write(fdServidor, &logout_msg, sizeof(MensagemT));
        close(fdServidor);
    }

    unlink(CLIENT_FIFO_FINAL);
    exit(0);
}

int main(int argc, char *argv[]){
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <username>\n", argv[0]);
        return 1;
    }
    printf("My PID [%d]\n", getpid());

    // 1. SIGINT Configuration
    struct sigaction sa;                 
    sa.sa_sigaction = handler_sigint; 
    sa.sa_flags = SA_RESTART | SA_SIGINFO;
    sigaction(SIGINT, &sa, NULL);

    // 2. Client FIFO Creation
    sprintf(CLIENT_FIFO_FINAL, CLIENT_FIFO, getpid());
    if (mkfifo(CLIENT_FIFO_FINAL, 0777) == -1 && errno != EEXIST) {
        perror("Error creating client FIFO");
        return 1;
    }

    // 3. Server FIFO Opening (O_WRONLY)
    fdServidor = open(SERVER_FIFO, O_WRONLY);
    if (fdServidor == -1) {
        fprintf(stderr, "Error opening server FIFO. Is the controller running?\n");
        unlink(CLIENT_FIFO_FINAL);
        return 1;
    }
    printf("Connection to server established. Client FIFO: %s\n", CLIENT_FIFO_FINAL);


    // 4. Send Login Message
    MensagemT login_msg;
    login_msg.pid = getpid();
    strncpy(login_msg.param2, argv[1], TAM_USERNAME);
    strcpy(login_msg.comando, "login");
    login_msg.msg[0] = '\0';
    write(fdServidor, &login_msg, sizeof(MensagemT));

    // 5. Launch Receiving Thread (Asynchronous reading)
    if (pthread_create(&receive_thread_id, NULL, receive_thread, NULL) != 0) {
        perror("Error creating receiving thread");
        close(fdServidor);
        unlink(CLIENT_FIFO_FINAL);
        return 1;
    }

    char input[128];

    // 6. Main Loop (Command sending)
    print_menu();
    for(;;){
        printf("Opção> ");
        if(!fgets(input, sizeof input, stdin)){
            putchar('\n');
            break; 
        }

        char *p = input;
        while(*p == ' ' || *p == '\t') p++;

        if(*p == '\n' || *p == '\0') continue; 

        // Correct command dispatch logic 
        if(strncasecmp(p, "agendar", 7) == 0){
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
            // Send 'terminar' message (logout) before closing
            MensagemT logout_msg;
            logout_msg.pid = getpid();
            strcpy(logout_msg.comando, "terminar");
            write(fdServidor, &logout_msg, sizeof(MensagemT));
            puts("Exiting...");
            break;
        }

        puts("Invalid option. Try again.");
    }

    // Final cleanup (main thread)
    close(fdServidor);
    unlink(CLIENT_FIFO_FINAL); 

    return 0;
}