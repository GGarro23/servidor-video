/* player.c - Detección y lanzamiento de un reproductor de video externo */
#include "cliente_player.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Candidatos, en orden de preferencia. Cada fila termina en NULL.
 * El marcador "%F" se sustituye por la ruta del archivo. */
static const char *const CAND_STDIN[][8] = {
    { "mpv",    "--really-quiet", "--force-window=yes", "-", NULL },
    { "ffplay", "-loglevel", "error", "-autoexit", "-i", "pipe:0", NULL },
    { "cvlc",   "--play-and-exit", "-", NULL },
    { "vlc",    "-", NULL },
};
static const char *const CAND_ARCHIVO[][8] = {
    { "mpv",    "--really-quiet", "%F", NULL },
    { "ffplay", "-loglevel", "error", "-autoexit", "%F", NULL },
    { "cvlc",   "--play-and-exit", "%F", NULL },
    { "vlc",    "%F", NULL },
};
#define N_CAND 4

static int en_path(const char *prog)
{
    const char *path = getenv("PATH");
    char ruta[4096];

    if (strchr(prog, '/') != NULL) {
        return access(prog, X_OK) == 0;
    }
    if (path == NULL) {
        return 0;
    }
    char *copia = strdup(path);
    if (copia == NULL) {
        return 0;
    }
    int hallado = 0;
    char *resto = NULL;
    for (char *dir = strtok_r(copia, ":", &resto); dir != NULL; dir = strtok_r(NULL, ":", &resto)) {
        snprintf(ruta, sizeof ruta, "%s/%s", *dir ? dir : ".", prog);
        if (access(ruta, X_OK) == 0) {
            hallado = 1;
            break;
        }
    }
    free(copia);
    return hallado;
}

void player_liberar_argv(char **argv)
{
    if (argv == NULL) return;
    for (size_t i = 0; argv[i] != NULL; i++) free(argv[i]);
    free(argv);
}

char **player_construir_argv(const char *personalizado, const char *archivo)
{
    char **argv = calloc(16, sizeof *argv);
    size_t n = 0;

    if (argv == NULL) return NULL;

    if (personalizado != NULL) {
        /* Se separa por espacios; sin shell, así no hay inyección de comandos. */
        char *copia = strdup(personalizado);
        char *resto = NULL;
        if (copia == NULL) { free(argv); return NULL; }
        for (char *t = strtok_r(copia, " \t", &resto); t != NULL && n < 13;
             t = strtok_r(NULL, " \t", &resto)) {
            argv[n++] = strdup(t);
        }
        free(copia);
        if (n == 0) { player_liberar_argv(argv); return NULL; }
        if (!en_path(argv[0])) {
            fprintf(stderr, "Error: el reproductor '%s' no existe o no es ejecutable.\n", argv[0]);
            player_liberar_argv(argv);
            return NULL;
        }
        if (archivo != NULL) argv[n++] = strdup(archivo);
        return argv;
    }

    for (int i = 0; i < N_CAND; i++) {
        const char *const *fila = archivo ? CAND_ARCHIVO[i] : CAND_STDIN[i];
        if (!en_path(fila[0])) continue;
        for (size_t k = 0; fila[k] != NULL; k++) {
            argv[n++] = strdup(strcmp(fila[k], "%F") == 0 ? archivo : fila[k]);
        }
        return argv;
    }

    player_liberar_argv(argv);
    return NULL;
}

int player_lanzar(char *const argv[], int por_stdin, player_t *p)
{
    int tub[2] = { -1, -1 };

    p->pid = -1;
    p->fd_entrada = -1;

    if (por_stdin && pipe(tub) != 0) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        if (por_stdin) { close(tub[0]); close(tub[1]); }
        return -1;
    }
    if (pid == 0) {
        /* Hijo: solo hereda lo imprescindible (el socket es CLOEXEC). */
        if (por_stdin) {
            dup2(tub[0], STDIN_FILENO);
            close(tub[0]);
            close(tub[1]);
        }
        execvp(argv[0], argv);
        fprintf(stderr, "Error: no se pudo ejecutar '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    if (por_stdin) {
        close(tub[0]);
        fcntl(tub[1], F_SETFD, FD_CLOEXEC);
        p->fd_entrada = tub[1];
    }
    p->pid = pid;
    return 0;
}

int player_vivo(player_t *p)
{
    if (p->pid <= 0) return 0;
    int st;
    pid_t r = waitpid(p->pid, &st, WNOHANG);
    if (r == p->pid || (r < 0 && errno == ECHILD)) {
        p->pid = -1;
        return 0;
    }
    return 1;
}

void player_detener(player_t *p, int espera_ms)
{
    if (p->fd_entrada >= 0) {
        close(p->fd_entrada);
        p->fd_entrada = -1;
    }
    if (p->pid <= 0) return;

    struct timespec t = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    for (int ms = 0; ms < espera_ms && player_vivo(p); ms += 50) {
        nanosleep(&t, NULL);
    }
    if (player_vivo(p)) {
        kill(p->pid, SIGTERM);
        for (int i = 0; i < 20 && player_vivo(p); i++) nanosleep(&t, NULL);
        if (player_vivo(p)) {
            kill(p->pid, SIGKILL);
            waitpid(p->pid, NULL, 0);
            p->pid = -1;
        }
    }
}

void player_esperar(player_t *p)
{
    if (p->fd_entrada >= 0) {
        close(p->fd_entrada);
        p->fd_entrada = -1;
    }
    if (p->pid > 0) {
        while (waitpid(p->pid, NULL, 0) < 0 && errno == EINTR) { }
        p->pid = -1;
    }
}