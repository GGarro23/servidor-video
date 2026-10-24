#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Códigos de terminación (RF-68)
#define EXITO 0
#define ERR_ARGS 1
#define ERR_RED 2
#define ERR_CONTENIDO 3
#define ERR_CAPACIDAD 4
#define ERR_INTERNO 5

// Función para imprimir la ayuda (RF-26)
void imprimir_ayuda(const char *nombre_programa) {
    printf("Uso: %s [OPCIONES]\n", nombre_programa);
    printf("Opciones:\n");
    printf("  -d, --direccion <IP>     Dirección del servidor (requerido)\n");
    printf("  -p, --puerto <PUERTO>    Puerto del servidor (requerido)\n");
    printf("  -a, --accion <ACCION>    Acción: canal | listar | descargar (requerido)\n");
    printf("  -f, --archivo <NOMBRE>   Nombre del contenido a descargar (requerido si accion=descargar)\n");
    printf("  -o, --destino <RUTA>     Ruta donde guardar el archivo (requerido si accion=descargar)\n");
    printf("  -h, --help               Muestra esta ayuda\n");
    printf("\nEjemplos:\n");
    printf("  %s -d 127.0.0.1 -p 8080 -a listar\n", nombre_programa);
    printf("  %s -d 127.0.0.1 -p 8080 -a descargar -f video.mp4 -o /tmp/video.mp4\n", nombre_programa);
}

int main(int argc, char *argv[]) {
    // guardar los parámetros nulos
    char *direccion = NULL;
    int puerto = 0;
    char *accion = NULL;
    char *archivo = NULL;
    char *destino = NULL;

    // si no hay argumentos, mostramos ayuda y salimos con error
    if (argc < 2) {
        imprimir_ayuda(argv[0]);
        return ERR_ARGS;
    }

    // Por ahora, una lectura manual básica, después getopt
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            imprimir_ayuda(argv[0]);
            return EXITO;
        } else if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--direccion") == 0) && i + 1 < argc) {
            direccion = argv[++i];
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--puerto") == 0) && i + 1 < argc) {
            // atoi es string to int
            puerto = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--accion") == 0) && i + 1 < argc) {
            accion = argv[++i];
        } else if ((strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--archivo") == 0) && i + 1 < argc) {
            archivo = argv[++i];
        } else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--destino") == 0) && i + 1 < argc) {
            destino = argv[++i];
        } else {
            fprintf(stderr, "Error: Argumento desconocido o incompleto: %s\n", argv[i]);
            return ERR_ARGS;
        }
    }

    // Validaciones logicas (RF-25 y RF-67)
    if (direccion == NULL) {
        fprintf(stderr, "Error: Falta la dirección del servidor (-d).\n");
        return ERR_ARGS;
    }
    if (puerto <= 0 || puerto > 65535) {
        fprintf(stderr, "Error: Puerto inválido (-p).\n");
        return ERR_ARGS;
    }
    if (accion == NULL) {
        fprintf(stderr, "Error: Falta la acción (-a).\n");
        return ERR_ARGS;
    }
    
    // Validar que la acción sea una de las permitidas
    if (strcmp(accion, "canal") != 0 && strcmp(accion, "listar") != 0 && strcmp(accion, "descargar") != 0) {
        fprintf(stderr, "Error: Acción inválida. Debe ser 'canal', 'listar' o 'descargar'.\n");
        return ERR_ARGS;
    }

    // Validaciones específicas si la acción es descargar
    if (strcmp(accion, "descargar") == 0) {
        if (archivo == NULL || destino == NULL) {
            fprintf(stderr, "Error: Para 'descargar' se requiere archivo (-f) y destino (-o).\n");
            return ERR_ARGS;
        }
    }

    // Si todo está bien, mostramos lo que el usuario ingresó
    printf("--- Configuración del Cliente ---\n");
    printf("Servidor: %s\n", direccion);
    printf("Puerto: %d\n", puerto);
    printf("Acción: %s\n", accion);
    if (archivo) printf("Archivo: %s\n", archivo);
    if (destino) printf("Destino: %s\n", destino);
    printf("---------------------------------\n");
    printf("Validación exitosa. Listo para conectar (pendiente).\n");

    return EXITO;
}