#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "protocol.h"

int main(void) {
    const uint8_t original[] = "video.mp4";

    uint8_t buffer[257];
    size_t written = 0;

    assert(protocol_encode_name(
        original, 9, buffer, sizeof(buffer), &written
    ) == PROTOCOL_OK);

    assert(written == 11);
    assert(buffer[0] == 0x00);
    assert(buffer[1] == 0x09);
    assert(memcmp(buffer + 2, original, 9) == 0);

    const uint8_t *decoded = NULL;
    uint16_t decoded_length = 0;

    assert(protocol_decode_name(
        buffer, written, &decoded, &decoded_length
    ) == PROTOCOL_OK);

    assert(decoded_length == 9);
    assert(memcmp(decoded, original, 9) == 0);

    printf("Pruebas de nombres superadas.\n");

    return 0;
}
