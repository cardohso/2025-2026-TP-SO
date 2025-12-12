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
    int fd_telemetria;  // Read end FD of the anonymous pipe
    char vehicle_fifo_name[100]; // FIFO name for client-vehicle direct communication
} ServicoEmCurso;

ServicoEmCurso frota[MAX_CLIENTES]; 
int num_servicos_ativos = 0;
long long int total_km_platform = 0; // Total km counter for the platform

// --- File Descriptor for the main Server FIFO ---
int fd_servidor_main = -1;
// File descriptor for the administration console (stdin)
int fd_admin_input = 0; // stdin file descriptor is 0

// --- Lock File Descriptor to ensure single instance ---
int lock_fd = -1;

// --- Control flag for main loop ---
volatile sig_atomic_t keep_running = 1;

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
    keep_running = 0; // Signal main loop to stop
    printf("\nClosing server and cleaning up resources (SIGINT received)...\n");

    // 1. Notify all active clients
    pthread_mutex_lock(&clientes_mutex);
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (clientes[i].active) {
            printf("[CLEANUP] Notifying client %s (PID %d)...\n", clientes[i].username, clientes[i].pid);
            send_client_response(clientes[i].pid, "Server is shutting down. Goodbye!");
        }
    }
    pthread_mutex_unlock(&clientes_mutex);
    
    // 1.5. Scan for and remove any leftover FIFOCLIENTE files
    printf("[CLEANUP] Scanning for leftover client FIFOs...\n");
    for (int i = 1; i < 100000; i++) { // Check a reasonable range of PIDs
        char client_fifo_name[100];
        sprintf(client_fifo_name, CLIENT_FIFO, i);
        
        // Check if FIFO exists using access()
        if (access(client_fifo_name, F_OK) == 0) {
            if (unlink(client_fifo_name) == 0) {
                printf("[CLEANUP] Removed leftover FIFO: %s\n", client_fifo_name);
            }
        }
    }

    // 2. Terminate all running vehicles
    pthread_mutex_lock(&frota_mutex);
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (frota[i].estado == 1 && frota[i].pid_veiculo > 0) {
            printf("[CLEANUP] Terminating vehicle PID %d (Service %d)...\n", 
                   frota[i].pid_veiculo, frota[i].id_servico);
            kill(frota[i].pid_veiculo, SIGTERM);
            waitpid(frota[i].pid_veiculo, NULL, WNOHANG); // Non-blocking wait
        }
        if (frota[i].fd_telemetria > 0) {
            close(frota[i].fd_telemetria);
        }
        // Clean up vehicle FIFOs
        if (frota[i].vehicle_fifo_name[0] != '\0') {
            unlink(frota[i].vehicle_fifo_name);
        }
    }
    pthread_mutex_unlock(&frota_mutex);

    // 3. Clean up server FIFO and file descriptors
    if (fd_servidor_main != -1) {
        close(fd_servidor_main);
        fd_servidor_main = -1;
    }
    unlink(SERVER_FIFO);
    
    // 3.5. Release lock file
    if (lock_fd != -1) {
        close(lock_fd);
        unlink(LOCK_FILE);
    }
    
    // 4. Destroy mutexes
    pthread_mutex_destroy(&clientes_mutex);
    pthread_mutex_destroy(&frota_mutex);
    pthread_mutex_destroy(&console_mutex);

    printf("[CLEANUP] Server terminated successfully.\n");
    _exit(0); // Force immediate termination
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
        
        // Check if this is the WAITING_FOR_CLIENT message with FIFO name
        if (strstr(buffer, "WAITING_FOR_CLIENT | FIFO:") != NULL) {
            char *fifo_ptr = strstr(buffer, "FIFOVEICULO");
            if (fifo_ptr) {
                pthread_mutex_lock(&frota_mutex);
                sscanf(fifo_ptr, "%s", servico->vehicle_fifo_name);
                pthread_mutex_unlock(&frota_mutex);
                // Notify client about the vehicle FIFO
                char msg_to_client[MAX_MSG];
                snprintf(msg_to_client, MAX_MSG, "VEHICLE_READY: Use FIFO %s for entrar/sair commands.", servico->vehicle_fifo_name);
                send_client_response(servico->pid_cliente, msg_to_client);
            }
        }
        
        printf("--- Telemetry [%d] --- %s\n", servico_id, buffer);
        
        // **LOGIC TO UPDATE SERVICE STATE AND km HERE**
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
        
        // CRITICAL: Only lock frota_mutex (no client access needed)
        pthread_mutex_lock(&frota_mutex);
        current_simulated_time++;
        
        // **VEHICLE LAUNCH LOGIC**
        for (int i = 0; i < MAX_CLIENTES; i++) {
            if (frota[i].estado == 0 && frota[i].hora_inicio == current_simulated_time) {
                
                char client_fifo_name[100];
                sprintf(client_fifo_name, CLIENT_FIFO, frota[i].pid_cliente); 
                
                // Launch vehicle (this is safe - it forks and doesn't access shared data)
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
    
    printf("Admin Console Ready. Use 'listar', 'frota', 'km', 'hora', 'terminar'.\n");
    
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
            printf("\n--- LIST OF SCHEDULED/IN PROGRESS SERVICES ---\n");
            printf(" ID \t| TIME \t| CLIENT PID \t| DIST(km) \t| VEHICLE PID \t| STATUS\n");
            printf("-------------------------------------------------------------------------\n");

            pthread_mutex_lock(&frota_mutex);
            int found = 0;
            
            for (int i = 0; i < MAX_CLIENTES; i++) {
                if (frota[i].estado == 0 || frota[i].estado == 1) {
                    const char *estado_str = (frota[i].estado == 0) ? "SCHEDULED" : "IN PROGRESS";
                    printf(" %d \t| H%d \t| %d \t| %d \t\t| %d \t\t| %s\n",
                           frota[i].id_servico,
                           frota[i].hora_inicio,
                           frota[i].pid_cliente,
                           frota[i].distancia_total,
                           frota[i].pid_veiculo > 0 ? frota[i].pid_veiculo : 0, 
                           estado_str);
                    found++;
                }
            }

            if (found == 0) {
                printf("No scheduled or in progress services.\n");
            }
            pthread_mutex_unlock(&frota_mutex);
            printf("-------------------------------------------------------------------------\n");
        } else if (strncasecmp(p, "frota", 5) == 0) {
            // Logic for 'frota' - Show percentage of trip for active vehicles
            printf("\n--- ACTIVE FLEET MONITORING ---\n");
            printf(" ID \t| VEHICLE PID \t| km TOTAL \t| PROGRESS\n");
            printf("-----------------------------------------------------\n");

            pthread_mutex_lock(&frota_mutex);
            int active_vehicles = 0;

            for (int i = 0; i < MAX_CLIENTES; i++) {
                if (frota[i].estado == 1 && frota[i].pid_veiculo > 0) { // Only vehicles IN COURSE
                    int percentage = 0;
                    if (frota[i].distancia_total > 0) {
                        percentage = (frota[i].km_percorridos * 100) / frota[i].distancia_total;
                    }
                    
                    printf(" %d \t| %d \t| %d km \t| %d%% (%d/%d km)\n",
                           frota[i].id_servico,
                           frota[i].pid_veiculo,
                           frota[i].distancia_total,
                           percentage,
                           frota[i].km_percorridos,
                           frota[i].distancia_total);
                    active_vehicles++;
                }
            }

            if (active_vehicles == 0) {
                printf("No vehicles in progress.\n");
            }
            pthread_mutex_unlock(&frota_mutex);
            printf("-----------------------------------------------------\n");
        } else if (strncasecmp(p, "km", 2) == 0) {
            // Logic for 'km' - Show total kilometers
            printf("Total km Done: %lld km\n", total_km_platform);
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

// Thread to process messages received from the CLIENT (FIFO)
void *thread_process_message(void *arg) {
    pthread_detach(pthread_self());
    MensagemT *msg = (MensagemT *)arg;

    // --- LOGIN/LOGOUT Management ---
    if (strcmp(msg->comando, "login") == 0) {
        pthread_mutex_lock(&clientes_mutex);
        
        // 1. Check if username is already active
        int username_duplicate = 0;
        int pid_duplicate = 0;
        int active_count = 0;
        
        for (int i = 0; i < MAX_CLIENTES; i++) {
            if (clientes[i].active) {
                active_count++;
                if (strcmp(clientes[i].username, msg->param2) == 0) {
                    username_duplicate = 1;
                }
                if (clientes[i].pid == msg->pid) {
                    pid_duplicate = 1;
                }
            }
        }
        
        // 2. Check if maximum user limit (30) has been reached
        if (active_count >= MAX_CLIENTES) {
            pthread_mutex_unlock(&clientes_mutex);
            send_client_response(msg->pid, "Login failed: Maximum number of users (30) reached.");
            printf("[LOGIN REJECTED] User limit reached (%d/%d). Username: %s (PID %d)\n", 
                   active_count, MAX_CLIENTES, msg->param2, msg->pid);
        } else if (username_duplicate) {
            pthread_mutex_unlock(&clientes_mutex);
            send_client_response(msg->pid, "Login failed: Username already in use.");
            printf("[LOGIN REJECTED] Username '%s' already active (PID %d tried to login).\n", 
                   msg->param2, msg->pid);
        } else if (pid_duplicate) {
            pthread_mutex_unlock(&clientes_mutex);
            send_client_response(msg->pid, "Login failed: PID already in use.");
            printf("[LOGIN REJECTED] PID %d already has an active session.\n", msg->pid);
        } else {
            // Find free slot
            int slot = -1;
            for (int i = 0; i < MAX_CLIENTES; i++) {
                if (!clientes[i].active) {
                    slot = i;
                    break;
                }
            }
            
            if (slot != -1) {
                clientes[slot].pid = msg->pid;
                strncpy(clientes[slot].username, msg->param2, TAM_USERNAME);
                clientes[slot].active = 1;
                pthread_mutex_unlock(&clientes_mutex);
                
                printf("[LOGIN] Client %s (PID %d) connected. Active users: %d/%d\n", 
                       msg->param2, msg->pid, active_count + 1, MAX_CLIENTES);
                send_client_response(msg->pid, "Login OK. Welcome!");
            } else {
                pthread_mutex_unlock(&clientes_mutex);
                send_client_response(msg->pid, "Login failed: Server is full.");
                printf("[LOGIN REJECTED] No free slots available.\n");
            }
        }
        free(msg);
        return NULL;
    }
    
    if (strcmp(msg->comando, "terminar") == 0) {
        // CRITICAL: Lock order is clientes_mutex THEN frota_mutex
        pthread_mutex_lock(&clientes_mutex);
        pthread_mutex_lock(&frota_mutex);
        
        // Find and deactivate client
        char client_username[TAM_USERNAME] = "";
        int found_client = 0;
        
        for (int i = 0; i < MAX_CLIENTES; i++) {
            if (clientes[i].active && clientes[i].pid == msg->pid) {
                strncpy(client_username, clientes[i].username, TAM_USERNAME);
                clientes[i].active = 0;
                found_client = 1;
                
                // Cancel ALL services (scheduled AND in course) for this client
                int cancelled_count = 0;
                for (int j = 0; j < MAX_CLIENTES; j++) {
                    if (frota[j].pid_cliente == msg->pid) {
                        if (frota[j].estado == 0) {
                            // Scheduled service - just free the slot
                            frota[j].estado = 2;
                            num_servicos_ativos--;
                            cancelled_count++;
                            printf("[LOGOUT] Cancelled scheduled service %d for client %s.\n", 
                                   frota[j].id_servico, client_username);
                        } else if (frota[j].estado == 1) {
                            // Service in course - terminate the vehicle
                            if (frota[j].pid_veiculo > 0) {
                                printf("[LOGOUT] Terminating vehicle PID %d (Service %d) for client %s.\n", 
                                       frota[j].pid_veiculo, frota[j].id_servico, client_username);
                                kill(frota[j].pid_veiculo, SIGTERM);
                            }
                            frota[j].estado = 2;
                            num_servicos_ativos--;
                            cancelled_count++;
                        }
                    }
                }
                
                printf("[LOGOUT] Client %s (PID %d) disconnected. Services cancelled: %d\n", 
                       client_username, msg->pid, cancelled_count);
                break;
            }
        }
        
        if (!found_client) {
            printf("[LOGOUT WARNING] Client PID %d not found in active list.\n", msg->pid);
        }
        
        pthread_mutex_unlock(&frota_mutex);
        pthread_mutex_unlock(&clientes_mutex);
        free(msg);
        return NULL;
    }
    
    // --- SERVICE COMMANDS ---
    if (strcmp(msg->comando, "agendar") == 0) {
        char hora_str[30], local[100], distancia_str[30];
        
        if (sscanf(msg->msg, "%s %s %s", hora_str, local, distancia_str) == 3) {
            
            // CRITICAL: Lock order is clientes_mutex THEN frota_mutex (if needed)
            pthread_mutex_lock(&frota_mutex);
            
            int requested_hour = atoi(hora_str);
            
            // Check if the requested hour has already passed
            if (requested_hour < current_simulated_time) {
                pthread_mutex_unlock(&frota_mutex);
                char error_msg[MAX_MSG];
                snprintf(error_msg, MAX_MSG, "ERROR: Cannot schedule for H%d. Current time is H%d. Please schedule for a future time.", 
                         requested_hour, current_simulated_time);
                send_client_response(msg->pid, error_msg);
                printf("[AGENDAR REJECTED] Client PID %d tried to schedule for past time H%d (current: H%d).\n", 
                       msg->pid, requested_hour, current_simulated_time);
            } else {
                // Find free service slot (must be called inside lock)
                ServicoEmCurso *new_service = NULL;
                for (int i = 0; i < MAX_CLIENTES; i++) {
                    if (frota[i].estado == 2) { // 2 = Concluded/Free
                        new_service = &frota[i];
                        break;
                    }
                }
                
                if (new_service) {
                    // Register new service
                    new_service->id_servico = rand() % 1000 + 1; // Simple ID generation
                    new_service->pid_cliente = msg->pid;
                    new_service->hora_inicio = requested_hour;
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
            }
        } else {
             send_client_response(msg->pid, "ERROR: Incomplete scheduling data.");
        }
    } else if (strcmp(msg->comando, "entrar") == 0 || strcmp(msg->comando, "sair") == 0) {
        // Find the active service for this client
        // Only need frota_mutex here (no client info access)
        pthread_mutex_lock(&frota_mutex);
        
        int found = 0;
        char vehicle_fifo_copy[100] = "";
        
        for (int i = 0; i < MAX_CLIENTES; i++) {
            if (frota[i].pid_cliente == msg->pid && frota[i].estado == 1 && frota[i].vehicle_fifo_name[0] != '\0') {
                // Copy FIFO name while holding lock
                strncpy(vehicle_fifo_copy, frota[i].vehicle_fifo_name, sizeof(vehicle_fifo_copy));
                found = 1;
                break;
            }
        }
        
        pthread_mutex_unlock(&frota_mutex);
        
        // Perform I/O operations OUTSIDE the lock to avoid blocking other threads
        if (found) {
            int fd_vehicle = open(vehicle_fifo_copy, O_WRONLY | O_NONBLOCK);
            if (fd_vehicle != -1) {
                write(fd_vehicle, msg, sizeof(MensagemT));
                close(fd_vehicle);
            } else {
                send_client_response(msg->pid, "ERROR: Cannot communicate with vehicle.");
            }
        } else {
            send_client_response(msg->pid, "ERROR: No active vehicle waiting for your command.");
        }
    } else {
        // Handle consultar, cancelar, other commands...
    }
    
    free(msg);
    return NULL;
}

// =========================================================
// 4. MAIN - SETUP
// =========================================================

int main() {
    // 0. Ensure only one instance is running (lock mechanism)
    lock_fd = open(LOCK_FILE, O_CREAT | O_RDWR, 0666);
    if (lock_fd == -1) {
        perror("Failed to create lock file");
        exit(EXIT_FAILURE);
    }
    
    // Try to acquire exclusive lock
    if (lockf(lock_fd, F_TLOCK, 0) == -1) {
        fprintf(stderr, "ERROR: Another instance of controlador is already running.\n");
        close(lock_fd);
        exit(EXIT_FAILURE);
    }
    
    printf("Lock acquired. Starting controlador...\n");
    
    // Initialization
    srand(time(NULL));
    for (int i = 0; i < MAX_CLIENTES; i++) {
        clientes[i].active = 0;
        frota[i].estado = 2; // Concluded/Free
        frota[i].vehicle_fifo_name[0] = '\0'; // Initialize vehicle FIFO name
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
    while (keep_running) {
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
    
    if (lock_fd != -1) {
        close(lock_fd);
        unlink(LOCK_FILE);
    }
    
    pthread_mutex_destroy(&clientes_mutex);
    pthread_mutex_destroy(&frota_mutex);
    pthread_mutex_destroy(&console_mutex);

    return 0;
}