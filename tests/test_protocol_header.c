#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include "protocol.h"

int main(void) {
    protocol_header_t original = {
        .version = PROTOCOL_VERSION,
        .type = PROTOCOL_MSG_LISTAR,
        .status = PROTOCOL_STATUS_EXITO,
        .request_id = 7,
        .payload_length = 0
    };

    uint8_t buffer[PROTOCOL_HEADER_SIZE];

    // Primera prueba: codificar.
    assert(protocol_encode_header(&original, buffer) == PROTOCOL_OK);

    // Comprobar cada byte esperado.
    const uint8_t esperado[PROTOCOL_HEADER_SIZE] = {
        0x01, 0x03, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x07,
        0x00, 0x00, 0x00, 0x00
    };

    for (size_t i = 0; i < PROTOCOL_HEADER_SIZE; i++) {
        assert(buffer[i] == esperado[i]);
    }

    // Segunda prueba: decodificar.
    protocol_header_t recuperado;

    assert(protocol_decode_header(buffer, &recuperado) == PROTOCOL_OK);

    assert(recuperado.version == original.version);
    assert(recuperado.type == original.type);
    assert(recuperado.status == original.status);
    assert(recuperado.request_id == original.request_id);
    assert(recuperado.payload_length == original.payload_length);

    // Tercera prueba: rechazar un encabezado inválido.
    original.request_id = 0;

    assert(protocol_encode_header(&original, buffer) == PROTOCOL_INVALID);

    printf("Todas las pruebas de cabecera pasaron correctamente.\n");

    return 0;
}
