#ifndef VOD_CONTROL_H
#define VOD_CONTROL_H

#include "vod.h"

typedef enum {
    VOD_CONTROL_COMPLETADO,
    VOD_CONTROL_CANCELADO,
    VOD_CONTROL_CERRAR,
    VOD_CONTROL_FALLO_LECTURA,
    VOD_CONTROL_FALLO_CONEXION
} vod_control_result_t;

/* Transmite DATOS/FIN sin bloquear el socket y procesa controles
   CANCELAR/CERRAR/solicitudes inválidas mientras VOD está activo.
   Requiere que ACEPTADO ya se haya enviado.
   NO cierra fd_cliente ni fd_archivo; el llamador limpia la transferencia. */
vod_control_result_t vod_control_servir(int fd_cliente, vod_transfer_t *transfer);

#endif
