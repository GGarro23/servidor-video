#!/bin/bash
# Prueba rapida de concurrencia. Uso (desde la raiz del repo, ya compilado):
#   bash tests/test_concurrencia.sh            # los tres modos
#   bash tests/test_concurrencia.sh FORKED     # uno solo
cd "$(dirname "$0")/.." || exit 1
[ -x ./servidor ] && [ -x ./cliente ] || { echo "Compila primero con make"; exit 1; }

FALLOS=0
ok()   { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FALLOS=$((FALLOS+1)); }
check() { # check "descripcion" rc_obtenido rc_esperado
    [ "$2" = "$3" ] && ok "$1" || fail "$1 (rc=$2, esperado $3)"
}

probar() {
    local M=$1 P=$((20000 + RANDOM % 20000)) LOG=/tmp/conc_$1.log
    echo "== $M (puerto $P)"
    ./servidor -m $M -p $P -a videos -c videos/ciudad.ts -n 3 >"$LOG" 2>&1 &
    local S=$!
    sleep 1
    kill -0 $S 2>/dev/null || { fail "el servidor no arranco (ver $LOG)"; return; }

    local CAN=()
    for i in 1 2 3; do
        ./cliente -d 127.0.0.1 -p $P -a canal -c 6 -n -t 3 >/dev/null 2>&1 &
        CAN+=($!)
    done
    sleep 1.5

    ./cliente -d 127.0.0.1 -p $P -a listar -t 2 >/tmp/conc_l.txt 2>&1; local rc=$?
    if [ $M = FIFO ]; then
        check "FIFO: con el canal ocupado, otro cliente espera (timeout)" $rc 2
    else
        check "$M: 4to cliente rechazado por capacidad" $rc 4
        grep -q CAPACIDAD /tmp/conc_l.txt && ok "$M: mensaje CAPACIDAD visible" || fail "$M: falta mensaje CAPACIDAD"
    fi

    wait "${CAN[@]}" 2>/dev/null
    sleep 0.5

    ./cliente -d 127.0.0.1 -p $P -a listar -t 3 >/dev/null 2>&1
    check "$M: listar con cupo libre" $? 0
    rm -f /tmp/conc_d.mp4
    ./cliente -d 127.0.0.1 -p $P -a descargar -f ciudad.mp4 -o /tmp/conc_d.mp4 -t 5 >/dev/null 2>&1
    check "$M: descarga termina bien" $? 0
    cmp -s /tmp/conc_d.mp4 videos/ciudad.mp4 && ok "$M: descarga identica al original" || fail "$M: descarga distinta"

    if [ $M != FIFO ]; then
        # canal + descarga a la vez
        ./cliente -d 127.0.0.1 -p $P -a canal -c 4 -n -t 3 >/dev/null 2>&1 & local C=$!
        sleep 1
        ./cliente -d 127.0.0.1 -p $P -a descargar -f flower.mp4 -o /tmp/conc_f.mp4 -t 5 >/dev/null 2>&1
        check "$M: descarga mientras otro cliente esta en el canal" $? 0
        cmp -s /tmp/conc_f.mp4 videos/flower.mp4 && ok "$M: descarga simultanea identica" || fail "$M: descarga simultanea distinta"
        wait $C 2>/dev/null
    fi

    # cliente muerto de golpe y el servidor sigue
    ./cliente -d 127.0.0.1 -p $P -a canal -n >/dev/null 2>&1 & local K=$!
    sleep 1; kill -9 $K 2>/dev/null; wait $K 2>/dev/null; sleep 0.5
    ./cliente -d 127.0.0.1 -p $P -a listar -t 3 >/dev/null 2>&1
    check "$M: sigue vivo tras kill -9 de un cliente" $? 0

    # detencion con un cliente en el canal
    ./cliente -d 127.0.0.1 -p $P -a canal -n -t 3 >/tmp/conc_fin.txt 2>&1 & local F=$!
    sleep 1.5
    kill -INT $S; wait $S; check "$M: servidor termina con codigo 0 al Ctrl+C" $? 0
    wait $F 2>/dev/null
    grep -q "detencion ordenada" /tmp/conc_fin.txt && ok "$M: el cliente distingue la detencion de una caida" || fail "$M: el cliente no vio la detencion ordenada"
    [ -z "$(pgrep -f "servidor -m $M -p $P")" ] && ok "$M: sin procesos residuales" || fail "$M: quedaron procesos"
    ZOMB=$(ps -eo stat,cmd | awk '$1 ~ /^Z/ && /servidor/' | wc -l)
    [ "$ZOMB" = 0 ] && ok "$M: sin zombis" || fail "$M: $ZOMB zombis"
}

MODOS=${1:-"FIFO FORKED THREADED"}q
for m in $MODOS; do probar $m; done
echo
[ $FALLOS = 0 ] && echo "TODO OK" || echo "$FALLOS comprobacion(es) fallaron"
exit $((FALLOS>0))