//Esta línea es para poder usar lo de CLOCK_MONOTONIC y eso.
#define _POSIX_C_SOURCE 200809L

#include "timeline.h"

#include <time.h>

//El reloj divide el tiempo en tv_sec (son segundos, que multiplica x1000) y el tv_nsec (son ns que divide /1000000), eso lo suma y obtiene el tiempo en ms.
uint64_t timeline_ahora_ms(void) {
    struct timespec timespec;
    clock_gettime(CLOCK_MONOTONIC, &timespec);
    return (uint64_t)timespec.tv_sec * 1000u + (uint64_t)timespec.tv_nsec / 1000000u;
}

//Da inicio al timeline con base al instante t0_ms que se le indique, tomando en cuenta la duración del ciclo de videos.
timeline_result_t timeline_iniciar_en(
    timeline_t *t, 
    uint64_t t0_ms,
    uint64_t ciclo_ms
) { //La idea es que guarde el inicio de cuándo arrancó el canal y la duración del ciclo para que todos los que entren vean la misma posición.
    if (t == NULL || ciclo_ms == 0) return TIMELINE_INVALID;
    t->t0_ms = t0_ms;
    t->ciclo_ms = ciclo_ms;
    return TIMELINE_OK;
}

//Simplemente llama a la anterior pasandole el instante actual como t0_ms. Así da inicio al timeline.
timeline_result_t timeline_iniciar(
    timeline_t *t,
    uint64_t ciclo_ms
) {
    return timeline_iniciar_en(t, timeline_ahora_ms(), ciclo_ms);
}

//Da la posición en el ciclo en el instante ahora_ms.
timeline_result_t timeline_posicion_en(
    const timeline_t *t,
    uint64_t ahora_ms,
    uint64_t *pos_ms
) {
    if (t == NULL || pos_ms == NULL || t->ciclo_ms == 0) return TIMELINE_INVALID;
    if (ahora_ms < t->t0_ms) return TIMELINE_NO_INICIADO;
    // Aquí metí lo que el profe pide directamente en la especificación, lo de p(t) = (t - t0) mod T
    uint64_t transcurrido = ahora_ms - t->t0_ms;
    *pos_ms = transcurrido % t->ciclo_ms;
    return TIMELINE_OK;
}

//Llama a la anterior pasándole el instante actual como ahora_ms. Así consulta la posición actual dentro del ciclo de videos.
timeline_result_t timeline_posicion_actual(
    const timeline_t *t,
    uint64_t *pos_ms
) {
    return timeline_posicion_en(t, timeline_ahora_ms(), pos_ms);
}

//Suma la duración de todos los videos del ciclo más el tiempo de separación.
uint64_t timeline_ciclo_total(
    const uint64_t *duraciones_ms,
    size_t n,
    uint64_t separacion_ms
) {
    if (duraciones_ms == NULL || n == 0) return 0;
    uint64_t total = separacion_ms;
    for (size_t i = 0; i < n; i++) {
        total += duraciones_ms[i];
    }
    return total;
}

//Dada una ubicación en ms, busca en qué video del ciclo está, cuántos ms dentro de ese video, o bien si está en una separación.
timeline_result_t timeline_localizar(
    const uint64_t *duraciones_ms,
    size_t n,
    uint64_t separacion_ms,
    uint64_t pos_ms,
    timeline_ubicacion_t *out
) {
    if (duraciones_ms == NULL || out == NULL || n == 0) return TIMELINE_INVALID;
    uint64_t inicio = 0;
    for (size_t i = 0; i < n; i++) {
        //Si la posición actual es menor que inicio + duración del i-ésimo video entonces está dentro de este. Llena out con el índice del video y los ms que lleva.
        if (pos_ms < inicio + duraciones_ms[i]) {
            out->indice = i;
            out->offset_ms = pos_ms - inicio;
            out->en_separacion = 0;
            return TIMELINE_OK;
        }
        inicio += duraciones_ms[i]; //Cambia el inicio por el del siguiente video.
    }
    if (pos_ms < inicio + separacion_ms) { //Si queda por fuera de todos los videos y dentro del ciclo, entonces está en la separación del final.
        out->indice = n - 1;
        out->offset_ms = 0;
        out->en_separacion = 1;
        return TIMELINE_OK;
    }
    return TIMELINE_FUERA_DE_CICLO;
}