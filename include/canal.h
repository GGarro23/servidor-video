#ifndef CANAL_H
#define CANAL_H

#include <stddef.h>
#include <stdint.h>

//Tipo opaco del canal.
typedef struct canal canal_t;

//Enum para los estados posibles.
typedef enum {
    CANAL_OK = 0,
    CANAL_INVALID,
    CANAL_IO_ERROR,
    CANAL_NO_MEMORY,
    CANAL_NO_INICIADO,
    CANAL_FORMATO_NO_SOPORTADO
} canal_result_t;

//Struct para cada elemento del contenido del canal que se le pasa a canal_create.
typedef struct {
    const char *ruta;
    uint64_t duracion_ms;
} canal_elemento_cfg_t;

//Struct para la función canal_create. Son uno o varios elementos en orden.
typedef struct {
    const canal_elemento_cfg_t *elementos;
    size_t cantidad;
    uint64_t separacion_ms;
} canal_config_t;

//Struct para la función canal_ubicar_en. Dice dónde está el canal.
typedef struct {
    size_t indice;
    const char *nombre;
    uint64_t posicion_ms;
    uint64_t offset_ms;
    uint64_t byte_offset;
    int en_separacion;
} canal_ubicacion_t;

//Funciones

//Crea el canal empezando con t0 = ahora, la línea de tiempo se fija al arrancar el servidor.
canal_result_t canal_create(
    const canal_config_t *config,
    canal_t **out);

canal_result_t canal_create_en(
    const canal_config_t *config,
    uint64_t t0_ms,
    canal_t **out);

//Cierra los archivos y libera la memoria del canal.
void canal_destroy(
    canal_t *canal);

//Devuelve cuántos elementos tiene el canal.
size_t canal_cantidad(
    const canal_t *canal);

//Devuelve el nombre lógico de un elemento, o NULL si el índice no existe.
const char *canal_nombre(
    const canal_t *canal,
    size_t indice);

//Devuelve la duración en ms de un elemento, o 0 si el índice no existe.
uint64_t canal_duracion_ms(
    const canal_t *canal,
    size_t indice);

//Devuelve el tamaño en bytes de un elemento, o 0 si el índice no existe. Sirve para que la sesión calcule a qué ritmo (bytes/ms) debe enviar.
uint64_t canal_tamano_bytes(
    const canal_t *canal,
    size_t indice);

//Devuelve la duración completa del ciclo.
uint64_t canal_ciclo_ms(
    const canal_t *canal);

//Texto corto para registrar un resultado en la consola.
const char *canal_result_str(
    canal_result_t resultado);

//Dice en qué elemento y en qué punto está el canal en el instante ahora_ms.
canal_result_t canal_ubicar_en(
    const canal_t *canal,
    uint64_t ahora_ms,
    canal_ubicacion_t *out);

//Llama a la anterior pasándole el instante actual como ahora_ms.
canal_result_t canal_ubicar_ahora(
    const canal_t *canal,
    canal_ubicacion_t *out);

//Lee hasta max bytes del elemento indice a partir de offset. Usa pread, así que es seguro desde varios hilos.
canal_result_t canal_leer(
    const canal_t *canal,
    size_t indice,
    uint64_t offset,
    uint8_t *buf,
    size_t max,
    size_t *leidos);

#endif