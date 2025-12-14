CC = gcc
CFLAGS = -Wall -Wextra -pthread -D_XOPEN_SOURCE=700
SRC_DIR = src
INCLUDE_DIR = include
BIN_DIR = src

# Source files
CLIENTE_SRC = $(SRC_DIR)/cliente.c
VEICULO_SRC = $(SRC_DIR)/veiculo.c
CONTROLADOR_SRC = $(SRC_DIR)/controlador.c

# Object files
CLIENTE_OBJ = $(BIN_DIR)/cliente.o
VEICULO_OBJ = $(BIN_DIR)/veiculo.o
CONTROLADOR_OBJ = $(BIN_DIR)/controlador.o

# Executables
CLIENTE_BIN = $(BIN_DIR)/cliente
VEICULO_BIN = $(BIN_DIR)/veiculo
CONTROLADOR_BIN = $(BIN_DIR)/controlador

# Targets
.PHONY: all clean help

all: $(CLIENTE_BIN) $(VEICULO_BIN) $(CONTROLADOR_BIN)

$(CLIENTE_BIN): $(CLIENTE_SRC)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) $< -o $@
	@echo "✓ Compiled cliente"

$(VEICULO_BIN): $(VEICULO_SRC)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) $< -o $@
	@echo "✓ Compiled veiculo"

$(CONTROLADOR_BIN): $(CONTROLADOR_SRC)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) $< -o $@
	@echo "✓ Compiled controlador"

clean:
	rm -f $(CLIENTE_BIN) $(VEICULO_BIN) $(CONTROLADOR_BIN)
	rm -f FIFOSERVIDOR FIFOCLIENTE* FIFOVEICULO*
	rm -f /tmp/controlador.lock
	@echo "✓ Cleaned up"

help:
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build all programs (cliente, veiculo, controlador)"
	@echo "  clean      - Remove all compiled files and FIFOs"
	@echo "  help       - Show this help message"
	@echo ""
	@echo "Running the programs:"
	@echo "  Terminal 1: ./src/controlador"
	@echo "  Terminal 2: ./src/cliente <username>"
	@echo "  Terminal 3: ./src/cliente <username>"

.SILENT: help
