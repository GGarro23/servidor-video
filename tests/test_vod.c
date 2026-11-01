#define _POSIX_C_SOURCE 200809L

#include "vod.h"
#include "protocol.h"
#include "catalog.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define ID_PRUEBA 42u

/* Produce bytes deterministas para comparar origen y descarga. */
static void crear_archivo(const char *ruta, size_t tamano) {
    FILE *f = fopen(ruta, "wb");
    assert(f != NULL);
    for (size_t i = 0; i < tamano; i++) {
        assert(fputc((int)(i % 251u), f) != EOF);
    }
    assert(fclose(f) == 0);
}

/* read() puede devolver menos bytes de los solicitados. */
static void comparar_bloque(int fd_origen, const uint8_t *recibido, size_t n) {
    uint8_t original[PROTOCOL_MAX_PAYLOAD];
    assert(n <= sizeof(original));

    size_t total = 0;
    while (total < n) {
        ssize_t r = read(fd_origen, original + total, n - total);
        assert(r > 0);
        total += (size_t)r;
    }
    assert(memcmp(original, recibido, n) == 0);
}

static void probar_transferencia(const char *dir, const char *nombre, size_t tamano) {
    char ruta[512];
    int n = snprintf(ruta, sizeof(ruta), "%s/%s", dir, nombre);
    assert(n > 0 && (size_t)n < sizeof(ruta));
    crear_archivo(ruta, tamano);

    catalog_t *catalog = NULL;
    assert(catalog_create(dir, &catalog) == CATALOG_OK);

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    /* Emisor y receptor trabajan en procesos distintos para no bloquearse
       al llenar el buffer del socket con archivos grandes. */
    pid_t hijo = fork();
    assert(hijo >= 0);

    if (hijo == 0) {
        close(sockets[0]);
        vod_transfer_t transferencia = {.fd_archivo = -1};

        assert(vod_preparar(catalog, nombre, ID_PRUEBA, &transferencia) == CATALOG_OK);
        assert(vod_enviar_aceptado(sockets[1], &transferencia) == PROTOCOL_OK);

        for (;;) {
            vod_result_t r = vod_enviar_siguiente(sockets[1], &transferencia);
            if (r == VOD_FINALIZADO) break;
            assert(r == VOD_BLOQUE_ENVIADO);
        }

        vod_liberar(&transferencia);
        catalog_destroy(catalog);
        close(sockets[1]);
        _exit(0);
    }

    close(sockets[1]);
    int fd_origen = open(ruta, O_RDONLY);
    assert(fd_origen >= 0);

    protocol_message_t mensaje = {0};
    assert(protocol_recv_message(sockets[0], &mensaje) == PROTOCOL_OK);
    assert(mensaje.header.type == PROTOCOL_MSG_ACEPTADO);
    assert(mensaje.header.request_id == ID_PRUEBA);

    uint64_t anunciado = 0;
    uint8_t formato = 0;
    assert(protocol_decode_vod_accepted(
        mensaje.payload, mensaje.header.payload_length,
        &anunciado, &formato) == PROTOCOL_OK);
    assert(anunciado == tamano && formato == 1);
    protocol_message_free(&mensaje);

    uint64_t recibidos = 0;
    size_t bloques = 0;
    for (;;) {
        assert(protocol_recv_message(sockets[0], &mensaje) == PROTOCOL_OK);
        assert(mensaje.header.request_id == ID_PRUEBA);

        if (mensaje.header.type == PROTOCOL_MSG_FIN) {
            assert(mensaje.header.payload_length == 0);
            protocol_message_free(&mensaje);
            break;
        }

        assert(mensaje.header.type == PROTOCOL_MSG_DATOS);
        assert(mensaje.header.payload_length > 0);
        comparar_bloque(fd_origen, mensaje.payload, mensaje.header.payload_length);
        recibidos += mensaje.header.payload_length;
        bloques++;
        protocol_message_free(&mensaje);
    }

    assert(recibidos == tamano);
    if (tamano == 0) assert(bloques == 0);
    if (tamano > 32768u) assert(bloques > 1);

    /* El emisor ya terminó; no puede haber más DATOS ni otro FIN. */
    assert(protocol_recv_message(sockets[0], &mensaje) == PROTOCOL_EOF);

    int estado = 0;
    assert(waitpid(hijo, &estado, 0) == hijo);
    assert(WIFEXITED(estado) && WEXITSTATUS(estado) == 0);

    close(fd_origen);
    close(sockets[0]);
    catalog_destroy(catalog);
    assert(unlink(ruta) == 0);
    printf("OK: %s (%zu bytes, %zu bloques)\n", nombre, tamano, bloques);
}

int main(void) {
    char directorio[] = "/tmp/test-vod-XXXXXX";
    assert(mkdtemp(directorio) != NULL);

    probar_transferencia(directorio, "vacio.mp4", 0);
    probar_transferencia(directorio, "pequeno.mp4", 1000);
    probar_transferencia(directorio, "grande.mp4", 100000);

    assert(rmdir(directorio) == 0);
    puts("Todas las pruebas VOD pasaron.");
    return 0;
}

