# ------------------------------------------
# VARIABLES DE CONFIGURACIÓN

CC = gcc
# -Wall -Wextra -Wpedantic: Avisos estrictos y -std=c11: el estándar C11
CFLAGS = -Wall -Wextra -Wpedantic -std=c11

# Rutas de los archivos fuente
SRC_CLIENTE = src/cliente/cliente.c
SRC_SERVIDOR = src/servidor/servidor.c

# Nombres de los ejecutables finales
TARGET_CLIENTE = cliente
TARGET_SERVIDOR = servidor

# ------------------------------------------
# REGLAS PRINCIPALES

# Al escribir "make", se compilan
all: $(TARGET_CLIENTE) $(TARGET_SERVIDOR)

# Regla para compilar el cliente
$(TARGET_CLIENTE): $(SRC_CLIENTE)
	$(CC) $(CFLAGS) $(SRC_CLIENTE) -o $(TARGET_CLIENTE)

# Regla para compilar el servidor
$(TARGET_SERVIDOR): $(SRC_SERVIDOR)
	$(CC) $(CFLAGS) $(SRC_SERVIDOR) -o $(TARGET_SERVIDOR)

# ------------------------------------------
# REGLAS DE MANTENIMIENTO

# Modo debug (con info extra para buscar errores)
debug: CFLAGS += -g -O0
debug: all

# Limpiar todos los ejecutables generados
clean:
	rm -f $(TARGET_CLIENTE) $(TARGET_SERVIDOR)