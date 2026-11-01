#include "servidor.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define EXITO          0
#define ERR_ARGS       1
#define ERR_RED        2
#define ERR_CONTENIDO  3
#define ERR_CAPACIDAD  4
#define ERR_INTERNO    5

#define BACKLOG_POR_DEFECTO 16

static volatile sig_atomic_t seguir_ejecutando = 1;

static void manejar_senal(int señal) {
    (void)señal;
    seguir_ejecutando = 0;
}

static void instalar_manejo_de_senales(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manejar_senal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void imprimir_ayuda(const char *nombre_programa) {
    printf("Uso: %s [OPCIONES]\n", nombre_programa);
    printf("Opciones:\n");
    printf("  -m, --modo <MODO>        FIFO | FORKED | THREADED (requerido)\n");
    printf("  -d, --direccion <IP>     Interfaz de escucha (opcional; por omision, todas)\n");
    printf("  -p, --puerto <PUERTO>    Puerto de escucha (requerido)\n");
    printf("  -a, --almacen <RUTA>     Directorio de contenido bajo demanda\n");
    printf("  -c, --canal <RUTA>       Archivo o lista del contenido del canal en vivo\n");
    printf("  -b, --backlog <N>        Profundidad de la cola de admision (por omision %d)\n",
           BACKLOG_POR_DEFECTO);
    printf("  -h, --help               Muestra esta ayuda\n");
    printf("\nEjemplo:\n");
    printf("  %s -m FIFO -p 8080\n", nombre_programa);
}

static int parsear_modo(const char *texto, modo_concurrencia_t *modo) {
    if (strcmp(texto, "FIFO") == 0) {
        *modo = MODO_FIFO;
        return 1;
    }
    if (strcmp(texto, "FORKED") == 0) {
        *modo = MODO_FORKED;
        return 1;
    }
    if (strcmp(texto, "THREADED") == 0) {
        *modo = MODO_THREADED;
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    config_servidor_t config;
    char ruta_almacen[256] = "";
    char ruta_canal[256] = "";
    int modo_fijado = 0;
    int puerto_fijado = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);

    memset(&config, 0, sizeof(config));
    config.backlog = BACKLOG_POR_DEFECTO;

    if (argc < 2) {
        imprimir_ayuda(argv[0]);
        return ERR_ARGS;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            imprimir_ayuda(argv[0]);
            return EXITO;
        } else if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--modo") == 0) && i + 1 < argc) {
            if (!parsear_modo(argv[++i], &config.modo)) {
                fprintf(stderr, "Error: modo invalido '%s'. Debe ser FIFO, FORKED o THREADED.\n", argv[i]);
                return ERR_ARGS;
            }
            modo_fijado = 1;
        } else if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--direccion") == 0) && i + 1 < argc) {
            strncpy(config.direccion, argv[++i], sizeof(config.direccion) - 1);
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--puerto") == 0) && i + 1 < argc) {
            int puerto = atoi(argv[++i]);
            if (puerto <= 0 || puerto > 65535) {
                fprintf(stderr, "Error: puerto invalido (-p). Debe estar entre 1 y 65535.\n");
                return ERR_ARGS;
            }
            config.puerto = (uint16_t)puerto;
            puerto_fijado = 1;
        } else if ((strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--almacen") == 0) && i + 1 < argc) {
            strncpy(ruta_almacen, argv[++i], sizeof(ruta_almacen) - 1);
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--canal") == 0) && i + 1 < argc) {
            strncpy(ruta_canal, argv[++i], sizeof(ruta_canal) - 1);
        } else if ((strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--backlog") == 0) && i + 1 < argc) {
            int backlog = atoi(argv[++i]);
            if (backlog <= 0) {
                fprintf(stderr, "Error: backlog invalido (-b). Debe ser positivo.\n");
                return ERR_ARGS;
            }
            config.backlog = backlog;
        } else {
            fprintf(stderr, "Error: argumento desconocido o incompleto: %s\n", argv[i]);
            return ERR_ARGS;
        }
    }

    if (!modo_fijado) {
        fprintf(stderr, "Error: falta el modo de concurrencia (-m).\n");
        return ERR_ARGS;
    }
    if (!puerto_fijado) {
        fprintf(stderr, "Error: falta el puerto de escucha (-p).\n");
        return ERR_ARGS;
    }
    const char *nombre_modo = (config.modo == MODO_FIFO) ? "FIFO"
                             : (config.modo == MODO_FORKED) ? "FORKED" : "THREADED";
    printf("[servidor] modo=%s direccion=%s puerto=%u backlog=%d\n",
           nombre_modo, config.direccion[0] ? config.direccion : "(todas)",
           (unsigned)config.puerto, config.backlog);

    if (config.modo != MODO_FIFO) {
        printf("[servidor] aviso: el modo %s todavia no esta implementado "
               "(tarea de concurrencia pendiente); sirviendo en modo FIFO mientras tanto.\n",
               nombre_modo);
    }

   if (ruta_almacen[0] == '\0') {
    fprintf(stderr, "Error: falta el directorio de videos (-a).\n");
    return ERR_ARGS;
    }

    catalog_t *catalog = NULL;

    catalog_result_t resultado =
    catalog_create(ruta_almacen, &catalog);

    if (resultado != CATALOG_OK) {
        fprintf(stderr, "[servidor] Error al inicializar el catalogo\n");
        return ERR_CONTENIDO;
    }

    printf("[servidor] almacen: %s\n", ruta_almacen);
    printf("[servidor] videos encontrados: %zu\n",
       catalog_count(catalog));

    if (ruta_canal[0]) {
        printf("[servidor] contenido del canal: %s (canal aun no implementado)\n", ruta_canal);
    }

   int fd_escucha = red_crear_escucha(&config);

    if (fd_escucha < 0) {
        catalog_destroy(catalog);
        return ERR_RED;
    }
    instalar_manejo_de_senales();
    printf("[servidor] escuchando; Ctrl+C para detener de forma ordenada\n");

    while (seguir_ejecutando) {
        struct sockaddr_storage direccion_cliente;
        socklen_t tam = sizeof(direccion_cliente);

        int fd_cliente = accept(fd_escucha, (struct sockaddr *)&direccion_cliente, &tam);
        if (fd_cliente < 0) {
            if (!seguir_ejecutando) {
                break;
            }
            perror("[servidor] accept");
            continue;
        }

        atender_cliente(fd_cliente,catalog);
    }

    printf("[servidor] senal de detencion recibida, cerrando de forma ordenada\n");
    close(fd_escucha);
    catalog_destroy(catalog);
    printf("[servidor] detenido\n");

    return EXITO;
}
