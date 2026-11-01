#include "protocol.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

protocol_result_t protocol_encode_header(
    const protocol_header_t *header,
    uint8_t out[PROTOCOL_HEADER_SIZE]
) {
    // 1. Validar punteros.
    if (header == NULL || out == NULL) {
        return PROTOCOL_INVALID;
    }

    // 2. Validar la versión del protocolo.
    if (header->version != PROTOCOL_VERSION) {
        return PROTOCOL_INVALID;
    }

    // 3. Validar el tipo de mensaje.
    if (header->type < PROTOCOL_MSG_SALUDO ||
        header->type > PROTOCOL_MSG_CANCELADO) {
        return PROTOCOL_INVALID;
    }

    // 4. Validar el identificador de solicitud.
    if (header->request_id == 0) {
        return PROTOCOL_INVALID;
    }

    // 5. Validar el tamaño del payload.
    if (header->payload_length > PROTOCOL_MAX_PAYLOAD) {
        return PROTOCOL_INVALID;
    }

    // 6. Validar el estado del mensaje.
    if (header->type == PROTOCOL_MSG_ERROR) {
        if (header->status < PROTOCOL_STATUS_VERSION ||
            header->status > PROTOCOL_STATUS_INTERNO) {
            return PROTOCOL_INVALID;
        }
    } else if (header->status != PROTOCOL_STATUS_EXITO) {
        return PROTOCOL_INVALID;
    }

    // 7. Codificar versión y tipo (1 byte cada uno).
    out[0] = header->version;
    out[1] = header->type;

    // 8. Codificar estado (2 bytes, big-endian).
    out[2] = (uint8_t)(header->status >> 8);
    out[3] = (uint8_t)(header->status);

    // 9. Codificar request_id (4 bytes, big-endian).
    out[4] = (uint8_t)(header->request_id >> 24);
    out[5] = (uint8_t)(header->request_id >> 16);
    out[6] = (uint8_t)(header->request_id >> 8);
    out[7] = (uint8_t)(header->request_id);

    // 10. Codificar payload_length (4 bytes, big-endian).
    out[8]  = (uint8_t)(header->payload_length >> 24);
    out[9]  = (uint8_t)(header->payload_length >> 16);
    out[10] = (uint8_t)(header->payload_length >> 8);
    out[11] = (uint8_t)(header->payload_length);

    return PROTOCOL_OK;
}

protocol_result_t protocol_decode_header(
    const uint8_t in[PROTOCOL_HEADER_SIZE],
    protocol_header_t *out
) {
    // 1. Validar los punteros.
    if (in == NULL || out == NULL) {
        return PROTOCOL_INVALID;
    }

    // 2. Reconstruir versión y tipo.
    protocol_header_t header;

    header.version = in[0];
    header.type = in[1];

    // 3. Reconstruir status (2 bytes).
    header.status =
        ((uint16_t)in[2] << 8) |
        (uint16_t)in[3];

    // 4. Reconstruir request_id (4 bytes).
    header.request_id =
        ((uint32_t)in[4] << 24) |
        ((uint32_t)in[5] << 16) |
        ((uint32_t)in[6] << 8)  |
        (uint32_t)in[7];

    // 5. Reconstruir payload_length (4 bytes).
    header.payload_length =
        ((uint32_t)in[8] << 24)  |
        ((uint32_t)in[9] << 16) |
        ((uint32_t)in[10] << 8) |
        (uint32_t)in[11];

    // 6. Validar la versión.
    if (header.version != PROTOCOL_VERSION) {
        return PROTOCOL_INVALID;
    }

    // 7. Validar el tipo.
    if (header.type < PROTOCOL_MSG_SALUDO ||
        header.type > PROTOCOL_MSG_CANCELADO) {
        return PROTOCOL_INVALID;
    }

    // 8. Validar request_id.
    if (header.request_id == 0) {
        return PROTOCOL_INVALID;
    }

    // 9. Validar payload_length.
    if (header.payload_length > PROTOCOL_MAX_PAYLOAD) {
        return PROTOCOL_INVALID;
    }

    // 10. Validar status.
    if (header.type == PROTOCOL_MSG_ERROR) {
        if (header.status < PROTOCOL_STATUS_VERSION ||
            header.status > PROTOCOL_STATUS_INTERNO) {
            return PROTOCOL_INVALID;
        }
    } else if (header.status != PROTOCOL_STATUS_EXITO) {
        return PROTOCOL_INVALID;
    }

    // 11. Guardar la estructura validada.
    *out = header;

    return PROTOCOL_OK;
}

protocol_result_t protocol_encode_name(
    const uint8_t *name,
    uint16_t name_length,
    uint8_t *out,
    size_t capacity,
    size_t *written
) {
    // Validar punteros.
    if (name == NULL || out == NULL || written == NULL) {
        return PROTOCOL_INVALID;
    }

    *written = 0;

    // Validar longitud.
    if (name_length == 0 || name_length > PROTOCOL_MAX_NAME) {
        return PROTOCOL_INVALID;
    }

    // Validar capacidad del buffer.
    size_t required = 2u + (size_t)name_length;

    if (capacity < required) {
        return PROTOCOL_INVALID;
    }

    // Validar caracteres prohibidos.
    for (size_t i = 0; i < name_length; i++) {
        if (name[i] == '\0' ||
            name[i] == '/' ||
            name[i] == '\\') {
            return PROTOCOL_INVALID;
        }
    }

    // Rechazar el componente de ruta "..".
    for (size_t i = 0; i + 1 < name_length; i++) {
        if (name[i] == '.' && name[i + 1] == '.') {
            return PROTOCOL_INVALID;
        }
    }

    // Escribir longitud en big-endian.
    out[0] = (uint8_t)(name_length >> 8);
    out[1] = (uint8_t)(name_length);

    // Copiar los bytes del nombre.
    memcpy(out + 2, name, name_length);

    // Indicar cuántos bytes se escribieron.
    *written = required;

    return PROTOCOL_OK;
}

protocol_result_t protocol_decode_name(
    const uint8_t *payload,
    size_t length,
    const uint8_t **name,
    uint16_t *name_length
) {
    // Validar punteros.
    if (payload == NULL ||
        name == NULL ||
        name_length == NULL) {
        return PROTOCOL_INVALID;
    }

    *name = NULL;
    *name_length = 0;

    // Necesitamos al menos 2 bytes para la longitud.
    if (length < 2) {
        return PROTOCOL_INVALID;
    }

    // Reconstruir longitud en big-endian.
    uint16_t decoded_length =
        ((uint16_t)payload[0] << 8) |
        (uint16_t)payload[1];

    // Validar longitud del nombre.
    if (decoded_length == 0 ||
        decoded_length > PROTOCOL_MAX_NAME) {
        return PROTOCOL_INVALID;
    }

    // El payload debe contener exactamente
    // 2 bytes de longitud + los bytes del nombre.
    if (length != 2u + (size_t)decoded_length) {
        return PROTOCOL_INVALID;
    }

    // Validar caracteres prohibidos.
    const uint8_t *decoded_name = payload + 2;

    for (size_t i = 0; i < decoded_length; i++) {
        if (decoded_name[i] == '\0' ||
            decoded_name[i] == '/' ||
            decoded_name[i] == '\\') {
            return PROTOCOL_INVALID;
        }
    }

    // Rechazar el componente de ruta "..".
    for (size_t i = 0; i + 1 < decoded_length; i++) {
        if (decoded_name[i] == '.' &&
            decoded_name[i + 1] == '.') {
            return PROTOCOL_INVALID;
        }
    }

    // Devolver una vista al nombre dentro del payload.
    *name = decoded_name;
    *name_length = decoded_length;

    return PROTOCOL_OK;
}

protocol_result_t protocol_encode_vod_accepted(
    uint64_t file_size,
    uint8_t transfer_format,
    uint8_t out[9]
) {
    if (out == NULL || transfer_format != 1) {
        return PROTOCOL_INVALID;
    }

    out[0] = (uint8_t)(file_size >> 56);
    out[1] = (uint8_t)(file_size >> 48);
    out[2] = (uint8_t)(file_size >> 40);
    out[3] = (uint8_t)(file_size >> 32);
    out[4] = (uint8_t)(file_size >> 24);
    out[5] = (uint8_t)(file_size >> 16);
    out[6] = (uint8_t)(file_size >> 8);
    out[7] = (uint8_t)(file_size);

    out[8] = transfer_format;

    return PROTOCOL_OK;
}

protocol_result_t protocol_decode_vod_accepted(
    const uint8_t *payload,
    size_t length,
    uint64_t *file_size,
    uint8_t *transfer_format
) {
    if (payload == NULL ||
        file_size == NULL ||
        transfer_format == NULL) {
        return PROTOCOL_INVALID;
    }

    if (length != 9 || payload[8] != 1) {
        return PROTOCOL_INVALID;
    }

    uint64_t decoded_size =
        ((uint64_t)payload[0] << 56) |
        ((uint64_t)payload[1] << 48) |
        ((uint64_t)payload[2] << 40) |
        ((uint64_t)payload[3] << 32) |
        ((uint64_t)payload[4] << 24) |
        ((uint64_t)payload[5] << 16) |
        ((uint64_t)payload[6] << 8)  |
        (uint64_t)payload[7];

    *file_size = decoded_size;
    *transfer_format = payload[8];

    return PROTOCOL_OK;
}

// Lee un entero de 16 bits en big-endian.
static uint16_t read_u16_be(const uint8_t *data) {
    return ((uint16_t)data[0] << 8) |
           (uint16_t)data[1];
}

// Lee un entero de 64 bits en big-endian.
static uint64_t read_u64_be(const uint8_t *data) {
    uint64_t value = 0;

    for (size_t i = 0; i < 8; i++) {
        value = (value << 8) | (uint64_t)data[i];
    }

    return value;
}

protocol_result_t protocol_validate_payload(
    const protocol_header_t *header,
    const uint8_t *payload
) {
    if (header == NULL) {
        return PROTOCOL_INVALID;
    }

    uint32_t length = header->payload_length;

    // Validar la cabecera.
    if (header->version != PROTOCOL_VERSION ||
        header->type < PROTOCOL_MSG_SALUDO ||
        header->type > PROTOCOL_MSG_CANCELADO ||
        header->request_id == 0 ||
        length > PROTOCOL_MAX_PAYLOAD) {
        return PROTOCOL_INVALID;
    }

    // Validar el estado.
    if (header->type == PROTOCOL_MSG_ERROR) {
        if (header->status < PROTOCOL_STATUS_VERSION ||
            header->status > PROTOCOL_STATUS_INTERNO) {
            return PROTOCOL_INVALID;
        }
    } else if (header->status != PROTOCOL_STATUS_EXITO) {
        return PROTOCOL_INVALID;
    }

    // Un payload no vacío necesita un buffer.
    if (length > 0 && payload == NULL) {
        return PROTOCOL_INVALID;
    }

    switch (header->type) {

        // Mensajes sin payload.
        case PROTOCOL_MSG_SALUDO:
        case PROTOCOL_MSG_LISTAR:
        case PROTOCOL_MSG_FIN:
        case PROTOCOL_MSG_CERRAR:
        case PROTOCOL_MSG_DESPEDIDA:
        case PROTOCOL_MSG_UNIR:
        case PROTOCOL_MSG_SALIR:
        case PROTOCOL_MSG_CANCELAR:
        case PROTOCOL_MSG_CANCELADO:
            return length == 0
                ? PROTOCOL_OK
                : PROTOCOL_INVALID;

        // Máscara de servicios: VOD y LIVE.
        case PROTOCOL_MSG_BIENVENIDA:
            if (length != 1) {
                return PROTOCOL_INVALID;
            }

            if (payload[0] == 0 ||
                (payload[0] & 0xFCu) != 0) {
                return PROTOCOL_INVALID;
            }

            return PROTOCOL_OK;

        // Nombre de contenido.
        case PROTOCOL_MSG_ABRIR: {
            const uint8_t *name = NULL;
            uint16_t name_length = 0;

            return protocol_decode_name(
                payload, length, &name, &name_length
            );
        }

        // ACEPTADO: VOD (9 bytes) o SALIR (vacío).
        case PROTOCOL_MSG_ACEPTADO: {
            if (length == 0) {
                return PROTOCOL_OK;
            }

            uint64_t file_size = 0;
            uint8_t transfer_format = 0;

            return protocol_decode_vod_accepted(
                payload, length,
                &file_size, &transfer_format
            );
        }

        // Bloque de datos.
        case PROTOCOL_MSG_DATOS:
            return length > 0
                ? PROTOCOL_OK
                : PROTOCOL_INVALID;

        // Mensaje de error.
        case PROTOCOL_MSG_ERROR:
            // La descripción puede estar vacía.
            // No se admite NUL en el texto.
            for (size_t i = 0; i < length; i++) {
                if (payload[i] == '\0') {
                    return PROTOCOL_INVALID;
                }
            }

            return PROTOCOL_OK;

        // Catálogo de archivos.
        case PROTOCOL_MSG_CATALOGO: {
            if (length < 2) {
                return PROTOCOL_INVALID;
            }

            uint16_t entry_count = read_u16_be(payload);

            if (entry_count > 255) {
                return PROTOCOL_INVALID;
            }

            size_t offset = 2;

            for (uint16_t i = 0; i < entry_count; i++) {

                // Se necesitan al menos 2 bytes
                // para leer la longitud del nombre.
                if ((size_t)length - offset < 2) {
                    return PROTOCOL_INVALID;
                }

                uint16_t name_length =
                    read_u16_be(payload + offset);

                if (name_length == 0 ||
                    name_length > PROTOCOL_MAX_NAME) {
                    return PROTOCOL_INVALID;
                }

                // Cada entrada contiene:
                // 2 bytes de longitud,
                // N bytes de nombre,
                // 8 bytes de tamaño.
                size_t entry_size =
                    2u + (size_t)name_length + 8u;

                if ((size_t)length - offset < entry_size) {
                    return PROTOCOL_INVALID;
                }

                // Reutilizar el validador de nombres.
                const uint8_t *name = NULL;
                uint16_t decoded_length = 0;

                protocol_result_t result =
                    protocol_decode_name(
                        payload + offset,
                        2u + (size_t)name_length,
                        &name,
                        &decoded_length
                    );

                if (result != PROTOCOL_OK) {
                    return result;
                }

                offset += entry_size;
            }

            // No deben sobrar bytes.
            return offset == length
                ? PROTOCOL_OK
                : PROTOCOL_INVALID;
        }

        // Metadatos del canal LIVE.
        case PROTOCOL_MSG_EMISION: {
            // 8 bytes de duración,
            // 8 bytes de posición,
            // 2 bytes de longitud del nombre.
            if (length < 18) {
                return PROTOCOL_INVALID;
            }

            uint64_t cycle_duration =
                read_u64_be(payload);

            uint64_t position =
                read_u64_be(payload + 8);

            if (cycle_duration == 0 ||
                position >= cycle_duration) {
                return PROTOCOL_INVALID;
            }

            // Los bytes restantes contienen
            // el nombre codificado.
            const uint8_t *name = NULL;
            uint16_t name_length = 0;

            return protocol_decode_name(
                payload + 16,
                (size_t)length - 16u,
                &name,
                &name_length
            );
        }

        default:
            return PROTOCOL_INVALID;
    }
}

static protocol_result_t send_all(
    int fd,
    const uint8_t *buffer,
    size_t length
) {
    size_t sent = 0;

    while (sent < length) {
        ssize_t result = send(
            fd,
            buffer + sent,
            length - sent,
            MSG_NOSIGNAL
        );

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return PROTOCOL_IO_ERROR;
        }

        if (result == 0) {
            return PROTOCOL_IO_ERROR;
        }

        sent += (size_t)result;
    }

    return PROTOCOL_OK;
}

protocol_result_t protocol_send_message(
    int fd,
    const protocol_header_t *header,
    const uint8_t *payload
) {
    if (fd < 0 || header == NULL) {
        return PROTOCOL_INVALID;
    }

    protocol_result_t result =
        protocol_validate_payload(header, payload);

    if (result != PROTOCOL_OK) {
        return result;
    }

    uint8_t encoded_header[PROTOCOL_HEADER_SIZE];

    result = protocol_encode_header(
        header,
        encoded_header
    );

    if (result != PROTOCOL_OK) {
        return result;
    }

    result = send_all(
        fd,
        encoded_header,
        PROTOCOL_HEADER_SIZE
    );

    if (result != PROTOCOL_OK) {
        return result;
    }

    if (header->payload_length > 0) {
        result = send_all(
            fd,
            payload,
            header->payload_length
        );

        if (result != PROTOCOL_OK) {
            return result;
        }
    }

    return PROTOCOL_OK;
}

static protocol_result_t recv_all(
    int fd,
    uint8_t *buffer,
    size_t length,
    int allow_eof
) {
    size_t received = 0;

    while (received < length) {
        ssize_t result = recv(
            fd,
            buffer + received,
            length - received,
            0
        );

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return PROTOCOL_IO_ERROR;
        }

        if (result == 0) {
            if (received == 0 && allow_eof) {
                return PROTOCOL_EOF;
            }

            return PROTOCOL_TRUNCATED;
        }

        received += (size_t)result;
    }

    return PROTOCOL_OK;
}

protocol_result_t protocol_recv_message(
    int fd,
    protocol_message_t *out
) {
    if (fd < 0 || out == NULL) {
        return PROTOCOL_INVALID;
    }

    // Inicializar la salida.
    *out = (protocol_message_t){0};

    uint8_t encoded_header[PROTOCOL_HEADER_SIZE];

    // Recibir los 12 bytes de cabecera.
    protocol_result_t result = recv_all(
        fd,
        encoded_header,
        PROTOCOL_HEADER_SIZE,
        1
    );

    if (result != PROTOCOL_OK) {
        return result;
    }

    // Interpretar y validar la cabecera.
    protocol_header_t header;

    result = protocol_decode_header(
        encoded_header,
        &header
    );

    if (result != PROTOCOL_OK) {
        return result;
    }

    uint8_t *payload = NULL;

    // Reservar memoria solamente si hay payload.
    if (header.payload_length > 0) {
        payload = malloc(header.payload_length);

        if (payload == NULL) {
            return PROTOCOL_NO_MEMORY;
        }

        result = recv_all(
            fd,
            payload,
            header.payload_length,
            0
        );

        if (result != PROTOCOL_OK) {
            free(payload);
            return result;
        }
    }

    // Comprobar el formato del mensaje completo.
    result = protocol_validate_payload(
        &header,
        payload
    );

    if (result != PROTOCOL_OK) {
        free(payload);
        return result;
    }

    // Transferir el mensaje al llamador.
    out->header = header;
    out->payload = payload;

    return PROTOCOL_OK;
}

void protocol_message_free(
    protocol_message_t *message
) {
    if (message == NULL) {
        return;
    }

    free(message->payload);

    *message = (protocol_message_t){0};
}


