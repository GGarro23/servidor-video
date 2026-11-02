#define _POSIX_C_SOURCE 200809L

#include "concurrencia.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

void concurrencia_pausa_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) { }
}

int concurrencia_esperar_conexion(int fd_escucha, int timeout_ms) {
    struct pollfd p = { fd_escucha, POLLIN, 0 };
    int n = poll(&p, 1, timeout_ms);
    if (n < 0) {
        return -1;   /* EINTR incluido: el llamador revisa la bandera de detencion */
    }
    return n;
}

void concurrencia_rechazar(int fd, protocol_status_t status, const char *texto) {
    uint32_t id = 1;   /* si el cliente no saluda a tiempo */

    /* Espera corta del SALUDO para correlacionar el ERROR con su request_id. */
    struct timeval tv = { 0, 500 * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    protocol_message_t msg;
    if (protocol_recv_message(fd, &msg) == PROTOCOL_OK) {
        if (msg.header.request_id != 0) {
            id = msg.header.request_id;
        }
        protocol_message_free(&msg);
    }

    protocol_header_t h;
    h.version        = PROTOCOL_VERSION;
    h.type           = PROTOCOL_MSG_ERROR;
    h.status         = (uint16_t)status;
    h.request_id     = id;
    h.payload_length = (uint32_t)strlen(texto);
    (void)protocol_send_message(fd, &h, (const uint8_t *)texto);

    shutdown(fd, SHUT_WR);   /* que el ERROR llegue antes del FIN de TCP */
    close(fd);
}