/* cliente_ops.h - Operaciones del cliente: listar, descargar y canal en vivo */
#ifndef CLIENTE_OPS_H
#define CLIENTE_OPS_H

#include <stdint.h>
#include "cliente_args.h"

/* Todas devuelven un código de terminación (EXITO, ERR_*, EXIT_INTERRUMPIDO).
 * No cierran la sesión: eso lo hace main con red_cerrar_sesion(). */
int op_listar(int fd, const cliente_config_t *cfg);
int op_descargar(int fd, const cliente_config_t *cfg);
int op_canal(int fd, const cliente_config_t *cfg, char **argv_reproductor);

#endif