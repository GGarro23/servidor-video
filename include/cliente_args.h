/* cliente_args.h - Configuración del cliente y análisis de argumentos (RF-23..26, RF-64..67) */
#ifndef CLIENTE_ARGS_H
#define CLIENTE_ARGS_H

#include <limits.h>

/* Códigos de terminación (RF-29, RF-68) */
#define EXITO            0
#define ERR_ARGS         1
#define ERR_RED          2
#define ERR_CONTENIDO    3
#define ERR_CAPACIDAD    4
#define ERR_INTERNO      5
#define EXIT_INTERRUMPIDO 130   /* convención 128 + SIGINT */

typedef enum {
    ACCION_CANAL,
    ACCION_LISTAR,
    ACCION_DESCARGAR
} accion_t;

typedef struct {
    const char *direccion;       /* IP o nombre resoluble                   */
    char        puerto[6];       /* como texto, para getaddrinfo            */
    accion_t    accion;
    const char *archivo;         /* solo descargar                          */
    const char *destino;         /* tal como lo escribió el usuario         */
    char        ruta_final[PATH_MAX];   /* donde quedará el archivo         */
    char        ruta_parcial[PATH_MAX]; /* ruta_final + ".part"             */
    int         timeout_s;       /* espera de conexión/respuesta (0=infinito)*/
    int         canal_s;         /* duración máx. del canal (0=indefinida)  */
    int         verboso;
    int         sin_reproductor;
    const char *reproductor;     /* comando personalizado, o NULL           */
} cliente_config_t;

typedef enum { ARGS_OK, ARGS_AYUDA, ARGS_ERROR } args_resultado_t;

/* Valida TODO antes de tocar red o disco (RF-64). */
args_resultado_t args_analizar(int argc, char **argv, cliente_config_t *cfg);
void args_ayuda(const char *programa, int a_stderr);

#endif