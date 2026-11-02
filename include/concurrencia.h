#ifndef CONCURRENCIA_H
#define CONCURRENCIA_H
#include <stddef.h>
#include "catalog.h"
#include "canal.h"
#include "protocol.h"

typedef struct {
    int fd_escucha;
    const catalog_t *catalog;
    const canal_t *canal;
    size_t max_clientes;       /* limite de clientes activos (RF-20); ignorado en FIFO */
} contexto_servidor_t;

int modo_fifo_ejecutar(const contexto_servidor_t *ctx);
int modo_forked_ejecutar(const contexto_servidor_t *ctx);
int modo_threaded_ejecutar(const contexto_servidor_t *ctx);

void concurrencia_rechazar(int fd, protocol_status_t status, const char *texto);

int concurrencia_esperar_conexion(int fd_escucha, int timeout_ms);

/* Pausa corta (nanosleep; usleep no existe en POSIX.1-2008). */
void concurrencia_pausa_ms(int ms);

#define CONCURRENCIA_REBANADA_MS 500
#define CONCURRENCIA_GRACIA_MS   3000   /* plazo para cierre ordenado (RFx-52.4, RF-58) */

#endif