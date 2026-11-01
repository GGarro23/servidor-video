/* cliente_player.h - Lanzamiento del reproductor de video (RF-24-bis) */
#ifndef CLIENTE_PLAYER_H
#define CLIENTE_PLAYER_H

#include <sys/types.h>

typedef struct {
    pid_t pid;        /* proceso del reproductor, -1 si no hay      */
    int   fd_entrada; /* extremo de escritura hacia su stdin, o -1  */
} player_t;

/* Construye el argv del reproductor.
 *   personalizado : comando del usuario (-r) o NULL para autodetectar
 *   archivo       : ruta a reproducir, o NULL para leer de stdin (canal)
 * Devuelve un arreglo terminado en NULL (liberar con player_liberar_argv),
 * o NULL si no hay reproductor disponible. */
char **player_construir_argv(const char *personalizado, const char *archivo);
void   player_liberar_argv(char **argv);

/* Lanza el reproductor. Con por_stdin != 0 crea una tubería hacia su stdin. */
int  player_lanzar(char *const argv[], int por_stdin, player_t *p);

/* 1 si el reproductor sigue vivo (no bloquea). */
int  player_vivo(player_t *p);

/* Cierra la tubería, espera hasta espera_ms y, si no termina, lo finaliza. */
void player_detener(player_t *p, int espera_ms);

/* Espera (sin límite) a que el usuario cierre el reproductor. */
void player_esperar(player_t *p);

#endif