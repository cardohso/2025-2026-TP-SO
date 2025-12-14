#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#include "../include/common.h"

// Forward declaration
void send_client_response(pid_t target_pid, const char *fifo_name, const char *message);

// GLOBAL STATE VARIABLES (for SIGUSR1 handler)
int keep_running = 1;
int current_distance = 0;
int total_distance = 0;
char client_fifo_name[100];
pid_t client_pid = -1;
char vehicle_fifo_name[100];
int vehicle_fifo_fd = -1;

// =========================================================
// 1. HANDLER DE SINAL (SIGUSR1 - Cancellation by Controller)
// =========================================================

/* Handler for SIGUSR1: Cancels the service and terminates immediately. */
void handle_sigusr1(int sig) {
    // 1. Report cancellation to the Controller (Telemetry/stdout)
    fprintf(stdout, "TELEMETRY: SERVICE_CANCELLED | Reason: SIGUSR1 received | Dist_km: %d/%d\n",
            current_distance, total_distance);
    fflush(stdout);

    // 2. Notify the Client (via Named Pipe)
    send_client_response(client_pid, client_fifo_name, "Service cancelled by Controller.");
    
    // 3. Terminate process immediately 
    exit(EXIT_SUCCESS); 
}

// Wrapper to send a message to the client's FIFO
void send_client_response(pid_t target_pid, const char *fifo_name, const char *message) {
    // The client is expected to have its FIFO open for reading.
    int fd = open(fifo_name, O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        // Client might have terminated; log the error but continue
        fprintf(stderr, "VEICULO [%d]: Warning: Failed to open Client FIFO %s: %s\n", 
                getpid(), fifo_name, strerror(errno));
        return;
    }

    MensagemT response;
    response.pid = getpid();
    strcpy(response.comando, "vehicle_notify");
    strncpy(response.msg, message, MAX_MSG);
    response.msg[MAX_MSG - 1] = '\0';
    
    write(fd, &response, sizeof(MensagemT));
    close(fd);
}

// =========================================================
// 2. CLIENT INTERACTION
// =========================================================

/* Blocks and waits for client's 'entrar' or 'sair' command via the server FIFO */
int await_client_entry(const char *local_partida) {
    // 1. Notify Client of Arrival
    send_client_response(client_pid, client_fifo_name, "VEICULO: Arrived at location. Please enter command (entrar <destination> or sair).");

    // Report arrival event to Controller (stdout)
    fprintf(stdout, "TELEMETRY: ARRIVED_AT_START | Location: %s\n", local_partida);
    fflush(stdout);

    // 2. Open the SERVER FIFO to read client's command
    // The controller should redirect the client's entrar/sair command to us
    // For direct communication, we'll read from a dedicated pipe or wait for signal
    
    char vehicle_fifo_name[100];
    sprintf(vehicle_fifo_name, "FIFOVEICULO%d", getpid());
    
    // Create vehicle's temporary FIFO
    if (mkfifo(vehicle_fifo_name, 0666) == -1 && errno != EEXIST) {
        perror("Error creating vehicle FIFO");
        return 0;
    }
    
    // Notify controller about the vehicle FIFO
    fprintf(stdout, "TELEMETRY: WAITING_FOR_CLIENT | FIFO: %s\n", vehicle_fifo_name);
    fflush(stdout);
    
    // Open FIFO for reading (blocks until client opens for writing)
    int fd_vehicle = open(vehicle_fifo_name, O_RDONLY);
    if (fd_vehicle == -1) {
        perror("Error opening vehicle FIFO");
        unlink(vehicle_fifo_name);
        return 0;
    }
    
    // Read command from client
    MensagemT cmd_msg;
    ssize_t bytes_read = read(fd_vehicle, &cmd_msg, sizeof(MensagemT));
    
    close(fd_vehicle);
    unlink(vehicle_fifo_name);
    
    if (bytes_read > 0) {
        if (strcmp(cmd_msg.comando, "entrar") == 0) {
            fprintf(stdout, "TELEMETRY: CLIENT_ENTERED | Destination: %s\n", cmd_msg.msg);
            fflush(stdout);
            send_client_response(client_pid, client_fifo_name, "VEICULO: Client entered. Starting trip...");
            return 1;
        } else if (strcmp(cmd_msg.comando, "sair") == 0) {
            fprintf(stdout, "TELEMETRY: CLIENT_REFUSED_ENTRY | Reason: Client refused entry\n");
            fflush(stdout);
            send_client_response(client_pid, client_fifo_name, "VEICULO: Client exited. Service cancelled.");
            return 0;
        }
    }
    
    fprintf(stderr, "VEICULO [%d]: No valid command received from client.\n", getpid());
    return 0;
}

// =========================================================
// 3. TRIP SIMULATION
// =========================================================

/* Simulates the trip and reports progress every 10% */
void start_trip() {
    int next_report_threshold = 0;
    
    fprintf(stdout, "TELEMETRY: START_TRIP | Total Distance: %d km\n", total_distance);
    fflush(stdout);

    while (keep_running && current_distance < total_distance) {
        // Calculate current percentage
        int current_percentage = (current_distance * 100) / total_distance;
        
        // Check for 10% progress threshold
        if (current_percentage >= next_report_threshold || current_distance == total_distance) {
            next_report_threshold = current_percentage + 10;
            
            // Report progress to the Controller (stdout)
            fprintf(stdout, "TELEMETRY: PROGRESS | %%_completed: %d | Dist_km: %d/%d\n", 
                    current_percentage, current_distance, total_distance);
            fflush(stdout); 
            
            // Check for destination reached
            if (current_distance >= total_distance) {
                fprintf(stdout, "TELEMETRY: SERVICE_FINISHED | Reason: Reached destination\n");
                send_client_response(client_pid, client_fifo_name, "VIAGEM: Destination reached. Service concluded.");
                keep_running = 0;
                break;
            }
        }
        sleep(1);
        current_distance++;
    }
    
    // If trip was interrupted (client exited), report the km done before terminating
    if (!keep_running || current_distance < total_distance) {
        fprintf(stdout, "TELEMETRY: TRIP_INTERRUPTED | Dist_km_done: %d/%d\n", 
                current_distance, total_distance);
        fflush(stdout);
    }
}

// =========================================================
// 4. MAIN
// =========================================================

int main(int argc, char *argv[]) {
    // 1. Setup Signal Handler (SIGUSR1)
    struct sigaction sa;
    sa.sa_handler = handle_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        perror("Error setting up SIGUSR1 handler");
        return EXIT_FAILURE;
    }

    // 2. Read Arguments: <PID_Cliente> <FIFO_Cliente_Name> <Distancia_Total> <Local_Partida>
    if (argc != 5) {
        fprintf(stderr, "VEICULO: Usage: %s <PID_Client> <FIFO_Client_Name> <Total_Distance> <Start_Location>\n", argv[0]);
        return EXIT_FAILURE;
    }

    client_pid = atoi(argv[1]);
    strncpy(client_fifo_name, argv[2], 100);
    total_distance = atoi(argv[3]);
    char *local_partida = argv[4];

    fprintf(stdout, "TELEMETRY: VEICULO_LAUNCHED | PID: %d | Service for Client: %d | Dist: %d km | Start: %s\n", 
            getpid(), client_pid, total_distance, local_partida);
    fflush(stdout);
    
    // 3. Vehicle Arrival and Client Interaction
    if (await_client_entry(local_partida)) {
        start_trip();
    } else {
        fprintf(stderr, "VEICULO [%d]: Failed to start trip.\n", getpid());
    }

    // 4. Cleanup and Termination
    fprintf(stdout, "TELEMETRY: VEICULO_TERMINATED | Status: Exiting process.\n");
    fflush(stdout);
    
    return EXIT_SUCCESS;
}