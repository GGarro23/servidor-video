#!/usr/bin/env bash
# Pruebas del cliente (VOD y canal) contra un servidor simulado.
# Uso (desde la raiz del repo, tras `make`):  bash tests/test_cliente_vod.sh
cd "$(dirname "$0")/.." || exit 2

PUERTO=${PUERTO:-18190}
TMP=$(mktemp -d)
python3 tests/mock_servidor_vod.py "$PUERTO" "$TMP" & MOCK1=$!
python3 tests/mock_servidor_vod.py $((PUERTO+1)) "$TMP/b" despedida & MOCK2=$!
mkdir -p "$TMP/b"
trap 'kill $MOCK1 $MOCK2 2>/dev/null; rm -rf "$TMP"' EXIT
sleep 0.8

C="./cliente -d 127.0.0.1 -p $PUERTO"
FALLAS=0
ok()   { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; FALLAS=$((FALLAS+1)); }

# caso <nombre> <rc_esperado> <¿queda_archivo? si|no>
caso() {
    local nombre=$1 esperado=$2 queda=$3
    $C -a descargar -f "$nombre" -o "$TMP/$nombre.out" -n >/dev/null 2>&1
    local rc=$?
    local hay=no; ls "$TMP/$nombre.out"* >/dev/null 2>&1 && hay=si
    if [ "$rc" = "$esperado" ] && [ "$hay" = "$queda" ]; then ok "$nombre (rc=$rc, archivo=$hay)"
    else fail "$nombre: rc=$rc (esperado $esperado), archivo=$hay (esperado $queda)"; fi
}

echo "--- VOD"
caso ok.bin        0 si
caso vacio.bin     0 si
caso prematuro.bin 2 no   # FIN antes de completar
caso exceso.bin    2 no   # mas bytes que los anunciados
caso corte.bin     2 no   # desconexion entre mensajes
caso truncado.bin  2 no   # desconexion a mitad de un DATOS
caso idmal.bin     2 no   # request_id equivocado
caso errmedio.bin  5 no   # ERROR(INTERNO) tras ACEPTADO
caso nohallado.bin 3 no   # ERROR(NO_HALLADO)
caso lleno.bin     4 no   # ERROR(CAPACIDAD)

cmp -s "$TMP/orig.bin" "$TMP/ok.bin.out" && ok "ok.bin identico byte a byte" || fail "ok.bin difiere del original"
[ "$(stat -c %s "$TMP/vacio.bin.out")" = 0 ] && ok "vacio.bin mide 0 bytes" || fail "vacio.bin"

# Servidor colgado: el timeout (-t) corta la espera y no deja parcial.
$C -a descargar -f colgado.bin -o "$TMP/c.out" -n -t 2 >/dev/null 2>&1; rc=$?
{ [ "$rc" = 2 ] && ! ls "$TMP"/c.out* >/dev/null 2>&1; } && ok "timeout con servidor colgado (rc=2)" || fail "timeout: rc=$rc"

# Un destino previo no se pisa si la descarga falla.
echo ORIGINAL > "$TMP/previo.out"
$C -a descargar -f corte.bin -o "$TMP/previo.out" -n >/dev/null 2>&1
{ [ "$(cat "$TMP/previo.out")" = ORIGINAL ] && [ ! -e "$TMP/previo.out.part" ]; } \
    && ok "destino previo intacto tras fallo" || fail "destino previo alterado"

# Ctrl+C a mitad de una descarga: CANCELAR al servidor, rc=130, sin parcial.
: > "$TMP/eventos.log"
$C -a descargar -f grande.bin -o "$TMP/g.out" -n >/dev/null 2>&1 &
CP=$!; sleep 0.8; kill -INT $CP; wait $CP; rc=$?
{ [ "$rc" = 130 ] && ! ls "$TMP"/g.out* >/dev/null 2>&1 && grep -q "tipo=15" "$TMP/eventos.log"; } \
    && ok "Ctrl+C en VOD: envia CANCELAR, rc=130, sin parcial" || fail "Ctrl+C VOD: rc=$rc"

# Al terminar, el cliente cierra con CERRAR.
grep -q "tipo=10" "$TMP/eventos.log" && ok "el cliente cierra la sesion con CERRAR" || fail "no envio CERRAR"

echo "--- Canal en vivo"
: > "$TMP/eventos.log"
out=$($C -a canal -n -c 2 2>&1); rc=$?
{ [ "$rc" = 0 ] && echo "$out" | grep -q "Posicion de ingreso" && echo "$out" | grep -q "tiempo de canal cumplido" \
    && grep -q "tipo=14" "$TMP/eventos.log"; } \
    && ok "canal -c 2: EMISION, DATOS, SALIR y rc=0" || fail "canal -c 2: rc=$rc"

: > "$TMP/eventos.log"
$C -a canal -n >/dev/null 2>&1 & CP=$!; sleep 1; kill -INT $CP; wait $CP; rc=$?
{ [ "$rc" = 0 ] && grep -q "tipo=14" "$TMP/eventos.log"; } \
    && ok "Ctrl+C en canal: SALIR ordenado, rc=0" || fail "Ctrl+C canal: rc=$rc"

out=$(./cliente -d 127.0.0.1 -p $((PUERTO+1)) -a canal -n 2>&1); rc=$?
{ [ "$rc" = 0 ] && echo "$out" | grep -q "detencion ordenada"; } \
    && ok "servidor se detiene: DESPEDIDA distinguida de una caida" || fail "DESPEDIDA en canal: rc=$rc"

echo "---- fallas: $FALLAS"
exit $FALLAS
