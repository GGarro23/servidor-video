//Includes
#include <stdint.h>
#include <stddef.h>

//Proteccion contra inclusiones multiples
#ifndef PROTOCOL_H
#define PROTOCOL_H

//defines de tamaños
#define PROTOCOL_VERSION 1u
#define PROTOCOL_HEADER_SIZE 12u
#define PROTOCOL_MAX_PAYLOAD 65536u
#define PROTOCOL_MAX_NAME 255u

//Estructuras de cabeceras y mensaje.
typedef struct {
    uint8_t version;
    uint8_t type;
    uint16_t status;
    uint32_t request_id;
    uint32_t payload_length;
} protocol_header_t;

typedef struct {
    protocol_header_t header;
    uint8_t *payload;
} protocol_message_t;

typedef struct {
    const uint8_t *name;
    uint16_t name_length;
    uint64_t file_size;
} protocol_catalog_entry_t;

//ENUMS de los tipos, estados y resultados.
typedef enum {
    PROTOCOL_MSG_SALUDO = 1,
    PROTOCOL_MSG_BIENVENIDA = 2, 
    PROTOCOL_MSG_LISTAR = 3,
    PROTOCOL_MSG_CATALOGO = 4,
    PROTOCOL_MSG_ABRIR = 5,
    PROTOCOL_MSG_ACEPTADO = 6,
    PROTOCOL_MSG_DATOS = 7,
    PROTOCOL_MSG_FIN = 8,
    PROTOCOL_MSG_ERROR = 9,
    PROTOCOL_MSG_CERRAR = 10,
    PROTOCOL_MSG_DESPEDIDA = 11,
    PROTOCOL_MSG_UNIR = 12,
    PROTOCOL_MSG_EMISION = 13,
    PROTOCOL_MSG_SALIR = 14,
    PROTOCOL_MSG_CANCELAR = 15,
    PROTOCOL_MSG_CANCELADO = 16
} protocol_message_type_t;

typedef enum {
    PROTOCOL_STATUS_EXITO = 0,
    PROTOCOL_STATUS_VERSION,
    PROTOCOL_STATUS_SOLICITUD,
    PROTOCOL_STATUS_NO_HALLADO,
    PROTOCOL_STATUS_NO_SERVIBLE,
    PROTOCOL_STATUS_NO_PERMITIDO,
    PROTOCOL_STATUS_CAPACIDAD,
    PROTOCOL_STATUS_INTERNO
} protocol_status_t;

typedef enum {
    PROTOCOL_OK = 0,
    PROTOCOL_EOF,
    PROTOCOL_TRUNCATED,
    PROTOCOL_INVALID,
    PROTOCOL_IO_ERROR,
    PROTOCOL_NO_MEMORY
} protocol_result_t;

//declaracion de funciones publicas

// Cabeceras binarias
protocol_result_t protocol_encode_header(
    const protocol_header_t *header,
    uint8_t out[PROTOCOL_HEADER_SIZE]);

protocol_result_t protocol_decode_header(
    const uint8_t in[PROTOCOL_HEADER_SIZE],
    protocol_header_t *out);

// Envío y recepción TCP
protocol_result_t protocol_send_message(
    int fd,
    const protocol_header_t *header,
    const uint8_t *payload);

protocol_result_t protocol_recv_message(
    int fd,
    protocol_message_t *out);

// Liberación de memoria
void protocol_message_free(protocol_message_t *message);

// Nombres de archivos
protocol_result_t protocol_encode_name(
    const uint8_t *name,
    uint16_t name_length,
    uint8_t *out,
    size_t capacity,
    size_t *written);

protocol_result_t protocol_decode_name(
    const uint8_t *payload,
    size_t length,
    const uint8_t **name,
    uint16_t *name_length);

// Metadatos de video bajo demanda
protocol_result_t protocol_encode_vod_accepted(
    uint64_t file_size,
    uint8_t transfer_format,
    uint8_t out[9]);

protocol_result_t protocol_decode_vod_accepted(
    const uint8_t *payload,
    size_t length,
    uint64_t *file_size,
    uint8_t *transfer_format);

// Validación
protocol_result_t protocol_validate_payload(
    const protocol_header_t *header,
    const uint8_t *payload);

protocol_result_t protocol_encode_catalog(
    const protocol_catalog_entry_t *entries,
    uint16_t entry_count,
    uint8_t *out,
    size_t capacity,
    size_t *written
);
//cierre de protocol_h
#endif
