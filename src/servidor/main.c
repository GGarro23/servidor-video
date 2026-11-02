#include "servidor.h"
#include "canal.h"
#include "concurrencia.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
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
#define CANAL_MAX_ELEMENTOS_CLI 16
#define MAX_CLIENTES_POR_DEFECTO 64
#define MAX_CLIENTES_TOPE 1024

//Bandera de detencion: la escribe el manejador de senal y la leen todos los hilos de sesion.
static _Atomic sig_atomic_t seguir_ejecutando = 1;

static void manejar_senal(int señal) {
    (void)señal;
    int guardado = errno;
    seguir_ejecutando = 0;
    errno = guardado;
}

int servidor_detencion_solicitada(void) {
    return !seguir_ejecutando;
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
    printf("  -s, --separacion <SEG>   Pausa entre ciclos del canal, en segundos (por omision 0)\n");
    printf("  -n, --max-clientes <N>   Maximo de clientes activos en FORKED/THREADED (por omision %d)\n",
           MAX_CLIENTES_POR_DEFECTO);
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
    uint64_t separacion_ms = 0;
    long max_clientes = MAX_CLIENTES_POR_DEFECTO;
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
        } else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--separacion") == 0) && i + 1 < argc) {
            char *fin = NULL;
            long seg = strtol(argv[++i], &fin, 10);
            if (*fin != '\0' || seg < 0 || seg > 86400) {
                fprintf(stderr, "Error: separacion invalida (-s). Debe estar entre 0 y 86400 segundos.\n");
                return ERR_ARGS;
            }
            separacion_ms = (uint64_t)seg * 1000u;
        } else if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--max-clientes") == 0) && i + 1 < argc) {
            char *fin = NULL;
            max_clientes = strtol(argv[++i], &fin, 10);
            if (*fin != '\0' || max_clientes < 1 || max_clientes > MAX_CLIENTES_TOPE) {
                fprintf(stderr, "Error: maximo de clientes invalido (-n). Debe estar entre 1 y %d.\n",
                        MAX_CLIENTES_TOPE);
                return ERR_ARGS;
            }
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
    char maxtxt[16];
    const char *nombre_modo = (config.modo == MODO_FIFO) ? "FIFO"
                             : (config.modo == MODO_FORKED) ? "FORKED" : "THREADED";
    printf("[servidor] modo=%s direccion=%s puerto=%u backlog=%d max_clientes=%s\n",
           nombre_modo, config.direccion[0] ? config.direccion : "(todas)",
           (unsigned)config.puerto, config.backlog,
           config.modo == MODO_FIFO ? "1 (FIFO)" : (snprintf(maxtxt, sizeof maxtxt, "%ld", max_clientes), maxtxt));

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

    //Canal en vivo. Se crea una vez, antes de aceptar clientes (y antes de cualquier fork o hilo), y despues es solo de lectura.
    canal_t *canal = NULL;
    if (ruta_canal[0]) {
        canal_elemento_cfg_t elementos[CANAL_MAX_ELEMENTOS_CLI];
        size_t cantidad = 0;
        char *resto = NULL;

        for (char *ruta = strtok_r(ruta_canal, ",", &resto);
             ruta != NULL && cantidad < CANAL_MAX_ELEMENTOS_CLI;
             ruta = strtok_r(NULL, ",", &resto)) {
            elementos[cantidad].ruta = ruta;
            elementos[cantidad].duracion_ms = 0;    //0 = sondear la duracion del archivo.
            cantidad++;
        }

        canal_config_t config_canal = { elementos, cantidad, separacion_ms };
        canal_result_t rc = cantidad > 0 ? canal_create(&config_canal, &canal) : CANAL_INVALID;
        if (rc != CANAL_OK) {
            fprintf(stderr, "[servidor] aviso: no se pudo crear el canal en vivo (%s); "
                            "se ofrece solo el servicio bajo demanda\n", canal_result_str(rc));
            canal = NULL;
        } else {
            printf("[servidor] canal en vivo: %zu elemento(s), ciclo de %.1f s\n",
                   canal_cantidad(canal), (double)canal_ciclo_ms(canal) / 1000.0);
            for (size_t k = 0; k < canal_cantidad(canal); k++) {
                printf("[servidor]   %zu: %s (%.1f s)\n", k, canal_nombre(canal, k),
                       (double)canal_duracion_ms(canal, k) / 1000.0);
            }
        }
    }

   int fd_escucha = red_crear_escucha(&config);

    if (fd_escucha < 0) {
        canal_destroy(canal);
        catalog_destroy(catalog);
        return ERR_RED;
    }
    instalar_manejo_de_senales();
    printf("[servidor] escuchando; Ctrl+C para detener de forma ordenada\n");

    contexto_servidor_t ctx = { fd_escucha, catalog, canal, (size_t)max_clientes };
    int limpio;
    switch (config.modo) {
        case MODO_FORKED:   limpio = modo_forked_ejecutar(&ctx);   break;
        case MODO_THREADED: limpio = modo_threaded_ejecutar(&ctx); break;
        default:            limpio = modo_fifo_ejecutar(&ctx);     break;
    }

    printf("[servidor] senal de detencion recibida, cerrando de forma ordenada\n");
    close(fd_escucha);
    if (limpio == 0) {
        canal_destroy(canal);
        catalog_destroy(catalog);
    }
    printf("[servidor] detenido\n");

    return EXITO;
}