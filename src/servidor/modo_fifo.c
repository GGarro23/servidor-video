#define _POSIX_C_SOURCE 200809L

#include "concurrencia.h"
#include "servidor.h"

#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

/* RF-17/RF-50: una conexion a la vez; las demas esperan en la cola del SO. */
int modo_fifo_ejecutar(const contexto_servidor_t *ctx) {
    while (!servidor_detencion_solicitada()) {
        int r = concurrencia_esperar_conexion(ctx->fd_escucha, CONCURRENCIA_REBANADA_MS);
        if (r <= 0) {
            continue;
        }
        int fd = accept(ctx->fd_escucha, NULL, NULL);
        if (fd < 0) {
            if (errno != EINTR && errno != EAGAIN && !servidor_detencion_solicitada()) {
                perror("[servidor] accept");
                concurrencia_pausa_ms(100);
            }
            continue;
        }
        atender_cliente(fd, ctx->catalog, ctx->canal);
        printf("[servidor] FIFO: sesion concluida, se atiende la siguiente\n");
    }
    return 0;
}