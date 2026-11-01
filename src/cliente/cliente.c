/* cliente.c - Punto de entrada del cliente de video */
#include "cliente_args.h"
#include "cliente_ops.h"
#include "cliente_player.h"
#include "cliente_red.h"
#include "protocol.h"

#include <stdio.h>
#include <unistd.h>

/* Tras una descarga completa se abre el archivo en el reproductor (RF-24-bis). */
static void reproducir_archivo(const cliente_config_t *cfg)
{
    char **argv = player_construir_argv(cfg->reproductor, cfg->ruta_final);
    player_t pl;

    if (argv == NULL) {
        fprintf(stderr, "Aviso: no se encontro un reproductor (mpv, ffplay, vlc); "
                        "el archivo quedo en %s\n", cfg->ruta_final);
        return;
    }
    printf("Abriendo reproductor: %s\n", argv[0]);
    if (player_lanzar(argv, 0, &pl) == 0) {
        player_esperar(&pl);
    }
    player_liberar_argv(argv);
}

int main(int argc, char *argv[])
{
    cliente_config_t cfg;
    char **argv_repro = NULL;
    uint8_t servicios = 0;
    int fd, rc;

    switch (args_analizar(argc, argv, &cfg)) {
    case ARGS_AYUDA: return EXITO;
    case ARGS_ERROR: return ERR_ARGS;
    case ARGS_OK:    break;
    }

    red_instalar_senales();

    /* El canal sin reproductor no tiene sentido: se verifica antes de conectar. */
    if (cfg.accion == ACCION_CANAL && !cfg.sin_reproductor) {
        argv_repro = player_construir_argv(cfg.reproductor, NULL);
        if (argv_repro == NULL) {
            fprintf(stderr, "Error: no se encontro un reproductor (mpv, ffplay o vlc). "
                            "Instale uno, indique otro con -r, o use -n para no reproducir.\n");

            return ERR_ARGS;
        }
    }
    if (cfg.verboso) {
        printf("Servidor: %s:%s | accion: %d | timeout: %d s\n",
               cfg.direccion, cfg.puerto, (int)cfg.accion, cfg.timeout_s);
    }

    fd = red_conectar(cfg.direccion, cfg.puerto, cfg.timeout_s, cfg.verboso);
    if (fd < 0) {
        player_liberar_argv(argv_repro);
        return red_interrumpido() ? EXIT_INTERRUMPIDO : ERR_RED;
    }

    rc = red_saludar(fd, cfg.timeout_s, &servicios);
    if (rc != EXITO) {
        close(fd);
        player_liberar_argv(argv_repro);
        return rc;
    }
    printf("Conectado a %s:%s (servicios:%s%s)\n", cfg.direccion, cfg.puerto,
           (servicios & 0x01) ? " VOD" : "", (servicios & 0x02) ? " CANAL" : "");

    switch (cfg.accion) {
    case ACCION_LISTAR:
        rc = op_listar(fd, &cfg);
        break;
    case ACCION_DESCARGAR:
        rc = (servicios & 0x01) ? op_descargar(fd, &cfg) : ERR_CONTENIDO;
        if (!(servicios & 0x01)) fprintf(stderr, "Error: el servidor no ofrece video bajo demanda.\n");
        break;
    case ACCION_CANAL:
        rc = (servicios & 0x02) ? op_canal(fd, &cfg, argv_repro) : ERR_CONTENIDO;
        if (!(servicios & 0x02)) fprintf(stderr, "Error: el servidor no ofrece canal en vivo.\n");
        break;
    }

    red_cerrar_sesion(fd, cfg.verboso);       /* siempre se libera la conexion */

    if (rc == EXITO && cfg.accion == ACCION_DESCARGAR && !cfg.sin_reproductor) {
        reproducir_archivo(&cfg);
    }
    player_liberar_argv(argv_repro);
    return rc;
}