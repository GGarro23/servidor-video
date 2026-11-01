#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "protocol.h"

int main(void) {
    protocol_header_t header = {
        .version = PROTOCOL_VERSION,
        .type = PROTOCOL_MSG_LISTAR,
        .status = PROTOCOL_STATUS_EXITO,
        .request_id = 7,
        .payload_length = 0
    };

    // LISTAR válido: sin payload.
    assert(protocol_validate_payload(
        &header, NULL
    ) == PROTOCOL_OK);

    // LISTAR inválido: tiene contenido.
    uint8_t extra = 0xAA;
    header.payload_length = 1;

    assert(protocol_validate_payload(
        &header, &extra
    ) == PROTOCOL_INVALID);

    // BIENVENIDA válida: VOD + LIVE.
    header.type = PROTOCOL_MSG_BIENVENIDA;
    uint8_t services = 0x03;

    assert(protocol_validate_payload(
        &header, &services
    ) == PROTOCOL_OK);

    // BIENVENIDA inválida: bit reservado.
    services = 0x80;

    assert(protocol_validate_payload(
        &header, &services
    ) == PROTOCOL_INVALID);

    // CATALOGO vacío válido.
    header.type = PROTOCOL_MSG_CATALOGO;
    header.payload_length = 2;

    uint8_t empty_catalog[] = {0x00, 0x00};

    assert(protocol_validate_payload(
        &header, empty_catalog
    ) == PROTOCOL_OK);

    // DATOS válido.
    header.type = PROTOCOL_MSG_DATOS;
    header.payload_length = 1;

    uint8_t data = 0x42;

    assert(protocol_validate_payload(
        &header, &data
    ) == PROTOCOL_OK);

    // DATOS inválido: vacío.
    header.payload_length = 0;

    assert(protocol_validate_payload(
        &header, NULL
    ) == PROTOCOL_INVALID);

    printf("Pruebas iniciales de validación superadas.\n");

    return 0;
}
