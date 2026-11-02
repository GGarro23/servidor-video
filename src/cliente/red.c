/* red.c - Conexión TCP, manejo de señales y utilidades de sesión del cliente */
#include "cliente_red.h"
#include "cliente_args.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_interrumpido = 0;
static uint32_t g_ultimo_id = 0;

static void manejador_senal(int sig)
{
    (void)sig;
    g_interrumpido = 1;     /* solo se marca; el trabajo se hace en un punto seguro */
}

void red_instalar_senales(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = manejador_senal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                 /* sin SA_RESTART: poll() devuelve EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);        /* escribir a un reproductor cerrado -> EPIPE */
}

int  red_interrumpido(void)         { return g_interrumpido; }
void red_limpiar_interrupcion(void) { g_interrumpido = 0; }

uint32_t red_nuevo_id(void)
{
    g_ultimo_id++;
    if (g_ultimo_id == 0) {
        g_ultimo_id = 1;
    }
    return g_ultimo_id;
}

static long long ahora_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int red_conectar(const char *host, const char *puerto, int timeout_s, int verboso)
{
    struct addrinfo pistas, *lista = NULL, *ai;
    int fd = -1;
    int rc;

    memset(&pistas, 0, sizeof pistas);
    pistas.ai_family = AF_UNSPEC;
    pistas.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(host, puerto, &pistas, &lista);
    if (rc != 0) {
        fprintf(stderr, "Error: no se pudo resolver '%s': %s\n", host, gai_strerror(rc));
        return -1;
    }

    for (ai = lista; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }

        /* connect() con plazo: se pasa a no bloqueante y se espera con poll(). */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            long long limite = ahora_ms() + (timeout_s > 0 ? timeout_s * 1000LL : -1);
            for (;;) {
                int espera = -1;
                if (timeout_s > 0) {
                    long long resta = limite - ahora_ms();
                    if (resta <= 0) { errno = ETIMEDOUT; rc = -1; break; }
                    espera = (int)(resta > 200 ? 200 : resta);
                } else {
                    espera = 200;
                }
                int n = poll(&p, 1, espera);
                if (red_interrumpido()) { errno = EINTR; rc = -1; break; }
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) { rc = -1; break; }
                if (n == 0) continue;
                int err = 0;
                socklen_t len = sizeof err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err != 0) { errno = err; rc = -1; } else { rc = 0; }
                break;
            }
        }

        if (rc == 0) {
            fcntl(fd, F_SETFL, flags);          /* volver a modo bloqueante */
            if (timeout_s > 0) {                /* respaldo ante servidor mudo a mitad de mensaje */
                struct timeval tv = { .tv_sec = timeout_s, .tv_usec = 0 };
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            }
            {   /* Detecta un servidor que desaparece sin cerrar la conexion (cable,
                 * caida de la maquina): el sistema envia sondas y, si nadie responde
                 * (~35 s), recv() falla. Es lo que permite no aplicar un timeout de
                 * aplicacion mientras el canal esta en silencio (separacion). */
                int activar = 1, inactivo_s = 20, intervalo_s = 5, sondas = 3;
                setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &activar, sizeof activar);
                setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &inactivo_s, sizeof inactivo_s);
                setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intervalo_s, sizeof intervalo_s);
                setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &sondas, sizeof sondas);
            }
            if (verboso) {
                fprintf(stdout, "Conexion TCP establecida con %s:%s\n", host, puerto);
            }
            break;
        }

        if (verboso) {
            fprintf(stderr, "  intento fallido: %s\n", strerror(errno));
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(lista);
    if (fd < 0) {
        fprintf(stderr, "Error: no se pudo conectar a %s:%s (%s).\n", host, puerto,
                red_interrumpido() ? "interrumpido" : strerror(errno));
    }
    return fd;
}

protocol_result_t red_enviar(int fd, uint8_t tipo, uint32_t id,
                             const uint8_t *payload, uint32_t largo)
{
    protocol_header_t h = {
        .version = PROTOCOL_VERSION,
        .type = tipo,
        .status = PROTOCOL_STATUS_EXITO,
        .request_id = id,
        .payload_length = largo
    };
    return protocol_send_message(fd, &h, payload);
}

recv_resultado_t red_recibir(int fd, protocol_message_t *msg,
                             int timeout_s, int respetar_ctrl_c)
{
    long long limite = timeout_s > 0 ? ahora_ms() + timeout_s * 1000LL : 0;
    struct pollfd p = { .fd = fd, .events = POLLIN };

    for (;;) {
        if (respetar_ctrl_c && red_interrumpido()) {
            return RECV_INTERRUMPIDO;
        }
        if (timeout_s > 0 && ahora_ms() >= limite) {
            return RECV_TIMEOUT;
        }
        int n = poll(&p, 1, 200);       /* porciones cortas para poder atender Ctrl+C */
        if (n < 0) {
            if (errno == EINTR) continue;
            return RECV_ERROR;
        }
        if (n > 0) break;
    }

    switch (protocol_recv_message(fd, msg)) {
    case PROTOCOL_OK:        return RECV_OK;
    case PROTOCOL_EOF:       return RECV_EOF;
    case PROTOCOL_TRUNCATED: return RECV_TRUNCADO;
    case PROTOCOL_INVALID:   return RECV_PROTOCOLO;
    case PROTOCOL_IO_ERROR:
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? RECV_TIMEOUT : RECV_ERROR;
    default:                 return RECV_ERROR;
    }
}

int red_esperar_datos(int fd, int ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    int n = poll(&p, 1, ms);
    if (n < 0) return errno == EINTR ? 0 : -1;
    return n > 0 ? 1 : 0;
}

const char *red_texto_recv(recv_resultado_t r)
{
    switch (r) {
    case RECV_OK:             return "ok";
    case RECV_EOF:            return "el servidor cerro la conexion";
    case RECV_TRUNCADO:       return "conexion cortada a mitad de un mensaje";
    case RECV_TIMEOUT:        return "tiempo de espera agotado (el servidor no responde)";
    case RECV_INTERRUMPIDO:   return "interrumpido por el usuario";
    case RECV_PROTOCOLO:      return "el servidor envio un mensaje invalido";
    default:                  return "error de red";
    }
}

int red_codigo_salida_recv(recv_resultado_t r)
{
    return r == RECV_INTERRUMPIDO ? EXIT_INTERRUMPIDO : ERR_RED;
}

int red_codigo_salida_error(uint16_t status)
{
    switch (status) {
    case PROTOCOL_STATUS_VERSION:      return ERR_RED;
    case PROTOCOL_STATUS_NO_HALLADO:
    case PROTOCOL_STATUS_NO_SERVIBLE:
    case PROTOCOL_STATUS_NO_PERMITIDO: return ERR_CONTENIDO;
    case PROTOCOL_STATUS_CAPACIDAD:    return ERR_CAPACIDAD;
    default:                           return ERR_INTERNO;
    }
}

int red_reportar_error(const protocol_message_t *msg)
{
    static const char *nombres[] = {
        "EXITO", "VERSION", "SOLICITUD", "NO_HALLADO",
        "NO_SERVIBLE", "NO_PERMITIDO", "CAPACIDAD", "INTERNO"
    };
    uint16_t st = msg->header.status;
    fprintf(stderr, "El servidor rechazo la solicitud [%s]: %.*s\n",
            st <= PROTOCOL_STATUS_INTERNO ? nombres[st] : "?",
            (int)msg->header.payload_length,
            msg->payload ? (const char *)msg->payload : "");
    return red_codigo_salida_error(st);
}

int red_saludar(int fd, int timeout_s, uint8_t *servicios)
{
    protocol_message_t m = {0};
    uint32_t id = red_nuevo_id();
    int rc;

    if (red_enviar(fd, PROTOCOL_MSG_SALUDO, id, NULL, 0) != PROTOCOL_OK) {
        fprintf(stderr, "Error: no se pudo enviar SALUDO.\n");
        return ERR_RED;
    }
    recv_resultado_t r = red_recibir(fd, &m, timeout_s, 1);
    if (r != RECV_OK) {
        fprintf(stderr, "Error durante el saludo: %s.\n", red_texto_recv(r));
        return red_codigo_salida_recv(r);
    }
    if (m.header.type == PROTOCOL_MSG_ERROR) {
        rc = red_reportar_error(&m);
    } else if (m.header.type == PROTOCOL_MSG_BIENVENIDA && m.header.request_id == id) {
        *servicios = m.payload[0];
        rc = EXITO;
    } else {
        fprintf(stderr, "Error: se esperaba BIENVENIDA y llego un mensaje de tipo %u.\n",
                (unsigned)m.header.type);
        rc = ERR_RED;
    }
    protocol_message_free(&m);
    return rc;
}

void red_cerrar_sesion(int fd, int verboso)
{
    protocol_message_t m = {0};
    uint32_t id = red_nuevo_id();

    if (red_enviar(fd, PROTOCOL_MSG_CERRAR, id, NULL, 0) == PROTOCOL_OK) {
        /* Se descartan mensajes en tránsito hasta DESPEDIDA (máx. 3 s). */
        long long fin = ahora_ms() + 3000;
        while (ahora_ms() < fin) {
            if (red_recibir(fd, &m, 3, 0) != RECV_OK) break;
            int fue_despedida = (m.header.type == PROTOCOL_MSG_DESPEDIDA);
            protocol_message_free(&m);
            if (fue_despedida) {
                if (verboso) printf("Sesion cerrada ordenadamente.\n");
                break;
            }
        }
    }
    close(fd);
}