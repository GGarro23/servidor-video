#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include "protocol.h"

int main(void) {
    uint64_t original_size = 123456789;
    uint8_t original_format = 1;

    uint8_t buffer[9];

    assert(protocol_encode_vod_accepted(
        original_size, original_format, buffer
    ) == PROTOCOL_OK);

    printf("Tamaño original: %" PRIu64 " bytes\n", original_size);
    printf("Formato original: %u\n", (unsigned)original_format);

    printf("Payload codificado: ");
    for (size_t i = 0; i < sizeof(buffer); i++) {
        printf("%02X ", buffer[i]);
    }
    printf("\n");

    uint64_t decoded_size = 0;
    uint8_t decoded_format = 0;

    assert(protocol_decode_vod_accepted(
        buffer, sizeof(buffer),
        &decoded_size, &decoded_format
    ) == PROTOCOL_OK);

    assert(decoded_size == original_size);
    assert(decoded_format == original_format);

    printf("Tamaño decodificado: %" PRIu64 " bytes\n", decoded_size);
    printf("Formato decodificado: %u\n", (unsigned)decoded_format);

    printf("\nPruebas VOD superadas.\n");

    return 0;
}
