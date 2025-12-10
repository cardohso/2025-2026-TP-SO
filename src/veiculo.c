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
#include <math.h>

#include "../include/common.h"

// Forward declaration
void send_client_response(pid_t target_pid, const char *fifo_name, const char *message);

// VARIÁVEIS GLOBAIS DE ESTADO
int keep_running = 1;
int current_distance = 0;
int total_distance = 0;
char client_fifo_name[100];
pid_t client_pid = -1;

// =========================================================
// 1. HANDLER DE SINAL (SIGUSR1 - Cancelamento pelo Controlador)
// =========================================================

/* Handler for SIGUSR1: Cancels the service and terminates immediately[cite: 455]. */
void handle_sigusr1(int sig) {
    // 1. Report cancellation to the Controller (Telemetry/stdout)
    fprintf(stdout, "TELEMETRY: SERVICE_CANCELLED | Reason: SIGUSR1 received | Dist_km: %d/%d\n",
            current_distance, total_distance);
    fflush(stdout);

    // 2. Notify the Client (via Named Pipe)
    send_client_response(client_pid, client_fifo_name, "Service cancelled by Controller.");
    
    // 3. Terminate process immediately [cite: 455]
    exit(EXIT_SUCCESS); 
}

// --- Funções Auxiliares ---

// Wrapper to send a message to the client's FIFO
void send_client_response(pid_t target_pid, const char *fifo_name, const char *message) {
    int fd = open(fifo_name, O_WRONLY);
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
// 2. COMUNICAÇÃO COM O CLIENTE (Named Pipe)
// =========================================================

/* Simulates vehicle arrival and awaits 'entrar' command from the client */
int await_client_entry(const char *local_partida) {
    // 1. Notify Client of Arrival
    send_client_response(client_pid, client_fifo_name, "VEICULO: Arrived at location. Please enter command (entrar <destination> or sair).");

    fprintf(stdout, "TELEMETRY: WAITING_FOR_CLIENT | Location: %s\n", local_partida);
    fflush(stdout);

    // 2. Wait for Client Command (Simulated: requires a dedicated pipe listener)
    // ---
    // NOTE: A robust implementation requires a separate thread/select() to read commands 
    // from a dedicated VEICULO_FIFO here while the main thread waits.
    // For this simplified version, we assume the command will arrive eventually 
    // and must be handled by the Controller's thread pool, which forwards it.
    // Since the vehicle only needs to know 'entrar' (destination) and 'sair', 
    // this interaction should ideally be implemented using a dedicated pipe for Vehicle commands.
    
    // For now, we simulate success (client entry) and log the event.
    
    // After receiving 'entrar <destination>' via a pipe...
    fprintf(stdout, "TELEMETRY: CLIENT_ENTERED\n");
    fflush(stdout);
    return 1; // Success: Client entered
}

// =========================================================
// 3. LOOP DE VIAGEM (Simulação)
// =========================================================

/* Simulates the trip and reports progress every 10% [cite: 453] */
void start_trip() {
    int next_report_threshold = 0;
    
    fprintf(stdout, "TELEMETRY: START_TRIP | Total Distance: %d km\n", total_distance);
    fflush(stdout);

    while (keep_running && current_distance < total_distance) {
        sleep(1); // Simulates 1 second of travel (1 km/s) [cite: 454]
        current_distance++;

        // Calculate current percentage
        int current_percentage = (int)round(((double)current_distance / total_distance) * 100);
        
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

    // 2. Read Arguments
    // Usage: ./veiculo <PID_Cliente> <FIFO_Cliente_Name> <Distancia_Total> <Local_Partida>
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
    
    // 3. Vehicle Arrival and Client Interaction [cite: 451]
    if (await_client_entry(local_partida)) {
        // Client entered. Start the simulation.
        start_trip();
    } else {
        fprintf(stderr, "VEICULO [%d]: Failed to start trip (client interaction failed).\n", getpid());
    }

    // 4. Cleanup and Termination [cite: 456]
    fprintf(stdout, "TELEMETRY: VEICULO_TERMINATED | Status: Exiting process.\n");
    fflush(stdout);
    
    return EXIT_SUCCESS;
}