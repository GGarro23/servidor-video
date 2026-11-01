#define _POSIX_C_SOURCE 200809L
#include "vod_control.h"
#include "protocol.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#define VOD_CONTROL_BLOQUE 32768u
#define VOD_CONTROL_MAX_ENTRADA (PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD)

/* Una trama de salida se conserva hasta que se haya escrito por completo.
   Nunca se mezclan bytes de DATOS con los de CANCELADO o DESPEDIDA. */
typedef struct {
    uint8_t bytes[PROTOCOL_HEADER_SIZE + VOD_CONTROL_BLOQUE];
    size_t longitud;
    size_t escritos;
    uint8_t tipo;
    uint32_t bytes_datos;
    bool terminal;
} trama_salida_t;

typedef struct {
    uint8_t bytes[VOD_CONTROL_MAX_ENTRADA];
    size_t recibidos;
    size_t esperados;
    protocol_header_t cabecera;
} trama_entrada_t;

typedef struct {
    bool pendiente;
    uint8_t tipo;
    uint16_t estado;
    uint32_t id;
    const char *texto;
    bool terminal;
} respuesta_t;

static void respuesta_preparar(respuesta_t *r, uint8_t tipo,
                              uint16_t estado, uint32_t id,
                              const char *texto, bool terminal) {
    r->pendiente = true;
    r->tipo = tipo;
    r->estado = estado;
    r->id = id;
    r->texto = texto;
    r->terminal = terminal;
}

static int empacar(trama_salida_t *s, uint8_t tipo, uint16_t estado,
                   uint32_t id, const uint8_t *datos, uint32_t longitud,
                   bool terminal) {
    if (longitud > VOD_CONTROL_BLOQUE) return -1;
    protocol_header_t h = {
        .version = PROTOCOL_VERSION,
        .type = tipo,
        .status = estado,
        .request_id = id,
        .payload_length = longitud
    };
    if (protocol_validate_payload(&h, datos) != PROTOCOL_OK ||
        protocol_encode_header(&h, s->bytes) != PROTOCOL_OK) return -1;

    if (longitud) memcpy(s->bytes + PROTOCOL_HEADER_SIZE, datos, longitud);
    s->longitud = PROTOCOL_HEADER_SIZE + longitud;
    s->escritos = 0;
    s->tipo = tipo;
    s->bytes_datos = tipo == PROTOCOL_MSG_DATOS ? longitud : 0;
    s->terminal = terminal;
    return 0;
}

static int empacar_respuesta(trama_salida_t *s, respuesta_t *r) {
    uint32_t len = r->texto ? (uint32_t)strlen(r->texto) : 0;
    int ok = empacar(s, r->tipo, r->estado, r->id,
                     (const uint8_t *)r->texto, len, r->terminal);
    if (ok == 0) r->pendiente = false;
    return ok;
}

/* Devuelve 0=preparado, 1=fallo de lectura, -1=estado inválido. */
static int preparar_datos(trama_salida_t *s, vod_transfer_t *t) {
    if (t->enviados > t->tamano) return -1;
    if (t->enviados == t->tamano)
        return empacar(s, PROTOCOL_MSG_FIN, PROTOCOL_STATUS_EXITO,
                       t->request_id, NULL, 0, true);

    uint64_t restantes = t->tamano - t->enviados;
    size_t n = restantes < VOD_CONTROL_BLOQUE ? (size_t)restantes : VOD_CONTROL_BLOQUE;
    ssize_t leidos;
    do {
        leidos = read(t->fd_archivo, s->bytes + PROTOCOL_HEADER_SIZE, n);
    } while (leidos < 0 && errno == EINTR);
    if (leidos <= 0) return 1;

    /* El payload ya está directamente en el arreglo de salida. */
    protocol_header_t h = {
        .version = PROTOCOL_VERSION,
        .type = PROTOCOL_MSG_DATOS,
        .status = PROTOCOL_STATUS_EXITO,
        .request_id = t->request_id,
        .payload_length = (uint32_t)leidos
    };
    if (protocol_encode_header(&h, s->bytes) != PROTOCOL_OK) return -1;
    s->longitud = PROTOCOL_HEADER_SIZE + (size_t)leidos;
    s->escritos = 0;
    s->tipo = PROTOCOL_MSG_DATOS;
    s->bytes_datos = (uint32_t)leidos;
    s->terminal = false;
    return 0;
}

/* Lee como máximo una trama completa. El MSG_DONTWAIT se aplica a esta
   operación individual; no modifica el modo del descriptor del servidor. */
static int recibir_control(int fd, trama_entrada_t *e,
                           const vod_transfer_t *t, respuesta_t *r) {
    for (;;) {
        ssize_t n = recv(fd, e->bytes + e->recibidos,
                         e->esperados - e->recibidos, MSG_DONTWAIT);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        e->recibidos += (size_t)n;
        if (e->recibidos != e->esperados) continue;

        if (e->esperados == PROTOCOL_HEADER_SIZE) {
            if (protocol_decode_header(e->bytes, &e->cabecera) != PROTOCOL_OK)
                return -1;
            e->esperados += e->cabecera.payload_length;
            if (e->cabecera.payload_length != 0) continue;
        }

        const uint8_t *payload = e->cabecera.payload_length
            ? e->bytes + PROTOCOL_HEADER_SIZE : NULL;
        if (protocol_validate_payload(&e->cabecera, payload) != PROTOCOL_OK)
            return -1;

        uint8_t tipo = e->cabecera.type;
        uint32_t id = e->cabecera.request_id;
        e->recibidos = 0;
        e->esperados = PROTOCOL_HEADER_SIZE;

        if (tipo == PROTOCOL_MSG_CANCELAR && id == t->request_id) {
            respuesta_preparar(r, PROTOCOL_MSG_CANCELADO,
                              PROTOCOL_STATUS_EXITO, id, NULL, true);
        } else if (tipo == PROTOCOL_MSG_CERRAR) {
            respuesta_preparar(r, PROTOCOL_MSG_DESPEDIDA,
                              PROTOCOL_STATUS_EXITO, id, NULL, true);
        } else {
            respuesta_preparar(r, PROTOCOL_MSG_ERROR,
                              PROTOCOL_STATUS_NO_PERMITIDO, id,
                              "operacion no permitida durante VOD", false);
        }
        return 1;
    }
}

vod_control_result_t vod_control_servir(int fd_cliente, vod_transfer_t *t) {
    if (fd_cliente < 0 || t == NULL || t->fd_archivo < 0 ||
        t->request_id == 0 || t->fin_enviado || t->enviados > t->tamano)
        return VOD_CONTROL_FALLO_CONEXION;

    trama_salida_t s = {0};
    trama_entrada_t e = {.esperados = PROTOCOL_HEADER_SIZE};
    respuesta_t respuesta = {0};
    bool error_lectura = false;

    for (;;) {
        if (s.longitud == 0) {
            if (respuesta.pendiente) {
                if (empacar_respuesta(&s, &respuesta) != 0)
                    return VOD_CONTROL_FALLO_CONEXION;
            } else if (!t->fin_enviado) {
                int pr = preparar_datos(&s, t);
                if (pr < 0) return VOD_CONTROL_FALLO_CONEXION;
                if (pr == 1) {
                    error_lectura = true;
                    respuesta_preparar(&respuesta, PROTOCOL_MSG_ERROR,
                                      PROTOCOL_STATUS_INTERNO, t->request_id,
                                      "error al leer el contenido", true);
                    continue;
                }
            } else if (e.recibidos == 0) {
                return VOD_CONTROL_COMPLETADO;
            }
        }

        struct pollfd p = {
            .fd = fd_cliente,
            .events = 0,
            .revents = 0
        };
        if (!respuesta.pendiente && !t->fin_enviado &&
            (!s.longitud || s.tipo == PROTOCOL_MSG_DATOS ||
             s.tipo == PROTOCOL_MSG_FIN))
            p.events |= POLLIN;
        if (s.longitud != 0) p.events |= POLLOUT;
        if (t->fin_enviado && e.recibidos != 0) p.events |= POLLIN;

        int n;
        do {
            n = poll(&p, 1, e.recibidos && t->fin_enviado ? 3000 : -1);
        } while (n < 0 && errno == EINTR);
        if (n <= 0 || (p.revents & (POLLERR | POLLNVAL)))
            return VOD_CONTROL_FALLO_CONEXION;

        /* Prioridad a la entrada para reconocer cancelaciones antes del
           próximo envío, incluso cuando el socket está listo para escribir. */
        if (p.revents & POLLIN) {
            int rr = recibir_control(fd_cliente, &e, t, &respuesta);
            if (rr < 0) return VOD_CONTROL_FALLO_CONEXION;
            if (rr == 1 && respuesta.terminal && s.escritos == 0) {
                /* Los bytes de esta trama aún no salieron a TCP. */
                s.longitud = 0;
            }
        }

        if (s.longitud && (p.revents & POLLOUT)) {
            ssize_t enviados = send(fd_cliente, s.bytes + s.escritos,
                                    s.longitud - s.escritos,
                                    MSG_DONTWAIT | MSG_NOSIGNAL);
            if (enviados < 0 && errno != EINTR &&
                errno != EAGAIN && errno != EWOULDBLOCK)
                return VOD_CONTROL_FALLO_CONEXION;
            if (enviados > 0) s.escritos += (size_t)enviados;

            if (s.escritos == s.longitud) {
                uint8_t tipo = s.tipo;
                bool terminal = s.terminal;
                t->enviados += s.bytes_datos;
                s.longitud = 0;
                s.escritos = 0;
                if (tipo == PROTOCOL_MSG_FIN) {
                    t->fin_enviado = true;
                    /* Si el FIN ya comenzó, una cancelación es tardía. */
                    if (respuesta.pendiente &&
                        respuesta.tipo == PROTOCOL_MSG_CANCELADO) {
                        respuesta_preparar(&respuesta, PROTOCOL_MSG_ERROR,
                                          PROTOCOL_STATUS_NO_PERMITIDO,
                                          respuesta.id,
                                          "la transferencia ya finalizo", false);
                    }
                }
                if (terminal && tipo == PROTOCOL_MSG_CANCELADO)
                    return VOD_CONTROL_CANCELADO;
                if (terminal && tipo == PROTOCOL_MSG_DESPEDIDA)
                    return VOD_CONTROL_CERRAR;
                if (terminal && tipo == PROTOCOL_MSG_ERROR)
                    return error_lectura ? VOD_CONTROL_FALLO_LECTURA
                                         : VOD_CONTROL_COMPLETADO;
                if (t->fin_enviado && !respuesta.pendiente && e.recibidos == 0)
                    return VOD_CONTROL_COMPLETADO;
            }
        }

        if (p.revents & POLLHUP) {
            /* POLLIN ya permitió consumir datos pendientes; si no quedan
               tramas de salida, la sesión ha terminado. */
            if (!(p.revents & POLLIN)) return VOD_CONTROL_FALLO_CONEXION;
        }
    }
}
