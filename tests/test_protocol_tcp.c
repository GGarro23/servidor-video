#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"

int main(void) {
    int sockets[2];

    assert(socketpair(
        AF_UNIX,
        SOCK_STREAM,
        0,
        sockets
    ) == 0);

    // Simulamos un bloque de video.
    uint8_t video_data[] = {
        0x10, 0x20, 0x30, 0x40, 0x50
    };

    protocol_header_t header = {
        .version = PROTOCOL_VERSION,
        .type = PROTOCOL_MSG_DATOS,
        .status = PROTOCOL_STATUS_EXITO,
        .request_id = 7,
        .payload_length = sizeof(video_data)
    };

    // Enviar por el primer socket.
    assert(protocol_send_message(
        sockets[0],
        &header,
        video_data
    ) == PROTOCOL_OK);

    printf("Mensaje DATOS enviado.\n");

    // Recibir por el segundo socket.
    protocol_message_t received = {0};

    assert(protocol_recv_message(
        sockets[1],
        &received
    ) == PROTOCOL_OK);

    // Verificar la cabecera.
    assert(received.header.version == PROTOCOL_VERSION);
    assert(received.header.type == PROTOCOL_MSG_DATOS);
    assert(received.header.request_id == 7);
    assert(received.header.payload_length == sizeof(video_data));

    // Verificar que los datos sean idénticos.
    assert(memcmp(
        received.payload,
        video_data,
        sizeof(video_data)
    ) == 0);

    printf("Mensaje DATOS recibido.\n");

    printf("Bytes recibidos: ");
    for (size_t i = 0; i < received.header.payload_length; i++) {
        printf("%02X ", received.payload[i]);
    }
    printf("\n");

    // Liberar la memoria del mensaje.
    protocol_message_free(&received);

    close(sockets[0]);
    close(sockets[1]);

    printf("Prueba TCP superada.\n");

    return 0;
}
