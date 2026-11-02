cd "$(dirname "$0")/.." || exit 2

MODO=THREADED; N=3; DUR=25; DESFASE=1; SEP=2; TOL=2000
CANAL="videos/ciudad.ts,videos/flower.ts"
PUERTO=$((24000 + RANDOM % 2000)); VISUAL=0; TRAZA=""; REPRO=""

uso() { sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; }
while [ $# -gt 0 ]; do
    case "$1" in
        --visual) VISUAL=1 ;;
        -m|--modo) MODO=$2; shift ;;
        -n|--clientes) N=$2; shift ;;
        -d|--duracion) DUR=$2; shift ;;
        --desfase) DESFASE=$2; shift ;;
        -s|--separacion) SEP=$2; shift ;;
        --canal) CANAL=$2; shift ;;
        --tolerancia) TOL=$2; shift ;;
        --traza) TRAZA=$2; shift ;;
        -p|--puerto) PUERTO=$2; shift ;;
        -r|--reproductor) REPRO=$2; shift ;;
        -h|--help) uso; exit 0 ;;
        *) echo "Opcion desconocida: $1" >&2; uso >&2; exit 2 ;;
    esac
    shift
done

[ -x ./servidor ] && [ -x ./cliente ] || { echo "Falta compilar: ejecute 'make' primero." >&2; exit 2; }
command -v python3 >/dev/null || { echo "Se necesita python3." >&2; exit 2; }
if [ "$MODO" = FIFO ] && [ "$N" -gt 1 ]; then
    echo "Aviso: FIFO atiende de a un cliente; se usa 1 espectador." >&2; N=1
fi
IFS=',' read -ra ARCHIVOS <<< "$CANAL"
for f in "${ARCHIVOS[@]}"; do [ -f "$f" ] || { echo "No existe el archivo del canal: $f" >&2; exit 2; }; done

LOG=$(mktemp /tmp/diag_sincronia_srv.XXXXXX)
./servidor -m "$MODO" -p "$PUERTO" -a videos -c "$CANAL" -s "$SEP" -n $((N + 4)) >"$LOG" 2>&1 &
SP=$!
CLIENTES=()
limpiar() {
    for c in "${CLIENTES[@]}"; do kill "$c" 2>/dev/null; done
    kill -INT "$SP" 2>/dev/null
    for _ in $(seq 1 50); do kill -0 "$SP" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$SP" 2>/dev/null
    wait 2>/dev/null
}
trap limpiar EXIT

# Esperar a que el servidor escuche (maximo ~5 s).
LISTO=0
for _ in $(seq 1 50); do
    kill -0 "$SP" 2>/dev/null || break
    if (exec 3<>"/dev/tcp/127.0.0.1/$PUERTO") 2>/dev/null; then LISTO=1; break; fi
    sleep 0.1
done
if [ "$LISTO" != 1 ]; then
    echo "El servidor no arranco; su salida:" >&2; cat "$LOG" >&2; rm -f "$LOG"; exit 2
fi
echo "Servidor $MODO en el puerto $PUERTO | canal: $CANAL | separacion: ${SEP}s"

if [ "$VISUAL" = 1 ]; then
    if [ -z "$REPRO" ] && ! command -v mpv >/dev/null && ! command -v ffplay >/dev/null \
       && ! command -v cvlc >/dev/null && ! command -v vlc >/dev/null; then
        echo "No hay reproductor (mpv, ffplay, vlc). Instale uno o use -r; sin --visual la medicion funciona igual." >&2
        exit 2
    fi
    echo "Abriendo $N ventanas, una cada ${DESFASE}s. Deben mostrar la MISMA escena (tolerancia ${TOL} ms)."
    echo "Observe sobre todo: (1) que las escenas coincidan, (2) que no se congelen al cambiar de video"
    echo "o en la pausa entre ciclos. Se cierran solas a los ${DUR}s; Ctrl+C para cortar antes."
    for i in $(seq 1 "$N"); do
        if [ -n "$REPRO" ]; then
            ./cliente -d 127.0.0.1 -p "$PUERTO" -a canal -c "$DUR" -r "$REPRO" >/dev/null 2>&1 &
        else
            ./cliente -d 127.0.0.1 -p "$PUERTO" -a canal -c "$DUR" >/dev/null 2>&1 &
        fi
        CLIENTES+=($!)
        [ "$i" -lt "$N" ] && sleep "$DESFASE"
    done
    wait "${CLIENTES[@]}" 2>/dev/null
    exit 0
fi

ARGS=(--port "$PUERTO" --canal "$CANAL" --clientes "$N" --duracion "$DUR" --desfase "$DESFASE" --tolerancia "$TOL")
[ -n "$TRAZA" ] && ARGS+=(--traza "$TRAZA")
python3 tests/diagnostico_sincronia.py "${ARGS[@]}"
RC=$?
if [ "$RC" = 2 ]; then echo "--- salida del servidor:" >&2; tail -15 "$LOG" >&2; fi
rm -f "$LOG"
exit $RC