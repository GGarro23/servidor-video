#define _POSIX_C_SOURCE 200809L

#include "concurrencia.h"
#include "servidor.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv_vacio = PTHREAD_COND_INITIALIZER;
static int *fds_activos;
static size_t capacidad;
static size_t activos;
static unsigned long admitidas, rechazadas, concluidas;

typedef struct {
    int fd;
    size_t ranura;
    const contexto_servidor_t *ctx;
} arg_hilo_t;

static void *hilo_sesion(void *p) {
    arg_hilo_t a = *(arg_hilo_t *)p;
    free(p);

    /* Las senales de terminacion las atiende el hilo principal. */
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGINT);
    sigaddset(&s, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &s, NULL);

    atender_cliente(a.fd, a.ctx->catalog, a.ctx->canal);   /* cierra fd */

    pthread_mutex_lock(&mtx);
    fds_activos[a.ranura] = -1;
    activos--;
    concluidas++;
    if (activos == 0) pthread_cond_broadcast(&cv_vacio);
    pthread_mutex_unlock(&mtx);
    return NULL;
}

static void esperar_vacio_ms(long ms) {
    struct timespec lim;
    clock_gettime(CLOCK_REALTIME, &lim);
    lim.tv_sec += ms / 1000;
    lim.tv_nsec += (ms % 1000) * 1000000L;
    if (lim.tv_nsec >= 1000000000L) { lim.tv_sec++; lim.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&mtx);
    while (activos > 0) {
        if (pthread_cond_timedwait(&cv_vacio, &mtx, &lim) != 0) break;
    }
    pthread_mutex_unlock(&mtx);
}

int modo_threaded_ejecutar(const contexto_servidor_t *ctx) {
    capacidad = ctx->max_clientes;
    fds_activos = malloc(capacidad * sizeof(int));
    if (fds_activos == NULL) {
        fprintf(stderr, "[servidor] sin memoria para la tabla de hilos\n");
        return 0;
    }
    for (size_t i = 0; i < capacidad; i++) fds_activos[i] = -1;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);   /* RFx-56.3 */

    while (!servidor_detencion_solicitada()) {
        if (concurrencia_esperar_conexion(ctx->fd_escucha, CONCURRENCIA_REBANADA_MS) <= 0) {
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

        pthread_mutex_lock(&mtx);
        size_t ranura = capacidad;
        if (activos < capacidad) {
            for (size_t i = 0; i < capacidad; i++) {
                if (fds_activos[i] < 0) { ranura = i; break; }
            }
        }
        if (ranura < capacidad) {
            fds_activos[ranura] = fd;   /* reservada antes de crear el hilo */
            activos++;
        } else {
            rechazadas++;
        }
        pthread_mutex_unlock(&mtx);

        if (ranura == capacidad) {
            fprintf(stderr, "[servidor] limite de %zu clientes alcanzado, conexion rechazada\n", capacidad);
            concurrencia_rechazar(fd, PROTOCOL_STATUS_CAPACIDAD, "servidor lleno, intente mas tarde");
            continue;
        }

        arg_hilo_t *a = malloc(sizeof(*a));
        pthread_t th;
        int rc = (a == NULL) ? ENOMEM : 0;
        if (a != NULL) {
            a->fd = fd; a->ranura = ranura; a->ctx = ctx;
            rc = pthread_create(&th, &attr, hilo_sesion, a);
            if (rc != 0) free(a);
        }
        if (rc != 0) {
            fprintf(stderr, "[servidor] no se pudo crear el hilo: %d\n", rc);
            pthread_mutex_lock(&mtx);
            fds_activos[ranura] = -1;
            activos--;
            rechazadas++;
            pthread_mutex_unlock(&mtx);
            concurrencia_rechazar(fd, PROTOCOL_STATUS_INTERNO, "no se pudo atender la conexion");
            continue;
        }
        pthread_mutex_lock(&mtx);
        admitidas++;
        size_t n = activos;
        pthread_mutex_unlock(&mtx);
        printf("[servidor] THREADED: nuevo hilo (%zu/%zu activos)\n", n, capacidad);
    }
    pthread_attr_destroy(&attr);

    esperar_vacio_ms(CONCURRENCIA_GRACIA_MS);
    pthread_mutex_lock(&mtx);
    for (size_t i = 0; i < capacidad; i++) {
        if (fds_activos[i] >= 0) shutdown(fds_activos[i], SHUT_RDWR);
    }
    pthread_mutex_unlock(&mtx);
    esperar_vacio_ms(CONCURRENCIA_GRACIA_MS);

    pthread_mutex_lock(&mtx);
    size_t restantes = activos;
    unsigned long a_ = admitidas, r_ = rechazadas, c_ = concluidas;
    pthread_mutex_unlock(&mtx);
    printf("[servidor] THREADED: admitidas=%lu rechazadas=%lu concluidas=%lu\n", a_, r_, c_);

    if (restantes > 0) {
        fprintf(stderr, "[servidor] %zu sesion(es) no cerraron a tiempo\n", restantes);
        return -1;   /* no liberar tabla ni destruir catalog/canal */
    }
    free(fds_activos);
    fds_activos = NULL;
    return 0;
}