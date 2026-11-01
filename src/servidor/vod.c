#include "vod.h"

#define VOD_BLOCK_SIZE 32768u

#include <unistd.h>
#include <errno.h>

// Prepara una transferencia VOD a partir de un archivo del catálogo.
catalog_result_t vod_preparar(
    const catalog_t *catalog,
    const char *nombre,
    uint32_t request_id,
    vod_transfer_t *transfer
) {
    // Validación de argumentos.
    if (transfer == NULL) {
        return CATALOG_INVALID;
    }

    // Inicializar la estructura.
    transfer->fd_archivo = -1;
    transfer->tamano = 0;
    transfer->enviados = 0;
    transfer->request_id = 0;

    if (catalog == NULL || nombre == NULL || request_id == 0) {
        return CATALOG_INVALID;
    }

    // Variables para recibir el descriptor y tamaño.
    int fd = -1;
    uint64_t tamano = 0;

    // Solicitar apertura segura al catálogo.
    catalog_result_t resultado = catalog_open(
        catalog,
        nombre,
        &fd,
        &tamano
    );

    // Propagar cualquier error del catálogo.
    if (resultado != CATALOG_OK) {
        return resultado;
    }

    // Registrar los datos de la transferencia.
    transfer->fd_archivo = fd;
    transfer->tamano = tamano;
    transfer->enviados = 0;
    transfer->request_id = request_id;
    transfer->fin_enviado = false;

    return CATALOG_OK;
}

// Libera los recursos de una transferencia VOD.
void vod_liberar(vod_transfer_t *transfer) {
    if (transfer == NULL) {
        return;
    }

    // Cerrar el archivo si está abierto.
    if (transfer->fd_archivo >= 0) {
        close(transfer->fd_archivo);
    }

    // Restablecer la estructura.
    transfer->fd_archivo = -1;
    transfer->tamano = 0;
    transfer->enviados = 0;
    transfer->request_id = 0;
}


protocol_result_t vod_enviar_aceptado(
    int fd_cliente,
    const vod_transfer_t *transfer
) {
    // Validar la transferencia y el socket.
    if (fd_cliente < 0 || transfer == NULL ||
        transfer->fd_archivo < 0 ||
        transfer->request_id == 0) {
        return PROTOCOL_INVALID;
    }

    // Codificar los metadatos de la transferencia.
    uint8_t payload[9];

    protocol_result_t resultado = protocol_encode_vod_accepted(
        transfer->tamano,
        1,
        payload
    );

    if (resultado != PROTOCOL_OK) {
        return resultado;
    }

    // Preparar la cabecera ACEPTADO.
    protocol_header_t cabecera = {
        .version = PROTOCOL_VERSION,
        .type = PROTOCOL_MSG_ACEPTADO,
        .status = PROTOCOL_STATUS_EXITO,
        .request_id = transfer->request_id,
        .payload_length = sizeof(payload)
    };

    // Reutilizar el envío existente del protocolo.
    return protocol_send_message(
        fd_cliente,
        &cabecera,
        payload
    );
}


vod_result_t vod_enviar_siguiente(
    int fd_cliente,
    vod_transfer_t *transfer
) {
    // Validar el estado de la transferencia.
    if (fd_cliente < 0 || transfer == NULL ||
        transfer->fd_archivo < 0 ||
        transfer->request_id == 0 ||
        transfer->fin_enviado ||
        transfer->enviados > transfer->tamano) {
        return VOD_ERROR_ESTADO;
    }

    // Si ya enviamos todos los bytes, enviar FIN.
    if (transfer->enviados == transfer->tamano) {
        protocol_header_t cabecera = {
            .version = PROTOCOL_VERSION,
            .type = PROTOCOL_MSG_FIN,
            .status = PROTOCOL_STATUS_EXITO,
            .request_id = transfer->request_id,
            .payload_length = 0
        };

        if (protocol_send_message(
                fd_cliente, &cabecera, NULL
            ) != PROTOCOL_OK) {
            return VOD_ERROR_RED;
        }

        transfer->fin_enviado = true;
        return VOD_FINALIZADO;
    }

    // Buffer temporal para leer el archivo.
    uint8_t buffer[VOD_BLOCK_SIZE];

    // Calcular cuantos bytes faltan.
    uint64_t restantes =
        transfer->tamano - transfer->enviados;

    // No leer mas de lo que permite el buffer.
    size_t cantidad = VOD_BLOCK_SIZE;

    if (restantes < cantidad) {
        cantidad = (size_t)restantes;
    }

    // Leer un bloque del archivo.
    ssize_t leidos;

    do {
        leidos = read(
            transfer->fd_archivo,
            buffer,
            cantidad
        );
    } while (leidos < 0 && errno == EINTR);

    // Error de lectura o EOF prematuro.
    if (leidos <= 0) {
        return VOD_ERROR_LECTURA;
    }

    // Preparar el mensaje DATOS.
    protocol_header_t cabecera = {
        .version = PROTOCOL_VERSION,
        .type = PROTOCOL_MSG_DATOS,
        .status = PROTOCOL_STATUS_EXITO,
        .request_id = transfer->request_id,
        .payload_length = (uint32_t)leidos
    };

    // Enviar el bloque utilizando nuestro protocolo.
    if (protocol_send_message(
            fd_cliente,
            &cabecera,
            buffer
        ) != PROTOCOL_OK) {
        return VOD_ERROR_RED;
    }

    // Actualizar los bytes enviados.
    transfer->enviados += (uint64_t)leidos;

    return VOD_BLOQUE_ENVIADO;
}

