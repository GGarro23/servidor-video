/* cliente_red.h - Conexión, señales y helpers de sesión del cliente */
#ifndef CLIENTE_RED_H
#define CLIENTE_RED_H

#include <stdint.h>
#include "protocol.h"

typedef enum {
    RECV_OK = 0,
    RECV_EOF,          /* el servidor cerró limpiamente         */
    RECV_TRUNCADO,     /* el servidor cerró a mitad de mensaje  */
    RECV_TIMEOUT,      /* se agotó el tiempo de espera          */
    RECV_INTERRUMPIDO, /* el usuario pulsó Ctrl+C               */
    RECV_PROTOCOLO,    /* el servidor envió algo inválido       */
    RECV_ERROR         /* error de sistema                      */
} recv_resultado_t;

/* Señales: SIGINT/SIGTERM ponen una bandera; SIGPIPE se ignora. */
void red_instalar_senales(void);
int  red_interrumpido(void);
void red_limpiar_interrupcion(void);

/* Conecta con plazo máximo (timeout_s, 0 = sin límite). Devuelve fd o -1. */
int red_conectar(const char *host, const char *puerto, int timeout_s, int verboso);

/* IDs de solicitud: nunca 0, siempre crecientes. */
uint32_t red_nuevo_id(void);

/* Envía un mensaje de control o de datos. */
protocol_result_t red_enviar(int fd, uint8_t tipo, uint32_t id,
                             const uint8_t *payload, uint32_t largo);

/* Recibe un mensaje. Si respetar_ctrl_c = 1, un Ctrl+C corta la espera. */
recv_resultado_t red_recibir(int fd, protocol_message_t *msg,
                             int timeout_s, int respetar_ctrl_c);

/* Traducción a texto / códigos de salida. */
const char *red_texto_recv(recv_resultado_t r);
int         red_codigo_salida_recv(recv_resultado_t r);
int         red_codigo_salida_error(uint16_t status);

/* Muestra por stderr el ERROR del servidor y devuelve el código de salida. */
int red_reportar_error(const protocol_message_t *msg);

/* SALUDO -> BIENVENIDA. Devuelve EXITO o un código de terminación. */
int red_saludar(int fd, int timeout_s, uint8_t *servicios);

/* CERRAR -> DESPEDIDA (mejor esfuerzo) y close(fd). */
void red_cerrar_sesion(int fd, int verboso);

#endif