#define _POSIX_C_SOURCE 200809L

#include "servidor.h"
#include "protocol.h"
#include "vod.h"
#include "vod_control.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif

//Tamaño de cada bloque DATOS del canal (el máximo del protocolo es 65536).
#define CANAL_BLOQUE_BYTES 32768u
//Cuánto contenido (en ms) puede ir adelantado el envío respecto a la línea de tiempo. Da colchón al reproductor sin romper la sincronía (RNF-04 tolera 2 s).
#define CANAL_ADELANTO_MS 3000u //Le puse más tiempo porque en videos pesados se empieza a laguear.
//Pausa entre revisiones cuando no hay nada que enviar (también es lo que tarda en notar un SALIR).
#define CANAL_PAUSA_MS 10
//Si el cliente no consume nada en este tiempo se abandona la sesión, para no dejar el servidor colgado en FIFO.
#define CANAL_ENVIO_TIMEOUT_S 10

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
                                         estado_sesion_t *estado,
                                         const canal_t *canal) {
    uint8_t servicios = SERVICIO_VOD | (canal != NULL ? SERVICIO_LIVE : 0u);
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

//Arma y envía EMISION con la posición vigente.
static protocol_result_t enviar_emision(
    int fd_cliente, uint32_t id_solicitud,
    const canal_t *canal,
    const canal_ubicacion_t *u
) {
    uint8_t payload[16u + 2u + PROTOCOL_MAX_NAME];
    size_t escritos = 0;
    size_t largo = strlen(u->nombre);

    if (largo == 0 || largo > PROTOCOL_MAX_NAME ||
        protocol_encode_emission(canal_ciclo_ms(canal), u->posicion_ms,
                                 (const uint8_t *)u->nombre, (uint16_t)largo,
                                 payload, sizeof(payload), &escritos) != PROTOCOL_OK) {
        return enviar_error(fd_cliente, id_solicitud, PROTOCOL_STATUS_INTERNO,
                            "error interno del canal");
    }

    protocol_header_t cabecera;
    cabecera.version        = PROTOCOL_VERSION;
    cabecera.type           = PROTOCOL_MSG_EMISION;
    cabecera.status         = PROTOCOL_STATUS_EXITO;
    cabecera.request_id     = id_solicitud;
    cabecera.payload_length = (uint32_t)escritos;
    return protocol_send_message(fd_cliente, &cabecera, payload);
}

//Atiende un UNIR: envía EMISION y luego DATOS del canal hasta que el cliente mande SALIR o CERRAR, o el servidor deje de funcionar.
static protocol_result_t atender_canal(int fd_cliente, uint32_t id_unir,
                                        const canal_t *canal,
                                        const char *origen,
                                        estado_sesion_t *estado) {
    if (canal == NULL) {
        return enviar_error(fd_cliente, id_unir, PROTOCOL_STATUS_NO_SERVIBLE,
                            "el canal en vivo no esta disponible");
    }

    canal_ubicacion_t u;
    canal_result_t cr = canal_ubicar_ahora(canal, &u);
    if (cr != CANAL_OK) {
        fprintf(stderr, "[servidor] %s: no se pudo ubicar el canal: %s\n",
                origen, canal_result_str(cr));
        return enviar_error(fd_cliente, id_unir, PROTOCOL_STATUS_NO_SERVIBLE,
                            "el canal en vivo no esta disponible");
    }

    uint8_t *buf = malloc(CANAL_BLOQUE_BYTES);
    if (buf == NULL) {
        return enviar_error(fd_cliente, id_unir, PROTOCOL_STATUS_INTERNO,
                            "error interno del canal");
    }

    //Si el cliente deja de leer, send() termina con error en vez de bloquear para siempre.
    struct timeval tv = { CANAL_ENVIO_TIMEOUT_S, 0 };
    setsockopt(fd_cliente, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    protocol_result_t res = enviar_emision(fd_cliente, id_unir, canal, &u);
    if (res != PROTOCOL_OK) {
        free(buf);
        return res;
    }
    printf("[servidor] %s: se une al canal (posicion %llu ms de %llu ms%s)\n",
           origen, (unsigned long long)u.posicion_ms,
           (unsigned long long)canal_ciclo_ms(canal),
           u.en_separacion ? ", en separacion" : "");

    size_t cur = u.indice;
    uint64_t cursor = u.byte_offset;
    int terminado = u.en_separacion;
    int fin = 0;

    while (!fin) {
        if (servidor_detencion_solicitada()) {
            res = enviar_vacio(fd_cliente, PROTOCOL_MSG_DESPEDIDA, id_unir);
            *estado = SESION_CERRANDO;
            break;
        }

        int espera_ms = 0;
        canal_ubicacion_t n;
        if (canal_ubicar_ahora(canal, &n) != CANAL_OK) {
            res = enviar_error(fd_cliente, id_unir, PROTOCOL_STATUS_INTERNO,
                               "error interno del canal");
            break;
        }

        if (terminado) {
            uint64_t dur = canal_duracion_ms(canal, cur);
            if (!n.en_separacion && (n.indice != cur || n.offset_ms < dur / 2)) {
                cur = n.indice;
                cursor = (n.offset_ms < 250) ? 0 : n.byte_offset;
                terminado = 0;
            } else {
                espera_ms = CANAL_PAUSA_MS;
            }
        } else {
            //Control de ritmo: no se adelanta más de CANAL_ADELANTO_MS (o un cuarto del elemento) sobre la línea de tiempo. 
            if (!n.en_separacion && n.indice == cur) {
                uint64_t dur = canal_duracion_ms(canal, cur);
                uint64_t adelanto = CANAL_ADELANTO_MS < dur / 4 ? CANAL_ADELANTO_MS : dur / 4;
                uint64_t margen = (canal_tamano_bytes(canal, cur) / dur) * adelanto;
                if (cursor > n.byte_offset + margen) {
                    espera_ms = CANAL_PAUSA_MS;
                }
            }
            if (espera_ms == 0) {
                size_t leidos = 0;
                cr = canal_leer(canal, cur, cursor, buf, CANAL_BLOQUE_BYTES, &leidos);
                if (cr != CANAL_OK) {
                    //El contenido falló a mitad de emisión.
                    fprintf(stderr, "[servidor] %s: fallo leyendo el canal: %s\n",
                            origen, canal_result_str(cr));
                    res = enviar_error(fd_cliente, id_unir, PROTOCOL_STATUS_INTERNO,
                                       "error de lectura del contenido");
                    break;
                }
                if (leidos == 0) {
                    terminado = 1;
                } else {
                    protocol_header_t cab;
                    cab.version        = PROTOCOL_VERSION;
                    cab.type           = PROTOCOL_MSG_DATOS;
                    cab.status         = PROTOCOL_STATUS_EXITO;
                    cab.request_id     = id_unir;
                    cab.payload_length = (uint32_t)leidos;
                    res = protocol_send_message(fd_cliente, &cab, buf);
                    if (res != PROTOCOL_OK) {
                        break;
                    }
                    cursor += leidos;
                }
            }
        }

        //Entre bloque y bloque se revisa si el cliente mandó algo.
        struct pollfd pfd = { fd_cliente, POLLIN, 0 };
        int pr = poll(&pfd, 1, espera_ms);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            res = PROTOCOL_IO_ERROR;
            break;
        }
        if (pr == 0) {
            continue;
        }
        if (pfd.revents & POLLNVAL) {
            res = PROTOCOL_IO_ERROR;
            break;
        }

        protocol_message_t msg;
        protocol_result_t rr = protocol_recv_message(fd_cliente, &msg);
        if (rr != PROTOCOL_OK) {
            res = rr;
            break;
        }
        switch ((protocol_message_type_t)msg.header.type) {
            case PROTOCOL_MSG_SALIR:
                res = enviar_vacio(fd_cliente, PROTOCOL_MSG_ACEPTADO, msg.header.request_id);
                printf("[servidor] %s: sale del canal\n", origen);
                fin = 1;
                break;
            case PROTOCOL_MSG_CERRAR:
                res = enviar_vacio(fd_cliente, PROTOCOL_MSG_DESPEDIDA, msg.header.request_id);
                *estado = SESION_CERRANDO;
                fin = 1;
                break;
            default:
                //Cualquier otra solicitud durante una transferencia se rechaza.
                res = enviar_error(fd_cliente, msg.header.request_id,
                                   PROTOCOL_STATUS_NO_PERMITIDO,
                                   "hay una transferencia en curso");
                break;
        }
        protocol_message_free(&msg);
        if (res != PROTOCOL_OK) {
            break;
        }
    }

    free(buf);
    return res;
}

static protocol_result_t despachar_en_lista(int fd_cliente,
                                             const protocol_message_t *msg,
                                             estado_sesion_t *estado,
                                             const catalog_t *catalog,
                                             vod_transfer_t *transfer,
                                             const canal_t *canal,
                                             const char *origen) {
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
            return atender_canal(fd_cliente, id, canal, origen, estado);

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

void atender_cliente(
        int fd_cliente,
        const catalog_t *catalog,
        const canal_t *canal
        ) {
    char origen[64];;
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
                r_envio = manejar_saludo(fd_cliente, msg.header.request_id, &estado, canal);
            } else {
                r_envio = enviar_error(fd_cliente,
                                       msg.header.request_id,
                                       PROTOCOL_STATUS_NO_PERMITIDO,
                                       "debe saludar primero");
            }
        } else if (estado == SESION_LISTA) {
            r_envio = despachar_en_lista(fd_cliente, &msg, &estado,
                                         catalog, &transferencia, canal, origen);
        } else {
            protocol_message_free(&msg);
            break;
        }

        protocol_message_free(&msg);

        if (r_envio == PROTOCOL_EOF) {
            printf("[servidor] %s cerro la conexion\n", origen);
            break;
        }
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
