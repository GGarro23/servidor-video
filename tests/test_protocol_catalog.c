#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "protocol.h"

int main(void) {
    uint8_t buffer[PROTOCOL_MAX_PAYLOAD];
    size_t written = 0;

    // Prueba 1: catalogo vacio.
    assert(protocol_encode_catalog(
        NULL, 0, buffer, sizeof(buffer), &written
    ) == PROTOCOL_OK);

    assert(written == 2);
    assert(buffer[0] == 0 && buffer[1] == 0);

    printf("OK: catalogo vacio\n");

    // Prueba 2: multiples entradas.
    protocol_catalog_entry_t entries[] = {
        {(const uint8_t *)"video.mp4", 9, 123456789},
        {(const uint8_t *)"pelicula.mkv", 12, 987654321},
        {(const uint8_t *)"demo.avi", 8, 5000}
    };

    assert(protocol_encode_catalog(
        entries, 3, buffer, sizeof(buffer), &written
    ) == PROTOCOL_OK);

    assert(written == 61);
    assert(buffer[0] == 0 && buffer[1] == 3);

    // Comprobar compatibilidad con el validador existente.
    protocol_header_t header = {
        .version = PROTOCOL_VERSION,
        .type = PROTOCOL_MSG_CATALOGO,
        .status = PROTOCOL_STATUS_EXITO,
        .request_id = 1,
        .payload_length = (uint32_t)written
    };

    assert(protocol_validate_payload(
        &header, buffer
    ) == PROTOCOL_OK);

    printf("OK: catalogo de tres archivos\n");

    // Prueba 3: buffer insuficiente.
    written = 999;

    assert(protocol_encode_catalog(
        entries, 3, buffer, 10, &written
    ) == PROTOCOL_INVALID);

    assert(written == 0);
    printf("OK: buffer insuficiente\n");

    // Prueba 4: nombre duplicado.
    protocol_catalog_entry_t duplicates[] = {
        {(const uint8_t *)"video.mp4", 9, 100},
        {(const uint8_t *)"video.mp4", 9, 200}
    };

    assert(protocol_encode_catalog(
        duplicates, 2, buffer, sizeof(buffer), &written
    ) == PROTOCOL_INVALID);

    printf("OK: nombres duplicados\n");

    // Prueba 5: cantidad superior al limite.
    assert(protocol_encode_catalog(
        entries, 256, buffer, sizeof(buffer), &written
    ) == PROTOCOL_INVALID);

    printf("OK: limite de entradas\n");

    // Prueba 6: nombre invalido.
    protocol_catalog_entry_t invalid[] = {
        {(const uint8_t *)"../video.mp4", 12, 100}
    };

    assert(protocol_encode_catalog(
        invalid, 1, buffer, sizeof(buffer), &written
    ) == PROTOCOL_INVALID);

    printf("OK: nombre invalido\n");

    puts("Todas las pruebas de catalogo superadas.");
    return 0;
}
