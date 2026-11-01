#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "canal.h"

#define PKT 188u

//Escribe un TS sintetico: "paquetes" paquetes, con PCR cada 10 paquetes. Cada PCR avanza "paso_90k" ticks de 90 kHz. Devuelve la ruta (hay que hacer free) o NULL.
static char *crear_ts(size_t paquetes, uint64_t pcr_inicial_90k, uint64_t paso_90k) {
    char *ruta = strdup("/tmp/test_canal_XXXXXX");
    int fd = mkstemp(ruta);
    assert(fd >= 0);
    uint8_t pkt[PKT];
    uint64_t pcr = pcr_inicial_90k;
    for (size_t i = 0; i < paquetes; i++) {
        memset(pkt, 0xFF, sizeof pkt);
        pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0x00;     //PID 0x100
        if (i % 10 == 0) {
            uint64_t base = pcr & ((1ull << 33) - 1);
            pkt[3] = 0x20;                               //solo campo de adaptacion
            pkt[4] = 183; pkt[5] = 0x10;                 //flag PCR
            pkt[6] = (uint8_t)(base >> 25); pkt[7] = (uint8_t)(base >> 17);
            pkt[8] = (uint8_t)(base >> 9);  pkt[9] = (uint8_t)(base >> 1);
            pkt[10] = (uint8_t)(((base & 1) << 7) | 0x7E);
            pkt[11] = 0;
            pcr += paso_90k;
        } else {
            pkt[3] = 0x10;                               //solo payload
        }
        assert(write(fd, pkt, sizeof pkt) == (ssize_t)sizeof pkt);
    }
    close(fd);
    return ruta;
}

int main(void) {
    //Dos elementos de duracion declarada 10 s y 20 s, separacion 5 s -> ciclo 35 s.
    char *a = crear_ts(1000, 0, 9000);
    char *b = crear_ts(2000, 0, 9000);
    canal_elemento_cfg_t el[2] = { { a, 10000 }, { b, 20000 } };
    canal_config_t cfg = { el, 2, 5000 };
    canal_t *c = NULL;
    const uint64_t T0 = 1000000;
    assert(canal_create_en(&cfg, T0, &c) == CANAL_OK);
    assert(canal_cantidad(c) == 2);
    assert(canal_ciclo_ms(c) == 35000);
    assert(canal_tamano_bytes(c, 0) == 1000u * PKT);
    assert(canal_nombre(c, 2) == NULL && canal_duracion_ms(c, 2) == 0);

    canal_ubicacion_t u;

    //Antes de t0.
    assert(canal_ubicar_en(c, T0 - 1, &u) == CANAL_NO_INICIADO);

    //Exactamente t0: posicion 0, elemento 0, byte 0.

    assert(canal_ubicar_en(c, T0, &u) == CANAL_OK);
    assert(u.posicion_ms == 0 && u.indice == 0 && u.offset_ms == 0 && u.byte_offset == 0 && !u.en_separacion);

    //t0 + 5 min (RF-03): 300000 mod 35000 = 20000 -> elemento 1, 10 s dentro.
    assert(canal_ubicar_en(c, T0 + 300000, &u) == CANAL_OK);
    assert(u.posicion_ms == 20000 && u.indice == 1 && u.offset_ms == 10000);
    assert(u.byte_offset % PKT == 0 && u.byte_offset == (2000u * PKT / 2) - ((2000u * PKT / 2) % PKT));

    //Borde exacto entre elementos.
    assert(canal_ubicar_en(c, T0 + 10000, &u) == CANAL_OK && u.indice == 1 && u.offset_ms == 0);

    //Justo antes del borde: ultimo ms del elemento 0.
    assert(canal_ubicar_en(c, T0 + 9999, &u) == CANAL_OK && u.indice == 0 && u.offset_ms == 9999);

    //Separacion (30 s .. 35 s).
    assert(canal_ubicar_en(c, T0 + 31000, &u) == CANAL_OK && u.en_separacion && u.byte_offset == 0);

    //Borde de ciclo: 35000 -> vuelve a 0.
    assert(canal_ubicar_en(c, T0 + 35000, &u) == CANAL_OK && u.posicion_ms == 0 && u.indice == 0);
    
    //Muchos ciclos despues: sin acumulacion de error (RNF-05).
    uint64_t muchos = 1000000ull * 35000ull + 12345ull;
    assert(canal_ubicar_en(c, T0 + muchos, &u) == CANAL_OK && u.posicion_ms == 12345 && u.indice == 1 && u.offset_ms == 2345);
    
    //Dos clientes con 7 s de diferencia reciben posiciones separadas por 7 s (RF-43).
    canal_ubicacion_t u1, u2;
    assert(canal_ubicar_en(c, T0 + 3000, &u1) == CANAL_OK);
    assert(canal_ubicar_en(c, T0 + 10000, &u2) == CANAL_OK);
    assert(u2.posicion_ms - u1.posicion_ms == 7000);

    //Lectura: nunca pasa del final y devuelve 0 en el final.
    uint8_t buf[4096];
    size_t n = 99;
    assert(canal_leer(c, 0, 0, buf, sizeof buf, &n) == CANAL_OK && n > 0 && buf[0] == 0x47);
    assert(canal_leer(c, 0, 1000u * PKT, buf, sizeof buf, &n) == CANAL_OK && n == 0);
    assert(canal_leer(c, 0, 1000u * PKT - 10, buf, sizeof buf, &n) == CANAL_OK && n == 10);
    assert(canal_leer(c, 5, 0, buf, sizeof buf, &n) == CANAL_INVALID);
    canal_destroy(c);
    canal_destroy(NULL);

    //Sondeo de duracion por PCR: 1000 paquetes, PCR cada 10 -> 100 PCR, 99 intervalos de 9000 ticks (=100 ms) -> ~10 s.
    canal_elemento_cfg_t sonda[1] = { { a, 0 } };
    canal_config_t cfg2 = { sonda, 1, 0 };
    assert(canal_create_en(&cfg2, T0, &c) == CANAL_OK);
    uint64_t d = canal_duracion_ms(c, 0);
    assert(d >= 9900 && d <= 10100);
    canal_destroy(c);

    //Sondeo con wrap de 33 bits: el PCR inicial esta justo antes del limite y da la vuelta.
    char *w = crear_ts(1000, (1ull << 33) - 4 * 9000, 9000);
    canal_elemento_cfg_t sw[1] = { { w, 0 } };
    canal_config_t cfg3 = { sw, 1, 0 };
    assert(canal_create_en(&cfg3, T0, &c) == CANAL_OK);
    d = canal_duracion_ms(c, 0);
    assert(d >= 9900 && d <= 10100);
    canal_destroy(c);

    //Rutas de error: nada se filtra ni queda abierto.
    canal_elemento_cfg_t malos[1];
    canal_config_t cm = { malos, 1, 0 };
    malos[0].ruta = "/tmp/no_existe_canal.ts"; malos[0].duracion_ms = 1000;
    assert(canal_create_en(&cm, T0, &c) == CANAL_IO_ERROR && c == NULL);
    malos[0].ruta = "/tmp"; assert(canal_create_en(&cm, T0, &c) == CANAL_IO_ERROR && c == NULL);
    malos[0].ruta = "/dev/null"; assert(canal_create_en(&cm, T0, &c) == CANAL_IO_ERROR && c == NULL);
    malos[0].ruta = a; malos[0].duracion_ms = 0xFFFFFFFFull + 1;
    assert(canal_create_en(&cm, T0, &c) == CANAL_INVALID && c == NULL);
    cm.separacion_ms = 0xFFFFFFFFull + 1; malos[0].duracion_ms = 1000;
    assert(canal_create_en(&cm, T0, &c) == CANAL_INVALID && c == NULL);
    cm.separacion_ms = 0;
    assert(canal_create_en(NULL, T0, &c) == CANAL_INVALID);

    //Archivo que no es TS y sin duracion declarada: formato no soportado.
    char basura[] = "/tmp/test_canal_basura_XXXXXX";
    int fd = mkstemp(basura);
    uint8_t txt[PKT * 4];
    memset(txt, 'x', sizeof txt);
    assert(write(fd, txt, sizeof txt) == (ssize_t)sizeof txt);
    close(fd);
    malos[0].ruta = basura; malos[0].duracion_ms = 0;
    assert(canal_create_en(&cm, T0, &c) == CANAL_FORMATO_NO_SOPORTADO && c == NULL);

    unlink(a); unlink(b); unlink(w); unlink(basura);
    free(a); free(b); free(w);
    printf("Pruebas del canal superadas.\n");
    return 0;
}