#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>

#include "../include/common.h"

// Client state management (shared resource)
typedef struct {
    pid_t pid;
    char username[TAM_USERNAME];
    int active;
} ClienteInfo;

ClienteInfo clientes[MAX_CLIENTES];
pthread_mutex_t clientes_mutex;

// --- RESPONSE AND CLEANUP FUNCTIONS ---

// Sends a response back to a specific client's private FIFO
void send_client_response(pid_t client_pid, const char *message) {
    char client_fifo_name[100];
    sprintf(client_fifo_name, CLIENT_FIFO, client_pid);
    
    // Open client pipe for WRITING ONLY, and close immediately (atomic action).
    int fd_cliente = open(client_fifo_name, O_WRONLY); 
    if (fd_cliente == -1) {
        // The client process might have terminated unexpectedly.
        perror("Warning: Failed to open client FIFO for response");
        return;
    }

    MensagemT response;
    response.pid = getpid();
    strcpy(response.param2, "Servidor");
    strncpy(response.msg, message, MAX_MSG);
    response.msg[MAX_MSG-1] = '\0';

    write(fd_cliente, &response, sizeof(MensagemT));
    close(fd_cliente);
}

// Signal handler for clean termination (e.g., Ctrl+C)
void cleanup(int signo) {
    printf("\nClosing server and cleaning up resources (SIGINT received)...\n");
    // Ideally, send NOTIFY_PLATAFORMA_END to all active clients here.
    unlink(SERVER_FIFO);
    exit(0);
}

// --- COMMAND PROCESSING ---

// Processes service-related commands (executed in a thread)
void process_command_message(const MensagemT *msg) {
    printf("Command '%s' from PID %d / User '%s' received.\n", msg->comando, msg->pid, msg->param2);

    // Implement real logic here (scheduling, database updates, launching vehicle process, etc.)

    if (strcmp(msg->comando, "agendar") == 0) {
        printf("  Details: %s\n", msg->msg);
        send_client_response(msg->pid, "Scheduling received and processing.");
    } else if (strcmp(msg->comando, "consultar") == 0) {
        send_client_response(msg->pid, "Consultation received. (No data available).");
    } else if (strcmp(msg->comando, "cancelar") == 0) {
        printf("  Details: ID=%s\n", msg->msg);
        send_client_response(msg->pid, "Cancellation request received.");
    } else {
        send_client_response(msg->pid, "Unknown service command.");
    }
}

// Thread to process any incoming message (handles login/logout/commands)
void *thread_process_message(void *arg) {
    pthread_detach(pthread_self()); // Thread exits automatically upon return
    MensagemT *msg = (MensagemT *)arg;

    // --- LOGIN/LOGOUT Management (Critical Section) ---
    if (strcmp(msg->comando, "login") == 0) {
        pthread_mutex_lock(&clientes_mutex);
        int found_slot = -1;
        int duplicate = 0;
        
        // 1. Check for duplicates and find a free slot
        for (int i = 0; i < MAX_CLIENTES; i++) {
            if (clientes[i].active) {
                if (clientes[i].pid == msg->pid || strncmp(clientes[i].username, msg->param2, TAM_USERNAME) == 0) {
                    duplicate = 1;
                    break;
                }
            } else if (found_slot == -1) {
                found_slot = i;
            }
        }
        
        // 2. Respond and update state
        if (duplicate) {
            send_client_response(msg->pid, "Login failed: Username or PID already in use.");
        } else if (found_slot != -1) {
            clientes[found_slot].pid = msg->pid;
            strncpy(clientes[found_slot].username, msg->param2, TAM_USERNAME);
            clientes[found_slot].active = 1;
            printf("Client with PID %d and username '%s' connected.\n", msg->pid, msg->param2);
            send_client_response(msg->pid, "Login OK. Welcome!");
        } else {
            send_client_response(msg->pid, "Login failed: Server is full (Max clients reached).");
        }
        pthread_mutex_unlock(&clientes_mutex);

    } else if (strcmp(msg->comando, "terminar") == 0) { // Handles client's 'terminar' (logout)
        pthread_mutex_lock(&clientes_mutex);
        for (int i = 0; i < MAX_CLIENTES; i++) {
            if (clientes[i].active && clientes[i].pid == msg->pid) {
                clientes[i].active = 0;
                printf("Client with PID %d and username '%s' disconnected.\n", msg->pid, clientes[i].username);
                break;
            }
        }
        pthread_mutex_unlock(&clientes_mutex);
    } else {
        // --- Service Commands ---
        process_command_message(msg);
    }

    free(msg);
    return NULL;
}

// --- MAIN LOOP ---

int main() {
    // Initialization
    for (int i = 0; i < MAX_CLIENTES; i++) {
        clientes[i].active = 0;
    }
    pthread_mutex_init(&clientes_mutex, NULL);

    // 1. FIFO Creation and Signal Handling
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
    sigaction(SIGINT, &sa, NULL);

    printf("Server is running. Waiting for clients on %s...\n", SERVER_FIFO);

    // 2. Open Server FIFO for Persistent Read/Write (O_RDWR)
    int fd_servidor = open(SERVER_FIFO, O_RDWR); 
    if (fd_servidor == -1) {
        perror("Failed to open server FIFO for R/W");
        unlink(SERVER_FIFO);
        exit(EXIT_FAILURE);
    }

    // 3. Main reading loop
    while (1) {
        // Allocate memory for the incoming message to pass to the thread
        MensagemT *msg = malloc(sizeof(MensagemT));
        if (!msg) {
            perror("malloc");
            continue;
        }

        // Blocking read: waits here for a client message
        ssize_t bytes_read = read(fd_servidor, msg, sizeof(MensagemT));
        
        if (bytes_read > 0) {
            // New message received: spawn a detached thread to process it
            pthread_t thread_id;
            if (pthread_create(&thread_id, NULL, thread_process_message, msg) != 0) {
                perror("pthread_create failed");
                free(msg);
            }
        } else if (bytes_read == 0) {
            // This should not happen with O_RDWR, but if it does, log it.
            printf("Warning: Unexpected EOF on server FIFO.\n");
            free(msg);
            // The loop will simply continue to read from the already open FD.
        } else if (bytes_read == -1 && errno != EINTR) {
            perror("Fatal error reading server FIFO");
            free(msg);
            break;
        } else {
            // EINTR (interruption by signal): read interrupted, free memory and re-read.
            free(msg);
        }
    }

    // Final cleanup
    close(fd_servidor);
    unlink(SERVER_FIFO);
    pthread_mutex_destroy(&clientes_mutex);

    return 0;
}