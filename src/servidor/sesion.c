#define _POSIX_C_SOURCE 200809L

#include "servidor.h"
#include "protocol.h"
#include "vod.h"
#include "vod_control.h"

#include <stdlib.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif

typedef enum {
    SESION_INICIO,
    SESION_LISTA,
    SESION_VOD,
    SESION_CERRANDO
} estado_sesion_t;

static void describir_origen(int fd_cliente, char *buf, size_t capacidad) {
    struct sockaddr_storage direccion;
    socklen_t tam = sizeof(direccion);
    char host[NI_MAXHOST];
    char puerto[NI_MAXSERV];

    if (getpeername(fd_cliente, (struct sockaddr *)&direccion, &tam) != 0) {
        snprintf(buf, capacidad, "(origen desconocido)");
        return;
    }

    if (getnameinfo((struct sockaddr *)&direccion, tam,
                     host, sizeof(host), puerto, sizeof(puerto),
                     NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        snprintf(buf, capacidad, "(origen no resoluble)");
        return;
    }

    snprintf(buf, capacidad, "%s:%s", host, puerto);
}

static protocol_result_t enviar_error(int fd_cliente, uint32_t id_solicitud,
                                       protocol_status_t status,
                                       const char *texto) {
    protocol_header_t cabecera;
    size_t longitud = strlen(texto);

    cabecera.version         = PROTOCOL_VERSION;
    cabecera.type             = PROTOCOL_MSG_ERROR;
    cabecera.status           = (uint16_t)status;
    cabecera.request_id       = id_solicitud;
    cabecera.payload_length   = (uint32_t)longitud;

    return protocol_send_message(fd_cliente, &cabecera, (const uint8_t *)texto);
}

static protocol_result_t enviar_vacio(int fd_cliente, protocol_message_type_t tipo,
                                       uint32_t id_solicitud) {
    protocol_header_t cabecera;

    cabecera.version         = PROTOCOL_VERSION;
    cabecera.type             = (uint8_t)tipo;
    cabecera.status           = PROTOCOL_STATUS_EXITO;
    cabecera.request_id       = id_solicitud;
    cabecera.payload_length   = 0;

    return protocol_send_message(fd_cliente, &cabecera, NULL);
}

static protocol_result_t manejar_saludo(int fd_cliente, uint32_t id_solicitud,
                                         estado_sesion_t *estado) {
    uint8_t servicios = SERVICIO_VOD;
    protocol_header_t cabecera;

    cabecera.version       = PROTOCOL_VERSION;
    cabecera.type           = PROTOCOL_MSG_BIENVENIDA;
    cabecera.status         = PROTOCOL_STATUS_EXITO;
    cabecera.request_id     = id_solicitud;
    cabecera.payload_length = 1;

    protocol_result_t r = protocol_send_message(fd_cliente, &cabecera, &servicios);
    if (r == PROTOCOL_OK) {
        *estado = SESION_LISTA;
    }
    return r;
}



static protocol_result_t manejar_listar(
    int fd_cliente,
    uint32_t id_solicitud,
    const catalog_t *catalog
) {
    if (catalog == NULL) {
        return enviar_error(fd_cliente, id_solicitud,
                            PROTOCOL_STATUS_INTERNO,
                            "catalogo no disponible");
    }

    protocol_catalog_entry_t entradas[255];
    uint16_t cantidad = 0;
    size_t requerido = 2;  // u16 entry_count

    // Recorrer las entradas disponibles del catálogo.
    for (size_t i = 0; i < catalog_count(catalog); i++) {
        const catalog_entry_t *archivo = catalog_at(catalog, i);

        if (archivo == NULL || archivo->name == NULL) {
            return enviar_error(fd_cliente, id_solicitud,
                                PROTOCOL_STATUS_INTERNO,
                                "error interno del catalogo");
        }

        if (!archivo->available) {
            continue;
        }

        size_t longitud = strlen(archivo->name);

        if (longitud == 0 || longitud > PROTOCOL_MAX_NAME) {
            return enviar_error(fd_cliente, id_solicitud,
                                PROTOCOL_STATUS_INTERNO,
                                "entrada invalida del catalogo");
        }

        // Comprobar los límites del protocolo antes de codificar.
        if (cantidad == 255 ||
            requerido + 2u + longitud + 8u > PROTOCOL_MAX_PAYLOAD) {
            return enviar_error(fd_cliente, id_solicitud,
                                PROTOCOL_STATUS_CAPACIDAD,
                                "catalogo supera el limite permitido");
        }

        entradas[cantidad].name = (const uint8_t *)archivo->name;
        entradas[cantidad].name_length = (uint16_t)longitud;
        entradas[cantidad].file_size = archivo->size_bytes;

        cantidad++;
        requerido += 2u + longitud + 8u;
    }

    // Reservar memoria para el payload.
    uint8_t *payload = malloc(requerido);

    if (payload == NULL) {
        return enviar_error(fd_cliente, id_solicitud,
                            PROTOCOL_STATUS_INTERNO,
                            "no se pudo preparar el catalogo");
    }

    size_t escritos = 0;

    protocol_result_t resultado = protocol_encode_catalog(
        entradas, cantidad, payload, requerido, &escritos
    );

    if (resultado != PROTOCOL_OK) {
        free(payload);
        return enviar_error(fd_cliente, id_solicitud,
                            PROTOCOL_STATUS_INTERNO,
                            "error al codificar el catalogo");
    }

    // Preparar la cabecera CATALOGO.
    protocol_header_t cabecera;

    cabecera.version = PROTOCOL_VERSION;
    cabecera.type = PROTOCOL_MSG_CATALOGO;
    cabecera.status = PROTOCOL_STATUS_EXITO;
    cabecera.request_id = id_solicitud;
    cabecera.payload_length = (uint32_t)escritos;

    // Enviar el catálogo por TCP.
    resultado = protocol_send_message(
        fd_cliente, &cabecera, payload
    );

    free(payload);

    return resultado;
}

static protocol_result_t manejar_abrir(
    int fd_cliente,
    const protocol_message_t *msg,
    const catalog_t *catalog,
    vod_transfer_t *transfer,
    estado_sesion_t *estado
) {
    uint32_t id = msg->header.request_id;

    // Decodificar el nombre recibido del cliente.
    const uint8_t *nombre_bytes = NULL;
    uint16_t longitud = 0;

    if (protocol_decode_name(
            msg->payload,
            msg->header.payload_length,
            &nombre_bytes,
            &longitud
        ) != PROTOCOL_OK) {
        return enviar_error(fd_cliente, id,
                            PROTOCOL_STATUS_SOLICITUD,
                            "nombre de contenido invalido");
    }

    // Convertir los bytes recibidos a una cadena C.
    char nombre[PROTOCOL_MAX_NAME + 1];

    memcpy(nombre, nombre_bytes, longitud);
    nombre[longitud] = '\0';

    // Preparar la transferencia mediante el catálogo.
    catalog_result_t resultado = vod_preparar(
        catalog, nombre, id, transfer
    );

    if (resultado == CATALOG_NOT_FOUND) {
        return enviar_error(fd_cliente, id,
                            PROTOCOL_STATUS_NO_HALLADO,
                            "contenido no encontrado");
    }

    if (resultado == CATALOG_NOT_SERVABLE) {
        return enviar_error(fd_cliente, id,
                            PROTOCOL_STATUS_NO_SERVIBLE,
                            "contenido no disponible");
    }

    if (resultado != CATALOG_OK) {
        return enviar_error(fd_cliente, id,
                            PROTOCOL_STATUS_INTERNO,
                            "no se pudo preparar la transferencia");
    }

    // Confirmar al cliente que el archivo fue aceptado.
    protocol_result_t r = vod_enviar_aceptado(
        fd_cliente, transfer
    );

    if (r != PROTOCOL_OK) {
        vod_liberar(transfer);
        return r;
    }

    // Ahora tenemos una transferencia activa.
    *estado = SESION_VOD;

    return PROTOCOL_OK;
}

static protocol_result_t despachar_en_lista(int fd_cliente,
                                             const protocol_message_t *msg,
                                             estado_sesion_t *estado,
                                             const catalog_t *catalog,
                                             vod_transfer_t *transfer) {
    uint32_t id = msg->header.request_id;

    switch ((protocol_message_type_t)msg->header.type) {
        case PROTOCOL_MSG_LISTAR:
            return manejar_listar(fd_cliente, id, catalog);

        case PROTOCOL_MSG_ABRIR:
            return manejar_abrir(
                fd_cliente,
                msg,
                catalog,
                transfer,
                estado
            );

        case PROTOCOL_MSG_UNIR:
            return enviar_error(fd_cliente, id, PROTOCOL_STATUS_NO_SERVIBLE,
                                 "el canal en vivo no esta disponible todavia");

        case PROTOCOL_MSG_CERRAR:
            *estado = SESION_CERRANDO;
            return enviar_vacio(fd_cliente, PROTOCOL_MSG_DESPEDIDA, id);

        case PROTOCOL_MSG_SALUDO:
            return enviar_error(fd_cliente, id, PROTOCOL_STATUS_NO_PERMITIDO,
                                 "la sesion ya fue saludada");

        default:
            return enviar_error(fd_cliente, id, PROTOCOL_STATUS_NO_PERMITIDO,
                                 "operacion invalida en el estado actual");
    }
}

void atender_cliente(int fd_cliente, const catalog_t *catalog) {
    char origen[64];
    estado_sesion_t estado = SESION_INICIO;
    vod_transfer_t transferencia = {.fd_archivo = -1};

    describir_origen(fd_cliente, origen, sizeof(origen));
    printf("[servidor] conexion admitida: %s\n", origen);

    for (;;) {
        if (estado == SESION_VOD) {
            vod_control_result_t resultado = vod_control_servir(
                fd_cliente, &transferencia
            );

            /* Conservamos el descriptor del cliente; cerramos solo el archivo. */
            vod_liberar(&transferencia);

            if (resultado == VOD_CONTROL_COMPLETADO ||
                resultado == VOD_CONTROL_CANCELADO ||
                resultado == VOD_CONTROL_FALLO_LECTURA) {
                estado = SESION_LISTA;
                continue;
            }

            if (resultado == VOD_CONTROL_CERRAR) {
                printf("[servidor] %s: cierre durante VOD\n", origen);
                break;
            }

            fprintf(stderr,
                    "[servidor] %s: fallo de conexion durante VOD\n", origen);
            break;
        }

        protocol_message_t msg;
        protocol_result_t r = protocol_recv_message(fd_cliente, &msg);

        if (r == PROTOCOL_EOF) {
            printf("[servidor] %s cerro la conexion\n", origen);
            break;
        }
        if (r == PROTOCOL_TRUNCATED) {
            fprintf(stderr,
                    "[servidor] %s: mensaje truncado, se cierra la sesion\n",
                    origen);
            break;
        }
        if (r == PROTOCOL_IO_ERROR) {
            fprintf(stderr,
                    "[servidor] %s: error de red, se cierra la sesion\n",
                    origen);
            break;
        }
        if (r == PROTOCOL_NO_MEMORY) {
            fprintf(stderr,
                    "[servidor] %s: sin memoria para el mensaje recibido\n",
                    origen);
            break;
        }
        if (r == PROTOCOL_INVALID) {
            fprintf(stderr,
                    "[servidor] %s: mensaje malformado, se cierra la sesion\n",
                    origen);
            break;
        }

        protocol_result_t r_envio;
        if (estado == SESION_INICIO) {
            if (msg.header.type == PROTOCOL_MSG_SALUDO) {
                r_envio = manejar_saludo(fd_cliente,
                                         msg.header.request_id, &estado);
            } else {
                r_envio = enviar_error(fd_cliente,
                                       msg.header.request_id,
                                       PROTOCOL_STATUS_NO_PERMITIDO,
                                       "debe saludar primero");
            }
        } else if (estado == SESION_LISTA) {
            r_envio = despachar_en_lista(fd_cliente, &msg, &estado,
                                         catalog, &transferencia);
        } else {
            protocol_message_free(&msg);
            break;
        }

        protocol_message_free(&msg);

        if (r_envio != PROTOCOL_OK) {
            fprintf(stderr,
                    "[servidor] %s: fallo al responder, se cierra la sesion\n",
                    origen);
            break;
        }
        if (estado == SESION_CERRANDO) {
            printf("[servidor] %s: cierre ordenado\n", origen);
            break;
        }
    }

    vod_liberar(&transferencia);
    close(fd_cliente);
}
