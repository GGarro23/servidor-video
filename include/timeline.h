#ifndef TIMELINE_H
#define TIMELINE_H

#include <stddef.h>
#include <stdint.h>

//Enum para los estados posibles.
typedef enum {
    TIMELINE_OK = 0,
    TIMELINE_INVALID,
    TIMELINE_NO_INICIADO,
    TIMELINE_FUERA_DE_CICLO
} timeline_result_t;

//Struct para la función timeline_iniciar_en
typedef struct {
    uint64_t t0_ms;
    uint64_t ciclo_ms;
} timeline_t;

//Struct para la función de timeline_localizar
typedef struct{
    size_t indice;
    uint64_t offset_ms;
    int en_separacion;
} timeline_ubicacion_t;

//Funciones

//Esta simplemente lee el reloj y devuelve el tiempo en ms.
uint64_t timeline_ahora_ms(void);

//Crea la línea de tiempo empezando con t0 = ahora.
timeline_result_t timeline_iniciar(
    timeline_t *t,
    uint64_t ciclo_ms);

timeline_result_t timeline_iniciar_en(
    timeline_t *t,
    uint64_t t0_ms,
    uint64_t ciclo_ms);

//Calcula el ms en el que está la transmisión.
timeline_result_t timeline_posicion_en(
    const timeline_t *t,
    uint64_t ahora_ms,
    uint64_t *pos_ms);
 
timeline_result_t timeline_posicion_actual(
    const timeline_t *t,
    uint64_t *pos_ms);
 
//Saca la duración completa del ciclo de programación sumando lo que duran los videos más la pausa final.
uint64_t timeline_ciclo_total(
    const uint64_t *duraciones_ms,
    size_t n,
    uint64_t separacion_ms);
 
//Dada una posición de tiempo dentro del ciclo calcula en qué video está y cuánto tiempo en ms lleva ese video, o bien si está en una separación.
timeline_result_t timeline_localizar(
    const uint64_t *duraciones_ms,
    size_t n,
    uint64_t separacion_ms,
    uint64_t pos_ms,
    timeline_ubicacion_t *out);


#endif