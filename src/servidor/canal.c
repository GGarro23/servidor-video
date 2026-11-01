//Para poder usar pread y lo de POSIX. Va antes de los includes.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "canal.h"
#include "protocol.h"
#include "timeline.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

//Duración máxima por elemento y por separación para en teoría evitar desbordes al calcular.
#define CANAL_DURACION_MAX_MS 0xFFFFFFFFull

//Máximo de elementos del canal.
#define CANAL_MAX_ELEMENTOS 64u

//Tamaño de un paquete de MPEG-TS.
#define TS_PAQUETE 188u

//Struct de cada elemento del canal.
typedef struct {
    char nombre[PROTOCOL_MAX_NAME + 1];
    int fd;
    uint64_t size_bytes;
    uint64_t duracion_ms;
} canal_elemento_t;

//Struct del canal. duraciones es una copia aparte porque timeline_localizar pide un arreglo seguido de uint64_t.
struct canal {
    timeline_t timeline;
    canal_elemento_t *elementos;
    uint64_t *duraciones;
    size_t cantidad;
    uint64_t separacion_ms;
    uint64_t ciclo_ms;
};

//Las siguientes funciones son las que dependen del formato del video (MPEG-TS).

//Máximo de paquetes que se revisan desde cada extremo buscando un PCR.
#define TS_BUSQUEDA_MAX 8192u

//La base del PCR es de 33 bits, por eso da la vuelta cada 2^33 ticks.
#define PCR_BASE_MODULO (1ull << 33)

//Lee el PCR de un paquete TS. Devuelve 1 si el paquete trae PCR, 0 si no.
static int ts_leer_pcr(const uint8_t *p, unsigned *pid, uint64_t *pcr_90khz) {
    if (p[0] != 0x47) {
        return 0;
    }
    unsigned afc = (p[3] >> 4) & 0x3;
    //El bit 1 de afc indica que hay campo de adaptación.
    if ((afc & 0x2) == 0 || p[4] < 7 || (p[5] & 0x10) == 0) {
        return 0;
    }
    *pid = (((unsigned)p[1] & 0x1F) << 8) | p[2];
    *pcr_90khz = ((uint64_t)p[6] << 25) | ((uint64_t)p[7] << 17) |
                 ((uint64_t)p[8] << 9)  | ((uint64_t)p[9] << 1) |
                 ((uint64_t)p[10] >> 7);
    return 1;
}

//Busca un PCR desde el inicio (hacia_atras = 0) o desde el final (hacia_atras = 1) del archivo. Devuelve 1 si lo encontró.
static int ts_buscar_pcr(int fd, uint64_t paquetes, int hacia_atras, int pid_fijo,
                         unsigned *pid, uint64_t *pcr, uint64_t *indice) {
    uint8_t pkt[TS_PAQUETE];
    uint64_t limite = paquetes < TS_BUSQUEDA_MAX ? paquetes : TS_BUSQUEDA_MAX;

    for (uint64_t k = 0; k < limite; k++) {
        uint64_t i = hacia_atras ? paquetes - 1 - k : k;
        ssize_t n;
        do {
            n = pread(fd, pkt, sizeof(pkt), (off_t)(i * TS_PAQUETE));
        } while (n < 0 && errno == EINTR);
        if (n != (ssize_t)sizeof(pkt)) {
            return 0;
        }
        unsigned p;
        uint64_t c;
        if (ts_leer_pcr(pkt, &p, &c) && (pid_fijo < 0 || (unsigned)pid_fijo == p)) {
            *pid = p;
            *pcr = c;
            *indice = i;
            return 1;
        }
    }
    return 0;
}

//Saca la duración de un TS: (último PCR - primer PCR), extendido por regla de tres a todo el archivo.
static canal_result_t canal_sondear_duracion_ms(
    int fd,
    uint64_t size_bytes,
    uint64_t *duracion_ms
) {
    //Un TS son paquetes completos de 188 bytes.
    if (size_bytes % TS_PAQUETE != 0 || size_bytes < 2u * TS_PAQUETE) {
        return CANAL_FORMATO_NO_SOPORTADO;
    }
    uint64_t paquetes = size_bytes / TS_PAQUETE;

    unsigned pid_ini = 0, pid_fin = 0;
    uint64_t pcr_ini = 0, pcr_fin = 0, idx_ini = 0, idx_fin = 0;
    if (!ts_buscar_pcr(fd, paquetes, 0, -1, &pid_ini, &pcr_ini, &idx_ini) ||
        !ts_buscar_pcr(fd, paquetes, 1, (int)pid_ini, &pid_fin, &pcr_fin, &idx_fin) ||
        idx_fin <= idx_ini) {
        return CANAL_FORMATO_NO_SOPORTADO;
    }

    uint64_t ticks = (pcr_fin + PCR_BASE_MODULO - pcr_ini) % PCR_BASE_MODULO;
    if (ticks == 0) {
        return CANAL_FORMATO_NO_SOPORTADO;
    }
    //Los ticks son de 90 kHz, entonces ms = ticks / 90.
    double entre_pcr_ms = (double)ticks / 90.0;
    double total_ms = entre_pcr_ms * (double)paquetes / (double)(idx_fin - idx_ini);
    if (total_ms < 1.0 || total_ms > (double)CANAL_DURACION_MAX_MS) {
        return CANAL_FORMATO_NO_SOPORTADO;
    }
    *duracion_ms = (uint64_t)(total_ms + 0.5);
    return CANAL_OK;
}

//Convierte los ms que lleva un elemento en el byte desde donde leer.
//PENDIENTE (formato): empezar en el fotograma clave anterior.
static uint64_t canal_offset_a_byte(
    const canal_elemento_t *e,
    uint64_t offset_ms
) {
    //La división se reparte para que size * offset no se desborde.
    uint64_t base = (e->size_bytes / e->duracion_ms) * offset_ms;
    uint64_t resto = ((e->size_bytes % e->duracion_ms) * offset_ms) / e->duracion_ms;
    uint64_t byte = base + resto;
    if (byte >= e->size_bytes) {
        byte = e->size_bytes - 1;
    }
    return byte - (byte % TS_PAQUETE);
}

//Saca el nombre del archivo de la ruta (/videos/a.ts queda a.ts).
static canal_result_t canal_nombre_de_ruta(
    const char *ruta,
    char *out,
    size_t cap
) {
    const char *barra = strrchr(ruta, '/');
    const char *base = barra ? barra + 1 : ruta;
    size_t largo = strlen(base);

    if (largo == 0 || largo >= cap) {
        return CANAL_INVALID;
    }
    memcpy(out, base, largo + 1);
    return CANAL_OK;
}

//Cierra los archivos de los primeros abiertos elementos.
static void canal_cerrar_elementos(
    canal_elemento_t *elementos,
    size_t abiertos
) {
    for (size_t i = 0; i < abiertos; i++) {
        if (elementos[i].fd >= 0) {
            close(elementos[i].fd);
        }
    }
}

//Crea el canal con el t0_ms que se le indique. Abre los archivos, saca duraciones y arranca la línea de tiempo.
canal_result_t canal_create_en(
    const canal_config_t *config,
    uint64_t t0_ms,
    canal_t **out
) {
    if (config == NULL || out == NULL ||
        config->elementos == NULL || config->cantidad == 0 ||
        config->cantidad > CANAL_MAX_ELEMENTOS ||
        config->separacion_ms > CANAL_DURACION_MAX_MS) {
        return CANAL_INVALID;
    }
    *out = NULL;

    canal_t *canal = calloc(1, sizeof(*canal));
    if (canal == NULL) {
        return CANAL_NO_MEMORY;
    }
    canal->elementos = calloc(config->cantidad, sizeof(*canal->elementos));
    canal->duraciones = calloc(config->cantidad, sizeof(*canal->duraciones));
    if (canal->elementos == NULL || canal->duraciones == NULL) {
        free(canal->elementos);
        free(canal->duraciones);
        free(canal);
        return CANAL_NO_MEMORY;
    }

    canal_result_t res = CANAL_OK;
    size_t abiertos = 0;

    for (size_t i = 0; i < config->cantidad; i++) {
        const canal_elemento_cfg_t *cfg = &config->elementos[i];
        canal_elemento_t *e = &canal->elementos[i];
        //Con fd en -1 la limpieza no intenta cerrar un archivo que no se abrió.
        e->fd = -1;
        abiertos = i + 1;

        if (cfg->ruta == NULL || cfg->duracion_ms > CANAL_DURACION_MAX_MS) {
            res = CANAL_INVALID;
            break;
        }
        res = canal_nombre_de_ruta(cfg->ruta, e->nombre, sizeof(e->nombre));
        if (res != CANAL_OK) {
            break;
        }

        e->fd = open(cfg->ruta, O_RDONLY | O_CLOEXEC);
        if (e->fd < 0) {
            res = CANAL_IO_ERROR;
            break;
        }

        //fstat sobre el archivo ya abierto: tiene que ser un archivo normal y no estar vacío.
        struct stat st;
        if (fstat(e->fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            res = CANAL_IO_ERROR;
            break;
        }
        e->size_bytes = (uint64_t)st.st_size;

        //Si la configuración no trae la duración se saca del archivo.
        if (cfg->duracion_ms > 0) {
            e->duracion_ms = cfg->duracion_ms;
        } else {
            res = canal_sondear_duracion_ms(e->fd, e->size_bytes, &e->duracion_ms);
            if (res != CANAL_OK) {
                break;
            }
            if (e->duracion_ms == 0 || e->duracion_ms > CANAL_DURACION_MAX_MS) {
                res = CANAL_INVALID;
                break;
            }
        }
        canal->duraciones[i] = e->duracion_ms;
    }

    //Si todo está bien se calcula el ciclo y arranca la línea de tiempo.
    if (res == CANAL_OK) {
        canal->cantidad = config->cantidad;
        canal->separacion_ms = config->separacion_ms;
        canal->ciclo_ms = timeline_ciclo_total(canal->duraciones,
                                               canal->cantidad,
                                               canal->separacion_ms);
        if (timeline_iniciar_en(&canal->timeline, t0_ms, canal->ciclo_ms) != TIMELINE_OK) {
            res = CANAL_INVALID;
        }
    }

    if (res != CANAL_OK) {
        canal_cerrar_elementos(canal->elementos, abiertos);
        free(canal->elementos);
        free(canal->duraciones);
        free(canal);
        return res;
    }

    *out = canal;
    return CANAL_OK;
}

//Llama a la anterior pasándole el instante actual como t0_ms.
canal_result_t canal_create(
    const canal_config_t *config,
    canal_t **out
) {
    return canal_create_en(config, timeline_ahora_ms(), out);
}

//Cierra los archivos y libera la memoria.
void canal_destroy(
    canal_t *canal
) {
    if (canal == NULL) {
        return;
    }
    canal_cerrar_elementos(canal->elementos, canal->cantidad);
    free(canal->elementos);
    free(canal->duraciones);
    free(canal);
}

//Devuelve cuántos elementos tiene el canal.
size_t canal_cantidad(
    const canal_t *canal
) {
    return canal ? canal->cantidad : 0;
}

//Devuelve el nombre del elemento, o NULL si no existe.
const char *canal_nombre(
    const canal_t *canal,
    size_t indice
) {
    if (canal == NULL || indice >= canal->cantidad) {
        return NULL;
    }
    return canal->elementos[indice].nombre;
}

//Devuelve la duración del elemento, o 0 si no existe.
uint64_t canal_duracion_ms(
    const canal_t *canal,
    size_t indice
) {
    if (canal == NULL || indice >= canal->cantidad) {
        return 0;
    }
    return canal->elementos[indice].duracion_ms;
}

//Devuelve el tamaño en bytes del elemento, o 0 si no existe.
uint64_t canal_tamano_bytes(
    const canal_t *canal,
    size_t indice
) {
    if (canal == NULL || indice >= canal->cantidad) {
        return 0;
    }
    return canal->elementos[indice].size_bytes;
}

//Texto para los mensajes de consola.
const char *canal_result_str(
    canal_result_t resultado
) {
    switch (resultado) {
        case CANAL_OK:                  return "ok";
        case CANAL_INVALID:             return "configuracion invalida";
        case CANAL_IO_ERROR:            return "error de lectura del contenido";
        case CANAL_NO_MEMORY:           return "sin memoria";
        case CANAL_NO_INICIADO:         return "el canal aun no inicia";
        case CANAL_FORMATO_NO_SOPORTADO:return "formato no soportado (se esperaba MPEG-TS)";
    }
    return "resultado desconocido";
}

//Devuelve la duración completa del ciclo.
uint64_t canal_ciclo_ms(
    const canal_t *canal
) {
    return canal ? canal->ciclo_ms : 0;
}

//Dice dónde está el canal en el instante ahora_ms: elemento, ms que lleva y byte desde donde leer.
canal_result_t canal_ubicar_en(
    const canal_t *canal,
    uint64_t ahora_ms,
    canal_ubicacion_t *out
) {
    if (canal == NULL || out == NULL) {
        return CANAL_INVALID;
    }

    //p(t) = (t - t0) mod T
    uint64_t posicion = 0;
    timeline_result_t tr = timeline_posicion_en(&canal->timeline, ahora_ms, &posicion);
    if (tr == TIMELINE_NO_INICIADO) {
        return CANAL_NO_INICIADO;
    }
    if (tr != TIMELINE_OK) {
        return CANAL_INVALID;
    }

    timeline_ubicacion_t u;
    if (timeline_localizar(canal->duraciones, canal->cantidad,
                           canal->separacion_ms, posicion, &u) != TIMELINE_OK) {
        return CANAL_INVALID;
    }

    const canal_elemento_t *e = &canal->elementos[u.indice];
    out->indice = u.indice;
    out->nombre = e->nombre;
    out->posicion_ms = posicion;
    out->en_separacion = u.en_separacion;
    out->offset_ms = u.offset_ms;
    //En la separación no hay byte que leer, y el índice y el nombre son los del último elemento.
    out->byte_offset = u.en_separacion ? 0 : canal_offset_a_byte(e, u.offset_ms);
    return CANAL_OK;
}

//Llama a la anterior pasándole el instante actual como ahora_ms.
canal_result_t canal_ubicar_ahora(
    const canal_t *canal,
    canal_ubicacion_t *out
) {
    return canal_ubicar_en(canal, timeline_ahora_ms(), out);
}

//Lee un pedazo del elemento con pread. Puede devolver menos de lo pedido, y leidos en 0 es que llegó al final.
canal_result_t canal_leer(
    const canal_t *canal,
    size_t indice,
    uint64_t offset,
    uint8_t *buf,
    size_t max,
    size_t *leidos
) {
    if (canal == NULL || buf == NULL || leidos == NULL || indice >= canal->cantidad) {
        return CANAL_INVALID;
    }
    *leidos = 0;

    const canal_elemento_t *e = &canal->elementos[indice];
    if (offset >= e->size_bytes || max == 0) {
        return CANAL_OK;
    }
    uint64_t restante = e->size_bytes - offset;
    if ((uint64_t)max > restante) {
        max = (size_t)restante;
    }

    ssize_t n;
    do {
        n = pread(e->fd, buf, max, (off_t)offset);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        return CANAL_IO_ERROR;
    }
    //Si devuelve 0 aunque ya se recortó al tamaño, el archivo se encogió después de abrirlo.
    if (n == 0) {
        return CANAL_IO_ERROR;
    }
    *leidos = (size_t)n;
    return CANAL_OK;
}