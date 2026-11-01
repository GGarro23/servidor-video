//
// Created by diego on 10/31/2026.
//

#ifndef SERVIDOR_VIDEO_SERVIDOR_H
#define SERVIDOR_VIDEO_SERVIDOR_H

#include <stdint.h>
#include "catalog.h"

typedef enum {
    MODO_FIFO,
    MODO_FORKED,
    MODO_THREADED
} modo_concurrencia_t;

typedef struct {
    modo_concurrencia_t modo;
    char direccion[128];
    uint16_t puerto;
    int backlog;
} config_servidor_t;

#define SERVICIO_VOD  0x01u
#define SERVICIO_LIVE 0x02u

int red_crear_escucha(const config_servidor_t *config);

void atender_cliente(
    int fd_cliente,
    const catalog_t *catalog
);

#endif //SERVIDOR_VIDEO_SERVIDOR_H
