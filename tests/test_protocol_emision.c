#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "protocol.h"

//Arma el header de un EMISION con el largo del payload para pasarlo por protocol_validate_payload.
static protocol_header_t header_emision(size_t largo) {
    protocol_header_t h;
    h.version = PROTOCOL_VERSION;
    h.type = PROTOCOL_MSG_EMISION;
    h.status = PROTOCOL_STATUS_EXITO;
    h.request_id = 1;
    h.payload_length = (uint32_t)largo;
    return h;
}

int main(void) {
    const uint8_t nombre[] = "canal.ts";
    uint8_t buf[16 + 2 + PROTOCOL_MAX_NAME];
    size_t n = 0;

    //Caso normal: lo que se codifica lo acepta el validador y los bytes estan en big-endian.
    assert(protocol_encode_emission(11000, 5000, nombre, 8, buf, sizeof buf, &n) == PROTOCOL_OK);
    assert(n == 16 + 2 + 8);
    protocol_header_t h = header_emision(n);
    assert(protocol_validate_payload(&h, buf) == PROTOCOL_OK);
    assert(buf[6] == 0x2A && buf[7] == 0xF8);            //11000 = 0x2AF8
    assert(buf[14] == 0x13 && buf[15] == 0x88);          //5000  = 0x1388
    assert(buf[16] == 0 && buf[17] == 8);                //name_length
    assert(memcmp(buf + 18, nombre, 8) == 0);

    //Posicion 0 (conexion exacta en t0) es valida.
    assert(protocol_encode_emission(11000, 0, nombre, 8, buf, sizeof buf, &n) == PROTOCOL_OK);

    //Posicion = ciclo, o ciclo 0: invalido.
    assert(protocol_encode_emission(11000, 11000, nombre, 8, buf, sizeof buf, &n) == PROTOCOL_INVALID);
    assert(protocol_encode_emission(0, 0, nombre, 8, buf, sizeof buf, &n) == PROTOCOL_INVALID);

    //Nombre vacio, con barra o con "..": invalido.
    assert(protocol_encode_emission(11000, 1, nombre, 0, buf, sizeof buf, &n) == PROTOCOL_INVALID);
    assert(protocol_encode_emission(11000, 1, (const uint8_t *)"a/b.ts", 6, buf, sizeof buf, &n) == PROTOCOL_INVALID);
    assert(protocol_encode_emission(11000, 1, (const uint8_t *)"..", 2, buf, sizeof buf, &n) == PROTOCOL_INVALID);

    //Buffer chico y punteros nulos.
    assert(protocol_encode_emission(11000, 1, nombre, 8, buf, 20, &n) == PROTOCOL_INVALID);
    assert(protocol_encode_emission(11000, 1, NULL, 8, buf, sizeof buf, &n) == PROTOCOL_INVALID);
    assert(protocol_encode_emission(11000, 1, nombre, 8, NULL, sizeof buf, &n) == PROTOCOL_INVALID);
    assert(protocol_encode_emission(11000, 1, nombre, 8, buf, sizeof buf, NULL) == PROTOCOL_INVALID);

    //Nombre de 255 bytes (el maximo) cabe justo.
    uint8_t largo[255];
    memset(largo, 'a', sizeof largo);
    assert(protocol_encode_emission(1, 0, largo, 255, buf, sizeof buf, &n) == PROTOCOL_OK);
    assert(n == 16 + 2 + 255);

    printf("Pruebas de EMISION superadas.\n");
    return 0;
}