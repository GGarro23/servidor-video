/* ops.c - Implementación de las tres acciones del cliente */
#include "cliente_ops.h"
#include "cliente_red.h"
#include "cliente_player.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------- utilidades ---------- */

static long long ahora_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t leer_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static uint16_t leer_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void formatear_bytes(uint64_t n, char *out, size_t cap)
{
    static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)n;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) snprintf(out, cap, "%" PRIu64 " B", n);
    else        snprintf(out, cap, "%.1f %s", v, u[i]);
}

/* Escribe todo el buffer; en EINTR respeta Ctrl+C. */
static int escribir_todo(int fd, const uint8_t *buf, size_t n)
{
    size_t hecho = 0;
    while (hecho < n) {
        ssize_t w = write(fd, buf + hecho, n - hecho);
        if (w < 0) {
            if (errno == EINTR) {
                if (red_interrumpido()) return -1;
                continue;
            }
            return -1;
        }
        hecho += (size_t)w;
    }
    return 0;
}

/* Espera el mensaje de 'tipo_final' descartando DATOS en tránsito (máx. 3 s). */
static void drenar_hasta(int fd, uint8_t tipo_final)
{
    protocol_message_t m = {0};
    long long limite = ahora_ms() + 3000;

    while (ahora_ms() < limite) {
        if (red_recibir(fd, &m, 3, 0) != RECV_OK) return;
        uint8_t t = m.header.type;
        protocol_message_free(&m);
        if (t == tipo_final || t == PROTOCOL_MSG_ERROR || t == PROTOCOL_MSG_DESPEDIDA) return;
    }
}

/* ---------- LISTAR ---------- */

int op_listar(int fd, const cliente_config_t *cfg)
{
    protocol_message_t m = {0};
    uint32_t id = red_nuevo_id();
    int rc;

    if (red_enviar(fd, PROTOCOL_MSG_LISTAR, id, NULL, 0) != PROTOCOL_OK) {
        fprintf(stderr, "Error: no se pudo enviar LISTAR.\n");
        return ERR_RED;
    }
    if (cfg->verboso) printf("Solicitud LISTAR enviada (id=%u).\n", (unsigned)id);

    recv_resultado_t r = red_recibir(fd, &m, cfg->timeout_s, 1);
    if (r != RECV_OK) {
        fprintf(stderr, "Error esperando el catalogo: %s.\n", red_texto_recv(r));
        return red_codigo_salida_recv(r);
    }
    if (m.header.type == PROTOCOL_MSG_ERROR) {
        rc = red_reportar_error(&m);
        protocol_message_free(&m);
        return rc;
    }
    if (m.header.type != PROTOCOL_MSG_CATALOGO || m.header.request_id != id) {
        fprintf(stderr, "Error: se esperaba CATALOGO y llego un mensaje de tipo %u.\n",
                (unsigned)m.header.type);
        protocol_message_free(&m);
        return ERR_RED;
    }

    /* La estructura ya fue validada por protocol_recv_message(). */
    uint16_t n = leer_u16(m.payload);
    size_t off = 2;

    printf("Contenidos disponibles bajo demanda: %u\n", (unsigned)n);
    for (uint16_t i = 0; i < n; i++) {
        uint16_t largo = leer_u16(m.payload + off);
        const uint8_t *nombre = m.payload + off + 2;
        uint64_t tam = leer_u64(m.payload + off + 2 + largo);
        char txt[32];
        formatear_bytes(tam, txt, sizeof txt);
        printf("  %-40.*s %12s\n", (int)largo, (const char *)nombre, txt);
        off += 2u + largo + 8u;
    }
    protocol_message_free(&m);
    return EXITO;
}

/* ---------- DESCARGAR ---------- */

/* Pide cancelar la descarga 'id' y espera CANCELADO (extensión del equipo). */
static void cancelar_vod(int fd, uint32_t id, int verboso)
{
    if (red_enviar(fd, PROTOCOL_MSG_CANCELAR, id, NULL, 0) == PROTOCOL_OK) {
        drenar_hasta(fd, PROTOCOL_MSG_CANCELADO);
        if (verboso) printf("Descarga cancelada en el servidor.\n");
    }
}

static void mostrar_progreso(uint64_t total, uint64_t tam, long long inicio,
                             long long *ultimo, int tty, int verboso, int final)
{
    long long ahora = ahora_ms();
    long long cada = tty ? 250 : 2000;

    if (!final && ahora - *ultimo < cada) return;
    if (!tty && !verboso && !final) return;
    *ultimo = ahora;

    double pct = tam > 0 ? 100.0 * (double)total / (double)tam : 100.0;
    double seg = (double)(ahora - inicio) / 1000.0;
    double vel = seg > 0.0 ? (double)total / seg : 0.0;
    char a[32], b[32], v[32];
    formatear_bytes(total, a, sizeof a);
    formatear_bytes(tam, b, sizeof b);
    formatear_bytes((uint64_t)vel, v, sizeof v);

    if (tty) {
        printf("\r  %5.1f%%  %s / %s  (%s/s)   ", pct, a, b, v);
        if (final) printf("\n");
        fflush(stdout);
    } else if (verboso || final) {
        printf("  %5.1f%%  %s / %s  (%s/s)\n", pct, a, b, v);
    }
}

int op_descargar(int fd, const cliente_config_t *cfg)
{
    protocol_message_t m = {0};
    uint8_t cuerpo[PROTOCOL_MAX_NAME + 2];
    size_t cuerpo_len = 0;
    uint32_t id = red_nuevo_id();
    int rc = ERR_INTERNO;
    int fd_out = -1;
    int tty = isatty(STDOUT_FILENO);
    uint64_t tam = 0, total = 0;
    uint8_t formato = 0;
    long long inicio = 0, ultimo = 0;
    int cancelar = 0;               /* 1 si hay que avisar al servidor */

    if (protocol_encode_name((const uint8_t *)cfg->archivo, (uint16_t)strlen(cfg->archivo),
                             cuerpo, sizeof cuerpo, &cuerpo_len) != PROTOCOL_OK) {
        fprintf(stderr, "Error: nombre de contenido invalido.\n");
        return ERR_ARGS;
    }
    if (red_enviar(fd, PROTOCOL_MSG_ABRIR, id, cuerpo, (uint32_t)cuerpo_len) != PROTOCOL_OK) {
        fprintf(stderr, "Error: no se pudo enviar ABRIR.\n");
        return ERR_RED;
    }
    printf("Solicitando '%s'...\n", cfg->archivo);

    /* --- respuesta inicial: ACEPTADO o ERROR --- */
    recv_resultado_t r = red_recibir(fd, &m, cfg->timeout_s, 1);
    if (r == RECV_INTERRUMPIDO) {
        red_limpiar_interrupcion();
        cancelar_vod(fd, id, cfg->verboso);
        fprintf(stderr, "Descarga interrumpida por el usuario.\n");
        return EXIT_INTERRUMPIDO;
    }
    if (r != RECV_OK) {
        fprintf(stderr, "Error esperando respuesta a ABRIR: %s.\n", red_texto_recv(r));
        return red_codigo_salida_recv(r);
    }
    if (m.header.type == PROTOCOL_MSG_ERROR) {
        rc = red_reportar_error(&m);
        protocol_message_free(&m);
        return rc;
    }
    if (m.header.type != PROTOCOL_MSG_ACEPTADO || m.header.request_id != id ||
        protocol_decode_vod_accepted(m.payload, m.header.payload_length, &tam, &formato) != PROTOCOL_OK) {
        fprintf(stderr, "Error: respuesta invalida a ABRIR (tipo %u).\n", (unsigned)m.header.type);
        protocol_message_free(&m);
        return ERR_RED;
    }
    protocol_message_free(&m);

    {
        char t[32];
        formatear_bytes(tam, t, sizeof t);
        printf("Aceptado: %s (%" PRIu64 " bytes). Guardando en %s\n", t, tam, cfg->ruta_final);
    }

    /* El archivo parcial solo se crea una vez aceptada la solicitud. */
    fd_out = open(cfg->ruta_parcial, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd_out < 0) {
        fprintf(stderr, "Error: no se pudo crear '%s': %s\n", cfg->ruta_parcial, strerror(errno));
        cancelar_vod(fd, id, cfg->verboso);
        return ERR_INTERNO;
    }
    cancelar = 1;
    inicio = ultimo = ahora_ms();

    /* --- bucle de recepción por bloques (no se carga el archivo en memoria) --- */
    for (;;) {
        r = red_recibir(fd, &m, cfg->timeout_s, 1);

        if (r == RECV_INTERRUMPIDO) {
            red_limpiar_interrupcion();
            printf("\nInterrumpido: cancelando descarga...\n");
            cancelar_vod(fd, id, cfg->verboso);
            cancelar = 0;
            fprintf(stderr, "Descarga cancelada; se descarta el archivo parcial.\n");
            rc = EXIT_INTERRUMPIDO;
            goto fin;
        }
        if (r != RECV_OK) {
            if (tty) printf("\n");
            fprintf(stderr, "Error durante la descarga: %s (se recibieron %" PRIu64 " de %" PRIu64 " bytes).\n",
                    red_texto_recv(r), total, tam);
            cancelar = 0;
            rc = red_codigo_salida_recv(r);
            goto fin;
        }

        if (m.header.type == PROTOCOL_MSG_DATOS && m.header.request_id == id) {
            if (total + m.header.payload_length > tam) {
                fprintf(stderr, "\nError de integridad: el servidor envio mas bytes de los anunciados.\n");
                protocol_message_free(&m);
                rc = ERR_RED;
                goto fin;
            }
            if (escribir_todo(fd_out, m.payload, m.header.payload_length) != 0) {
                fprintf(stderr, "\nError al escribir en disco: %s\n", strerror(errno));
                protocol_message_free(&m);
                rc = ERR_INTERNO;
                goto fin;       /* 'cancelar' sigue en 1: se avisa al servidor */
            }
            total += m.header.payload_length;
            protocol_message_free(&m);
            mostrar_progreso(total, tam, inicio, &ultimo, tty, cfg->verboso, 0);
            continue;
        }

        if (m.header.type == PROTOCOL_MSG_FIN && m.header.request_id == id) {
            protocol_message_free(&m);
            cancelar = 0;
            if (total != tam) {
                if (tty) printf("\n");
                fprintf(stderr, "Error de integridad: FIN recibido con %" PRIu64 " de %" PRIu64 " bytes.\n",
                        total, tam);
                rc = ERR_RED;
                goto fin;
            }
            break;
        }

        if (m.header.type == PROTOCOL_MSG_ERROR) {
            if (tty) printf("\n");
            cancelar = 0;
            rc = red_reportar_error(&m);
            protocol_message_free(&m);
            goto fin;
        }

        fprintf(stderr, "\nError: mensaje inesperado durante la descarga (tipo %u, id %u).\n",
                (unsigned)m.header.type, (unsigned)m.header.request_id);
        protocol_message_free(&m);
        rc = ERR_RED;
        goto fin;
    }

    mostrar_progreso(total, tam, inicio, &ultimo, tty, cfg->verboso, 1);

    if (close(fd_out) != 0) {
        fd_out = -1;
        fprintf(stderr, "Error al cerrar el archivo: %s\n", strerror(errno));
        rc = ERR_INTERNO;
        goto fin;
    }
    fd_out = -1;

    if (rename(cfg->ruta_parcial, cfg->ruta_final) != 0) {
        fprintf(stderr, "Error: no se pudo mover '%s' a '%s': %s\n",
                cfg->ruta_parcial, cfg->ruta_final, strerror(errno));
        rc = ERR_INTERNO;
        goto fin;
    }
    printf("Descarga completa y verificada: %s (%" PRIu64 " bytes)\n", cfg->ruta_final, total);
    return EXITO;

fin:
    if (cancelar) {
        cancelar_vod(fd, id, cfg->verboso);
    }
    if (fd_out >= 0) close(fd_out);
    unlink(cfg->ruta_parcial);       /* nunca queda un parcial confundible (RF-36/70) */
    return rc;
}

/* ---------- CANAL EN VIVO ---------- */

int op_canal(int fd, const cliente_config_t *cfg, char **argv_reproductor)
{
    protocol_message_t m = {0};
    uint32_t id = red_nuevo_id();
    player_t pl = { .pid = -1, .fd_entrada = -1 };
    int rc = EXITO;
    int sesion_activa = 1;          /* 0 si el servidor ya terminó la emisión */
    uint64_t total = 0;
    long long inicio, limite = 0;
    const char *motivo = "finalizado";

    if (red_enviar(fd, PROTOCOL_MSG_UNIR, id, NULL, 0) != PROTOCOL_OK) {
        fprintf(stderr, "Error: no se pudo enviar UNIR.\n");
        return ERR_RED;
    }

    recv_resultado_t r = red_recibir(fd, &m, cfg->timeout_s, 1);
    if (r != RECV_OK) {
        fprintf(stderr, "Error esperando EMISION: %s.\n", red_texto_recv(r));
        return red_codigo_salida_recv(r);
    }
    if (m.header.type == PROTOCOL_MSG_ERROR) {
        rc = red_reportar_error(&m);
        protocol_message_free(&m);
        return rc;
    }
    if (m.header.type != PROTOCOL_MSG_EMISION || m.header.request_id != id) {
        fprintf(stderr, "Error: se esperaba EMISION y llego un mensaje de tipo %u.\n",
                (unsigned)m.header.type);
        protocol_message_free(&m);
        return ERR_RED;
    }
    {
        uint64_t ciclo = leer_u64(m.payload);
        uint64_t pos = leer_u64(m.payload + 8);
        const uint8_t *nombre = NULL;
        uint16_t largo = 0;
        if (protocol_decode_name(m.payload + 16, m.header.payload_length - 16u,
                                 &nombre, &largo) != PROTOCOL_OK) {
            fprintf(stderr, "Error: EMISION con nombre invalido.\n");
            protocol_message_free(&m);
            return ERR_RED;
        }
        printf("Canal en vivo: '%.*s'\n", (int)largo, (const char *)nombre);
        printf("  Duracion del ciclo : %.1f s\n", (double)ciclo / 1000.0);
        printf("  Posicion de ingreso: %.1f s\n", (double)pos / 1000.0);
    }
    protocol_message_free(&m);

    if (argv_reproductor != NULL) {
        if (cfg->verboso) printf("Lanzando reproductor: %s\n", argv_reproductor[0]);
        if (player_lanzar(argv_reproductor, 1, &pl) != 0) {
            fprintf(stderr, "Error: no se pudo lanzar el reproductor.\n");
            rc = ERR_INTERNO;
            goto salir_canal;
        }
    } else {
        printf("(sin reproductor: los datos se reciben y se descartan)\n");
    }

    inicio = ahora_ms();
    if (cfg->canal_s > 0) limite = inicio + cfg->canal_s * 1000LL;
    printf("Recibiendo canal... (Ctrl+C para salir)\n");

    for (;;) {
        if (limite && ahora_ms() >= limite) { motivo = "tiempo de canal cumplido"; break; }
        if (argv_reproductor != NULL && !player_vivo(&pl)) { motivo = "reproductor cerrado"; break; }

        r = red_recibir(fd, &m, cfg->timeout_s, 1);
        if (r == RECV_INTERRUMPIDO) {
            red_limpiar_interrupcion();
            motivo = "interrumpido por el usuario";
            rc = EXITO;                 /* salir del canal con Ctrl+C es normal */
            break;
        }
        if (r != RECV_OK) {
            fprintf(stderr, "Conexion perdida con el servidor: %s.\n", red_texto_recv(r));
            rc = red_codigo_salida_recv(r);
            sesion_activa = 0;
            goto salir_canal;
        }

        if (m.header.type == PROTOCOL_MSG_DATOS && m.header.request_id == id) {
            total += m.header.payload_length;
            if (pl.fd_entrada >= 0 &&
                escribir_todo(pl.fd_entrada, m.payload, m.header.payload_length) != 0) {
                protocol_message_free(&m);
                if (red_interrumpido()) {
                    red_limpiar_interrupcion();
                    motivo = "interrumpido por el usuario";
                } else {
                    motivo = "reproductor cerrado";
                }
                break;
            }
            protocol_message_free(&m);
            continue;
        }
        if (m.header.type == PROTOCOL_MSG_DESPEDIDA) {
            protocol_message_free(&m);
            printf("El servidor finalizo la emision (detencion ordenada del servidor).\n");
            sesion_activa = 0;
            motivo = "servidor detenido";
            break;
        }
        if (m.header.type == PROTOCOL_MSG_ERROR) {
            rc = red_reportar_error(&m);
            protocol_message_free(&m);
            sesion_activa = 0;
            goto salir_canal;
        }
        fprintf(stderr, "Error: mensaje inesperado en el canal (tipo %u).\n", (unsigned)m.header.type);
        protocol_message_free(&m);
        rc = ERR_RED;
        goto salir_canal;
    }

    /* Salida ordenada del canal: SALIR -> ACEPTADO (vacío); la sesión queda abierta. */
    if (sesion_activa) {
        uint32_t id_salir = red_nuevo_id();
        if (red_enviar(fd, PROTOCOL_MSG_SALIR, id_salir, NULL, 0) == PROTOCOL_OK) {
            drenar_hasta(fd, PROTOCOL_MSG_ACEPTADO);
        }
    }

salir_canal:
    player_detener(&pl, 1500);
    printf("Canal terminado (%s). Bytes recibidos: %" PRIu64 "\n", motivo, total);
    return rc;
}