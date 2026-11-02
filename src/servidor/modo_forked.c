#define _POSIX_C_SOURCE 200809L

#include "concurrencia.h"
#include "servidor.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t *pids;
static size_t capacidad;
static size_t activos;
static unsigned long admitidas, rechazadas, concluidas;

static void registrar(pid_t pid) {
    for (size_t i = 0; i < capacidad; i++) {
        if (pids[i] == 0) { pids[i] = pid; activos++; return; }
    }
}

/* Recolecta hijos terminados sin bloquear (RFx-52.3). */
static void recolectar(void) {
    for (;;) {
        int st;
        pid_t pid = waitpid(-1, &st, WNOHANG);
        if (pid <= 0) {
            return;
        }
        for (size_t i = 0; i < capacidad; i++) {
            if (pids[i] == pid) { pids[i] = 0; activos--; break; }
        }
        concluidas++;
        if (WIFSIGNALED(st)) {
            fprintf(stderr, "[servidor] hijo %d termino por senal %d\n", (int)pid, WTERMSIG(st));
        }
    }
}

static long ahora_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void detener_hijos(void) {
    for (size_t i = 0; i < capacidad; i++) {
        if (pids[i] > 0) kill(pids[i], SIGTERM);   /* RFx-52.4 */
    }
    long limite = ahora_ms() + CONCURRENCIA_GRACIA_MS;
    while (activos > 0 && ahora_ms() < limite) {
        recolectar();
        concurrencia_pausa_ms(50);
    }
    if (activos > 0) {
        for (size_t i = 0; i < capacidad; i++) {
            if (pids[i] > 0) kill(pids[i], SIGKILL);
        }
        concurrencia_pausa_ms(100);
        recolectar();
    }
}

int modo_forked_ejecutar(const contexto_servidor_t *ctx) {
    capacidad = ctx->max_clientes;
    pids = calloc(capacidad, sizeof(pid_t));
    if (pids == NULL) {
        fprintf(stderr, "[servidor] sin memoria para la tabla de hijos\n");
        return 0;
    }

    while (!servidor_detencion_solicitada()) {
        recolectar();   /* antes de comprobar el limite */

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

        recolectar();   /* un hijo pudo terminar mientras esperabamos: contador al dia */
        if (activos >= capacidad) {
            rechazadas++;
            fprintf(stderr, "[servidor] limite de %zu clientes alcanzado, conexion rechazada\n", capacidad);
            concurrencia_rechazar(fd, PROTOCOL_STATUS_CAPACIDAD, "servidor lleno, intente mas tarde");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("[servidor] fork");
            rechazadas++;
            concurrencia_rechazar(fd, PROTOCOL_STATUS_INTERNO, "no se pudo atender la conexion");
            continue;
        }
        if (pid == 0) {
            /* HIJO: solo conserva su cliente (RFx-18.3 / RFx-52.1). */
            close(ctx->fd_escucha);
            atender_cliente(fd, ctx->catalog, ctx->canal);   /* cierra fd */
            fflush(NULL);
            _exit(0);
        }
        /* PADRE: el descriptor del cliente pertenece al hijo. */
        close(fd);
        admitidas++;
        registrar(pid);
        printf("[servidor] FORKED: hijo %d atiende la conexion (%zu/%zu activos)\n",
               (int)pid, activos, capacidad);
    }

    detener_hijos();
    printf("[servidor] FORKED: admitidas=%lu rechazadas=%lu concluidas=%lu\n",
           admitidas, rechazadas, concluidas);
    int limpio = (activos == 0) ? 0 : -1;
    free(pids);
    pids = NULL;
    return limpio;
}