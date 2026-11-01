#ifndef VOD_H
#define VOD_H

#include <stdint.h>
#include <stdbool.h>
#include "catalog.h"
#include "protocol.h"

// Estructura que representa una transferencia VOD.
typedef struct {
    int fd_archivo;
    uint64_t tamano;
    uint64_t enviados;
    uint32_t request_id;
    bool fin_enviado;  // Nuevo: evita enviar FIN dos veces.
} vod_transfer_t;

// Posibles resultados al enviar un bloque VOD.
typedef enum {
    VOD_BLOQUE_ENVIADO,
    VOD_FINALIZADO,
    VOD_ERROR_LECTURA,
    VOD_ERROR_RED,
    VOD_ERROR_ESTADO
} vod_result_t;

// Prepara una transferencia utilizando el catálogo.
catalog_result_t vod_preparar(
    const catalog_t *catalog,
    const char *nombre,
    uint32_t request_id,
    vod_transfer_t *transfer
);

// Envía la confirmación ACEPTADO al cliente.
protocol_result_t vod_enviar_aceptado(
    int fd_cliente,
    const vod_transfer_t *transfer
);

// Envía el siguiente bloque DATOS o FIN.
vod_result_t vod_enviar_siguiente(
    int fd_cliente,
    vod_transfer_t *transfer
);

// Cierra el archivo y libera los recursos.
void vod_liberar(vod_transfer_t *transfer);

#endif // VOD_H

