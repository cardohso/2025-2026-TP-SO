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
#include <sys/wait.h>
#include <time.h>

#include "../include/common.h"

// =========================================================
// GLOBAL STATE AND SYNCHRONIZATION VARIABLES
// =========================================================

// --- Mutexes ---
pthread_mutex_t clientes_mutex; // Protects the 'clientes' array
pthread_mutex_t frota_mutex;    // Protects the 'frota' (services) array and simulated time
pthread_mutex_t console_mutex;  // Protects console output

// --- Simulated Time Control ---
int current_simulated_time = 0;

// --- Client Management ---
typedef struct {
    pid_t pid;
    char username[TAM_USERNAME];
    int active;
} ClienteInfo;
ClienteInfo clientes[MAX_CLIENTES];

// --- Fleet and Service Management ---
// NOTE: This structure is used to manage both scheduled and running services.
typedef struct {
    int id_servico; // Unique Service ID
    pid_t pid_cliente;
    pid_t pid_veiculo;  // PID of the running vehicle process
    int hora_inicio;    // Scheduled hour (simulated time)
    int distancia_total;
    int km_percorridos;
    int estado;         // 0: Scheduled, 1: In Course, 2: Concluded/Free
    int fd_telemetria;  // File Descriptor of the anonymous pipe (to read vehicle stdout)
} ServicoEmCurso;

ServicoEmCurso frota[MAX_CLIENTES]; 
int num_servicos_ativos = 0;
long long int total_km_platform = 0; // Total KM counter for the platform

// --- File Descriptor for the main Server FIFO ---
int fd_servidor_main = -1;
// File descriptor for the administration console (stdin)
int fd_admin_input = 0; // stdin file descriptor is 0

// =========================================================
// 1. SUPPORT FUNCTIONS AND TELEMETRY
// =========================================================

// Sends a response back to a specific client's private FIFO (O_WRONLY)
void send_client_response(pid_t client_pid, const char *message) {
    char client_fifo_name[100];
    sprintf(client_fifo_name, CLIENT_FIFO, client_pid);
    
    // Use O_NONBLOCK to prevent the controller thread from blocking here if the client is not yet ready to read
    int fd_cliente = open(client_fifo_name, O_WRONLY | O_NONBLOCK); 
    if (fd_cliente == -1) {
        if (errno == ENXIO) {
            printf("[LOG] Client %d disconnected (FIFO not found/opened).\n", client_pid);
        } else {
            perror("Warning: Failed to open client FIFO for response");
        }
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

// Handler for cleanup upon receiving SIGINT
void cleanup(int signo) {
    printf("\nClosing server and cleaning up resources (SIGINT received)...\n");

    // Implement logic to notify all active clients and running vehicles of termination
    // ...

    // Clean up FIFOs and Mutexes
    if (fd_servidor_main != -1) close(fd_servidor_main);
    unlink(SERVER_FIFO);
    
    // Destroy mutexes (important!)
    pthread_mutex_destroy(&clientes_mutex);
    pthread_mutex_destroy(&frota_mutex);
    pthread_mutex_destroy(&console_mutex);

    exit(0);
}


// Thread to monitor a running vehicle via anonymous pipe (telemetry)
void *monitor_veiculo_thread(void *arg) {
    pthread_detach(pthread_self());
    ServicoEmCurso *servico = (ServicoEmCurso *)arg;
    
    char buffer[256];
    ssize_t bytes_read;
    int servico_id = servico->id_servico;
    pid_t vehicle_pid = servico->pid_veiculo;
    
    printf("[MONITOR %d] Monitoring vehicle PID %d...\n", servico_id, vehicle_pid);
    
    // Blocking read loop on the anonymous pipe (telemetry)
    while ((bytes_read = read(servico->fd_telemetria, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        printf("--- Telemetry [%d] --- %s\n", servico_id, buffer);
        
        // **LOGIC TO UPDATE SERVICE STATE AND KM HERE**
        if (strstr(buffer, "PROGRESS") != NULL) {
            // Update km_percorridos in the global structure
            // ...
            
        } else if (strstr(buffer, "VEICULO_TERMINATED") != NULL || strstr(buffer, "SERVICE_CANCELLED") != NULL) {
            printf("[MONITOR %d] Vehicle terminated or cancelled.\n", servico_id);
            break;
        }
    }
    
    // Wait for the Vehicle process to finish (reap the child)
    int status;
    waitpid(vehicle_pid, &status, 0);
    
    // Clean up the slot
    pthread_mutex_lock(&frota_mutex);
    servico->estado = 2; // Concluded
    close(servico->fd_telemetria);
    num_servicos_ativos--;
    
    // Update total kilometers for the platform here
    // total_km_platform += ...
    
    pthread_mutex_unlock(&frota_mutex);
    
    return NULL;
}

// Launches the vehicle process and sets up anonymous pipe monitoring
ServicoEmCurso *launch_vehicle(ServicoEmCurso *servico, const char *client_fifo_name, const char *client_username) {
    int pipe_fds[2]; // [0] read end, [1] write end
    if (pipe(pipe_fds) == -1) {
        perror("Error creating anonymous pipe for telemetry");
        return NULL;
    }

    pid_t pid = fork();

    if (pid == -1) {
        perror("Error forking vehicle process");
        close(pipe_fds[0]); close(pipe_fds[1]);
        return NULL;
    }

    if (pid == 0) {
        // Child Process (The Vehicle)
        close(pipe_fds[0]); // Close read end
        
        // Redirect stdout to the write end of the anonymous pipe
        if (dup2(pipe_fds[1], STDOUT_FILENO) == -1) {
            perror("dup2 failed in vehicle");
            exit(EXIT_FAILURE);
        }
        close(pipe_fds[1]); // Close the original write end FD

        // Arguments for the vehicle process: <PID_Cliente> <FIFO_Cliente_Name> <Distancia_Total> <Local_Partida>
        char dist_str[16];
        char client_pid_str[16];
        snprintf(dist_str, sizeof(dist_str), "%d", servico->distancia_total);
        snprintf(client_pid_str, sizeof(client_pid_str), "%d", servico->pid_cliente);
        
        char *vehicle_args[] = {
            "./veiculo",
            client_pid_str,
            (char *)client_fifo_name,
            dist_str,
            "FIXED_START_LOCATION", // Placeholder for actual start location
            NULL
        };

        // Execute the vehicle program
        execve(vehicle_args[0], vehicle_args, NULL);
        
        // If execve returns, there was an error
        perror("Execve failed for vehicle");
        exit(EXIT_FAILURE);
    } else {
        // Parent Process (The Controller)
        close(pipe_fds[1]); // Close write end
        
        // Update service state
        servico->pid_veiculo = pid;
        servico->estado = 1; // In Course
        servico->fd_telemetria = pipe_fds[0]; // Store the read end FD
        
        // Launch a monitoring thread for the telemetry pipe
        pthread_t monitor_tid;
        pthread_create(&monitor_tid, NULL, monitor_veiculo_thread, servico);
        
        return servico;
    }
}

// =========================================================
// 2. ASYNCHRONOUS READING THREADS
// =========================================================

// Thread to simulate time passing and check for scheduled tasks
void *simulated_time_thread(void *arg) {
    pthread_detach(pthread_self());
    
    while (1) {
        sleep(1); // 1 time unit = 1 second
        pthread_mutex_lock(&frota_mutex);
        current_simulated_time++;
        // printf("[TIME SIMULATOR] Hour: %d seconds.\n", current_simulated_time);
        
        // **VEHICLE LAUNCH LOGIC**
        for (int i = 0; i < MAX_CLIENTES; i++) {
            if (frota[i].estado == 0 && frota[i].hora_inicio == current_simulated_time) {
                
                // For simplicity, we use the client's FIFO name here. 
                // In a proper design, the service struct should store the client's FIFO name.
                char client_fifo_name[100];
                sprintf(client_fifo_name, CLIENT_FIFO, frota[i].pid_cliente); 
                
                ServicoEmCurso *launched = launch_vehicle(&frota[i], client_fifo_name, "unknown");
                
                if (launched) {
                    printf("[TIME SIMULATOR] Launching vehicle for Service ID %d (Client PID %d).\n", 
                           frota[i].id_servico, frota[i].pid_cliente);
                } else {
                    printf("[TIME SIMULATOR] ERROR: Failed to launch vehicle for service ID %d.\n", frota[i].id_servico);
                }
            }
        }

        pthread_mutex_unlock(&frota_mutex);
    }
    return NULL;
}


// Thread to read commands from the Administrator (stdin)
void *admin_input_thread(void *arg) {
    pthread_detach(pthread_self());
    char input[128];
    
    printf("Admin Console Ready. Use 'terminar', 'listar', 'frota', 'km', 'hora'.\n");
    
    while (1) {
        printf("Admin> ");
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break; // EOF
        }

        // Clean input
        input[strcspn(input, "\n")] = 0;
        char *p = input;
        while(*p == ' ' || *p == '\t') p++;
        
        // Command parsing
        if (strncasecmp(p, "listar", 6) == 0) {
            // Logic for 'listar' - Show all scheduled services
            // ... 
        } else if (strncasecmp(p, "frota", 5) == 0) {
            // Logic for 'frota' - Show percentage of trip for active vehicles
            // ...
        } else if (strncasecmp(p, "km", 2) == 0) {
            // Logic for 'km' - Show total kilometers
            printf("Total KM Percorridos: %lld\n", total_km_platform);
        } else if (strncasecmp(p, "hora", 4) == 0) {
            // Logic for 'hora' - Show simulated time
            printf("Simulated Time: %d seconds\n", current_simulated_time);
        } else if (strncasecmp(p, "terminar", 8) == 0) {
            // Logic for 'terminar' - Terminate the entire system
            printf("TERMINAR command received. Initiating system shutdown...\n");
            // ... (full shutdown implementation required here) ...
            cleanup(0);
        } else if (strlen(p) > 0) {
            printf("Unknown command.\n");
        }
    }
    printf("[ADMIN THREAD] Terminated.\n");
    return NULL;
}

// =========================================================
// 3. CLIENT MESSAGE PROCESSING (Worker Threads)
// =========================================================

// Utility to find a free service slot
ServicoEmCurso *find_free_service_slot() {
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (frota[i].estado == 2) { // 2 = Concluded/Free
            return &frota[i];
        }
    }
    return NULL;
}

// Thread to process messages received from the CLIENT (FIFO)
void *thread_process_message(void *arg) {
    pthread_detach(pthread_self());
    MensagemT *msg = (MensagemT *)arg;

    // --- LOGIN/LOGOUT Management (Already correct and handles concurrency) ---
    // (Existing login/terminar logic from previous steps)
    // ...
    
    // --- SERVICE COMMANDS ---
    if (strcmp(msg->comando, "agendar") == 0) {
        char hora_str[30], local[100], distancia_str[30];
        
        if (sscanf(msg->msg, "%s %s %s", hora_str, local, distancia_str) == 3) {
            
            pthread_mutex_lock(&frota_mutex);
            ServicoEmCurso *new_service = find_free_service_slot();
            
            if (new_service) {
                // Register new service
                new_service->id_servico = rand() % 1000 + 1; // Simple ID generation
                new_service->pid_cliente = msg->pid;
                new_service->hora_inicio = atoi(hora_str);
                new_service->distancia_total = atoi(distancia_str);
                new_service->estado = 0; // Scheduled
                num_servicos_ativos++;
                
                printf("[AGENDAR] New service %d scheduled for H%d, %d km (Client: %s).\n", 
                       new_service->id_servico, new_service->hora_inicio, new_service->distancia_total, msg->param2);
                
                char response_msg[MAX_MSG];
                snprintf(response_msg, MAX_MSG, "Service ID %d scheduled successfully for H%d.", new_service->id_servico, new_service->hora_inicio);
                send_client_response(msg->pid, response_msg);
            } else {
                send_client_response(msg->pid, "ERROR: Cannot schedule. Fleet is full or max services reached.");
            }
            pthread_mutex_unlock(&frota_mutex);
        } else {
             send_client_response(msg->pid, "ERROR: Incomplete scheduling data.");
        }
    } else {
        // Handle consultar, cancelar, entrar, sair...
    }
    
    free(msg);
    return NULL;
}

// =========================================================
// 4. MAIN - SETUP
// =========================================================

int main() {
    // Initialization
    srand(time(NULL));
    for (int i = 0; i < MAX_CLIENTES; i++) {
        clientes[i].active = 0;
        frota[i].estado = 2; // Concluded/Free
    }
    pthread_mutex_init(&clientes_mutex, NULL);
    pthread_mutex_init(&frota_mutex, NULL);
    pthread_mutex_init(&console_mutex, NULL);

    // 1. FIFO Creation and SIGINT Configuration
    if (mkfifo(SERVER_FIFO, 0666) == -1 && errno != EEXIST) {
        perror("Failed to create server FIFO");
        exit(EXIT_FAILURE);
    }
    struct sigaction sa;
    sa.sa_handler = cleanup;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    printf("Controller is running. Waiting for client messages...\n");

    // 2. Open Server FIFO for Persistent Read/Write (O_RDWR)
    fd_servidor_main = open(SERVER_FIFO, O_RDWR); 
    if (fd_servidor_main == -1) {
        perror("Failed to open server FIFO for R/W");
        unlink(SERVER_FIFO);
        exit(EXIT_FAILURE);
    }

    // 3. Launch Functionality Threads
    pthread_t admin_tid, time_tid;
    pthread_create(&admin_tid, NULL, admin_input_thread, NULL); 
    pthread_create(&time_tid, NULL, simulated_time_thread, NULL);

    // 4. Loop Principal de Leitura do FIFO dos Clientes (Bloqueante)
    while (1) {
        MensagemT *msg = malloc(sizeof(MensagemT));
        if (!msg) {
            perror("malloc");
            continue;
        }

        // Blocking read: waits for a client message
        ssize_t bytes_read = read(fd_servidor_main, msg, sizeof(MensagemT));
        
        if (bytes_read > 0) {
            // Launch worker thread to process the request
            pthread_t thread_id;
            if (pthread_create(&thread_id, NULL, thread_process_message, msg) != 0) {
                perror("pthread_create failed");
                free(msg);
            }
        } else if (bytes_read == -1 && errno != EINTR) {
            // Fatal error
            perror("Fatal error reading server FIFO");
            free(msg);
            break;
        } else {
            // EINTR (signal interruption) or bytes_read == 0: just continue
            free(msg);
        }
    }

    // Cleanup on fatal loop break
    close(fd_servidor_main);
    unlink(SERVER_FIFO);
    pthread_mutex_destroy(&clientes_mutex);
    pthread_mutex_destroy(&frota_mutex);
    pthread_mutex_destroy(&console_mutex);

    return 0;
}