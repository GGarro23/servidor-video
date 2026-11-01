#include <assert.h>
#include <stdio.h>
#include "timeline.h"

int main(void) {
    timeline_t t;
    uint64_t p;
    timeline_ubicacion_t u;

    // RF-03: 5 min despues -> minuto 5 (video de 20 min)
    assert(timeline_iniciar_en(&t, 1000, 20*60*1000) == TIMELINE_OK);
    assert(timeline_posicion_en(&t, 1000+5*60*1000, &p) == TIMELINE_OK && p == 5*60*1000);

    // t == t0 -> 0 ; antes de t0 -> TIMELINE_NO_INICIADO
    assert(timeline_posicion_en(&t, 1000, &p) == TIMELINE_OK && p == 0);
    assert(timeline_posicion_en(&t, 999, &p) == TIMELINE_NO_INICIADO);

    // borde de ciclo y muchos ciclos
    assert(timeline_posicion_en(&t, 1000+20*60*1000, &p) == TIMELINE_OK && p == 0);
    assert(timeline_posicion_en(&t, 1000+20*60*1000-1, &p) == TIMELINE_OK && p == 20*60*1000-1);
    assert(timeline_posicion_en(&t, 1000+1000000ull*20*60*1000+42, &p) == TIMELINE_OK && p == 42);

    // invalidos
    assert(timeline_iniciar_en(&t, 0, 0) == TIMELINE_INVALID);
    assert(timeline_iniciar_en(NULL, 0, 5) == TIMELINE_INVALID);

    // compuesto: 3 archivos + separacion
    uint64_t d[3] = {1000, 2000, 500};
    assert(timeline_ciclo_total(d, 3, 300) == 3800);
    assert(timeline_ciclo_total(d, 0, 300) == 0);
    assert(timeline_localizar(d,3,300,0,&u)==TIMELINE_OK && u.indice==0 && u.offset_ms==0 && !u.en_separacion);
    assert(timeline_localizar(d,3,300,999,&u)==TIMELINE_OK && u.indice==0 && u.offset_ms==999);
    assert(timeline_localizar(d,3,300,1000,&u)==TIMELINE_OK && u.indice==1 && u.offset_ms==0);
    assert(timeline_localizar(d,3,300,3499,&u)==TIMELINE_OK && u.indice==2 && u.offset_ms==499);
    assert(timeline_localizar(d,3,300,3500,&u)==TIMELINE_OK && u.en_separacion==1);
    assert(timeline_localizar(d,3,300,3799,&u)==TIMELINE_OK && u.en_separacion==1);
    assert(timeline_localizar(d,3,300,3800,&u)==TIMELINE_FUERA_DE_CICLO);
    assert(timeline_localizar(d,0,300,0,&u)==TIMELINE_INVALID);

    // sin separacion
    assert(timeline_localizar(d,3,0,3500,&u)==TIMELINE_FUERA_DE_CICLO);
   
    // reloj monotono real 
    uint64_t a = timeline_ahora_ms(), b = timeline_ahora_ms(); assert(b >= a);
    assert(timeline_iniciar(&t, 1000)==TIMELINE_OK && timeline_posicion_actual(&t,&p)==TIMELINE_OK && p<1000);
    
    printf("Todas las pruebas de timeline pasaron correctamente.\n");
    return 0;
}