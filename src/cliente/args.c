/* args.c - Análisis y validación de la línea de comandos del cliente */
#include "cliente_args.h"
#include "protocol.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void args_ayuda(const char *programa, int a_stderr)
{
    FILE *f = a_stderr ? stderr : stdout;

    fprintf(f, "Uso: %s -d <dir> -p <puerto> -a <accion> [opciones]\n\n", programa);
    fprintf(f, "Parametros obligatorios:\n");
    fprintf(f, "  -d, --direccion <host>     Direccion IP o nombre del servidor\n");
    fprintf(f, "  -p, --puerto <1-65535>     Puerto del servidor\n");
    fprintf(f, "  -a, --accion <accion>      canal | listar | descargar\n\n");
    fprintf(f, "Solo para la accion 'descargar' (obligatorios):\n");
    fprintf(f, "  -f, --archivo <nombre>     Nombre del contenido tal como aparece en el catalogo\n");
    fprintf(f, "  -o, --destino <ruta>       Archivo o directorio donde guardar lo recibido\n\n");
    fprintf(f, "Opcionales:\n");
    fprintf(f, "  -t, --timeout <seg>        Espera maxima de conexion/respuesta (defecto: 10, 0 = sin limite).\n"
                  "                             En 'canal' no limita los silencios entre ciclos\n");
    fprintf(f, "  -c, --tiempo-canal <seg>   Solo 'canal': duracion maxima de la conexion (defecto: indefinida)\n");
    fprintf(f, "  -r, --reproductor <cmd>    Comando del reproductor (defecto: mpv, ffplay o vlc, el primero que exista)\n");
    fprintf(f, "  -n, --sin-reproductor      No abrir reproductor (los datos se reciben y se descartan / guardan)\n");
    fprintf(f, "  -v, --verboso              Mas detalle en la salida informativa\n");
    fprintf(f, "  -h, --help                 Muestra esta ayuda\n\n");
    fprintf(f, "Codigos de terminacion:\n");
    fprintf(f, "  0 exito | 1 argumentos | 2 red/conexion | 3 contenido rechazado\n");
    fprintf(f, "  4 servidor sin capacidad | 5 error interno | 130 interrumpido (Ctrl+C)\n\n");
    fprintf(f, "Ejemplos:\n");
    fprintf(f, "  %s -d 127.0.0.1 -p 8080 -a listar\n", programa);
    fprintf(f, "  %s -d 127.0.0.1 -p 8080 -a descargar -f video.mp4 -o /tmp/\n", programa);
    fprintf(f, "  %s -d 192.168.1.10 -p 8080 -a canal -c 600\n", programa);
}

/* Entero estricto: todo el texto debe ser un número dentro de [min,max]. */
static int parse_int(const char *texto, long min, long max, long *out)
{
    char *fin = NULL;

    if (texto == NULL || *texto == '\0') {
        return -1;
    }
    errno = 0;
    long v = strtol(texto, &fin, 10);
    if (errno != 0 || *fin != '\0' || v < min || v > max) {
        return -1;
    }
    *out = v;
    return 0;
}

/* Comprueba que 'ruta' (directorio) existe y se puede escribir. */
static int dir_escribible(const char *ruta)
{
    struct stat st;
    return stat(ruta, &st) == 0 && S_ISDIR(st.st_mode) &&
           access(ruta, W_OK | X_OK) == 0;
}

/* Calcula ruta_final / ruta_parcial y verifica que se pueda escribir (RF-67). */
static int resolver_destino(cliente_config_t *cfg)
{
    struct stat st;
    int n;

    if (stat(cfg->destino, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* Es un directorio: el archivo conserva su nombre del catálogo. */
        if (!dir_escribible(cfg->destino)) {
            fprintf(stderr, "Error: el directorio de destino '%s' no es escribible (-o).\n",
                    cfg->destino);
            return -1;
        }
        size_t len = strlen(cfg->destino);
        const char *sep = (len > 0 && cfg->destino[len - 1] == '/') ? "" : "/";
        n = snprintf(cfg->ruta_final, sizeof cfg->ruta_final, "%s%s%s",
                     cfg->destino, sep, cfg->archivo);
    } else {
        /* Es (o será) un archivo: su directorio padre debe existir. */
        if (cfg->destino[strlen(cfg->destino) - 1] == '/') {
            fprintf(stderr, "Error: el directorio de destino '%s' no existe (-o).\n",
                    cfg->destino);
            return -1;
        }
        char padre[PATH_MAX];
        if (snprintf(padre, sizeof padre, "%s", cfg->destino) >= (int)sizeof padre) {
            fprintf(stderr, "Error: ruta de destino demasiado larga (-o).\n");
            return -1;
        }
        char *barra = strrchr(padre, '/');
        if (barra == NULL) {
            strcpy(padre, ".");
        } else if (barra == padre) {
            padre[1] = '\0';          /* "/archivo" -> padre "/" */
        } else {
            *barra = '\0';
        }
        if (!dir_escribible(padre)) {
            fprintf(stderr, "Error: no se puede escribir en '%s' (directorio de -o).\n", padre);
            return -1;
        }
        if (stat(cfg->destino, &st) == 0 && access(cfg->destino, W_OK) != 0) {
            fprintf(stderr, "Error: el archivo de destino '%s' no es escribible (-o).\n",
                    cfg->destino);
            return -1;
        }
        n = snprintf(cfg->ruta_final, sizeof cfg->ruta_final, "%s", cfg->destino);
    }

    if (n < 0 || (size_t)n >= sizeof cfg->ruta_final) {
        fprintf(stderr, "Error: ruta de destino demasiado larga (-o).\n");
        return -1;
    }
    if (snprintf(cfg->ruta_parcial, sizeof cfg->ruta_parcial, "%s.part",
                 cfg->ruta_final) >= (int)sizeof cfg->ruta_parcial) {
        fprintf(stderr, "Error: ruta de destino demasiado larga (-o).\n");
        return -1;
    }
    return 0;
}

args_resultado_t args_analizar(int argc, char **argv, cliente_config_t *cfg)
{
    static const struct option largas[] = {
        {"direccion",      required_argument, NULL, 'd'},
        {"puerto",         required_argument, NULL, 'p'},
        {"accion",         required_argument, NULL, 'a'},
        {"archivo",        required_argument, NULL, 'f'},
        {"destino",        required_argument, NULL, 'o'},
        {"timeout",        required_argument, NULL, 't'},
        {"tiempo-canal",   required_argument, NULL, 'c'},
        {"reproductor",    required_argument, NULL, 'r'},
        {"sin-reproductor",no_argument,       NULL, 'n'},
        {"verboso",        no_argument,       NULL, 'v'},
        {"help",           no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    const char *accion_txt = NULL;
    const char *puerto_txt = NULL;
    int tiene_t = 0, tiene_c = 0;
    long v;
    int opt;

    memset(cfg, 0, sizeof *cfg);
    cfg->timeout_s = 10;

    if (argc < 2) {
        fprintf(stderr, "Error: faltan argumentos.\n\n");
        args_ayuda(argv[0], 1);
        return ARGS_ERROR;
    }

    /* La ayuda tiene prioridad sobre cualquier otro error. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            args_ayuda(argv[0], 0);
            return ARGS_AYUDA;
        }
    }

    opterr = 0;
    optind = 1;
    while ((opt = getopt_long(argc, argv, ":d:p:a:f:o:t:c:r:nvh", largas, NULL)) != -1) {
        switch (opt) {
        case 'd': cfg->direccion = optarg; break;
        case 'p': puerto_txt = optarg; break;
        case 'a': accion_txt = optarg; break;
        case 'f': cfg->archivo = optarg; break;
        case 'o': cfg->destino = optarg; break;
        case 't':
            if (parse_int(optarg, 0, 3600, &v) != 0) {
                fprintf(stderr, "Error: --timeout (-t) debe ser un entero entre 0 y 3600 (recibido '%s').\n", optarg);
                return ARGS_ERROR;
            }
            cfg->timeout_s = (int)v; tiene_t = 1; break;
        case 'c':
            if (parse_int(optarg, 1, 86400 * 7, &v) != 0) {
                fprintf(stderr, "Error: --tiempo-canal (-c) debe ser un entero positivo de segundos (recibido '%s').\n", optarg);
                return ARGS_ERROR;
            }
            cfg->canal_s = (int)v; tiene_c = 1; break;
        case 'r': cfg->reproductor = optarg; break;
        case 'n': cfg->sin_reproductor = 1; break;
        case 'v': cfg->verboso = 1; break;
        case ':':
            fprintf(stderr, "Error: la opcion '%s' requiere un valor.\n", argv[optind - 1]);
            return ARGS_ERROR;
        default:
            fprintf(stderr, "Error: opcion desconocida '%s'.\n", argv[optind - 1]);
            return ARGS_ERROR;
        }
    }
    (void)tiene_t;

    if (optind < argc) {
        fprintf(stderr, "Error: argumento inesperado '%s'.\n", argv[optind]);
        return ARGS_ERROR;
    }

    /* Obligatorios */
    if (cfg->direccion == NULL || *cfg->direccion == '\0') {
        fprintf(stderr, "Error: falta la direccion del servidor (-d).\n");
        return ARGS_ERROR;
    }
    if (strlen(cfg->direccion) > 253) {
        fprintf(stderr, "Error: la direccion (-d) es demasiado larga.\n");
        return ARGS_ERROR;
    }
    if (puerto_txt == NULL) {
        fprintf(stderr, "Error: falta el puerto del servidor (-p).\n");
        return ARGS_ERROR;
    }
    if (parse_int(puerto_txt, 1, 65535, &v) != 0) {
        fprintf(stderr, "Error: puerto invalido (-p): '%s'. Debe estar entre 1 y 65535.\n", puerto_txt);
        return ARGS_ERROR;
    }
    snprintf(cfg->puerto, sizeof cfg->puerto, "%ld", v);

    if (accion_txt == NULL) {
        fprintf(stderr, "Error: falta la accion (-a).\n");
        return ARGS_ERROR;
    }
    if (strcmp(accion_txt, "canal") == 0)          cfg->accion = ACCION_CANAL;
    else if (strcmp(accion_txt, "listar") == 0)    cfg->accion = ACCION_LISTAR;
    else if (strcmp(accion_txt, "descargar") == 0) cfg->accion = ACCION_DESCARGAR;
    else {
        fprintf(stderr, "Error: accion invalida (-a): '%s'. Debe ser 'canal', 'listar' o 'descargar'.\n", accion_txt);
        return ARGS_ERROR;
    }

    /* Combinaciones contradictorias (RF-67) */
    if (cfg->accion == ACCION_DESCARGAR) {
        if (cfg->archivo == NULL) {
            fprintf(stderr, "Error: la accion 'descargar' requiere el nombre del contenido (-f).\n");
            return ARGS_ERROR;
        }
        if (cfg->destino == NULL || *cfg->destino == '\0') {
            fprintf(stderr, "Error: la accion 'descargar' requiere un destino (-o).\n");
            return ARGS_ERROR;
        }
        /* Mismas reglas de nombre que el protocolo: sin '/', '\\', NUL ni '..' */
        uint8_t tmp[PROTOCOL_MAX_NAME + 2];
        size_t escrito = 0;
        size_t largo = strlen(cfg->archivo);
        if (largo == 0 || largo > PROTOCOL_MAX_NAME ||
            protocol_encode_name((const uint8_t *)cfg->archivo, (uint16_t)largo,
                                 tmp, sizeof tmp, &escrito) != PROTOCOL_OK) {
            fprintf(stderr, "Error: nombre de contenido invalido (-f): debe medir 1-%u bytes y no contener '/', '\\' ni '..'.\n",
                    PROTOCOL_MAX_NAME);
            return ARGS_ERROR;
        }
        if (resolver_destino(cfg) != 0) {
            return ARGS_ERROR;
        }
    } else {
        if (cfg->archivo != NULL) {
            fprintf(stderr, "Error: -f solo es valido con la accion 'descargar'.\n");
            return ARGS_ERROR;
        }
        if (cfg->destino != NULL) {
            fprintf(stderr, "Error: -o solo es valido con la accion 'descargar'.\n");
            return ARGS_ERROR;
        }
    }
    if (tiene_c && cfg->accion != ACCION_CANAL) {
        fprintf(stderr, "Error: -c (--tiempo-canal) solo es valido con la accion 'canal'.\n");
        return ARGS_ERROR;
    }
    if (cfg->sin_reproductor && cfg->reproductor != NULL) {
        fprintf(stderr, "Error: -n y -r son contradictorios.\n");
        return ARGS_ERROR;
    }
    if (cfg->accion == ACCION_LISTAR && (cfg->sin_reproductor || cfg->reproductor)) {
        fprintf(stderr, "Error: -n y -r no aplican a la accion 'listar'.\n");
        return ARGS_ERROR;
    }
    return ARGS_OK;
}