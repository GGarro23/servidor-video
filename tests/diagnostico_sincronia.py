import argparse
import os
import socket
import statistics
import struct
import sys
import threading
import time

SALUDO, BIENVENIDA, DATOS, ERROR, CERRAR, DESPEDIDA = 1, 2, 7, 9, 10, 11
ACEPTADO, UNIR, EMISION, SALIR = 6, 12, 13, 14
SERVICIO_LIVE = 0x02

TS = 188
PCR_MOD = 1 << 33            # el PCR (base de 90 kHz) da la vuelta cada 2^33
SALTO_MAX_TICKS = 5 * 90000  # > 5 s entre PCR consecutivos: cambio de elemento
BUSQUEDA_MAX = 8192


def pcr_de_paquete(p):
    """(pid, pcr_90khz) si el paquete trae PCR; si no, None."""
    if p[0] != 0x47:
        return None
    afc = (p[3] >> 4) & 3
    if not (afc & 2) or p[4] < 7 or not (p[5] & 0x10):
        return None
    pid = ((p[1] & 0x1F) << 8) | p[2]
    pcr = (p[6] << 25) | (p[7] << 17) | (p[8] << 9) | (p[9] << 1) | (p[10] >> 7)
    return pid, pcr


class Elemento:

    def __init__(self, ruta):
        self.ruta = ruta
        self.nombre = os.path.basename(ruta)
        tam = os.path.getsize(ruta)
        if tam % TS or tam < 2 * TS:
            raise ValueError("%s no es un MPEG-TS de paquetes completos" % ruta)
        n = tam // TS
        with open(ruta, "rb") as f:
            ini = fin = None
            for i in range(min(n, BUSQUEDA_MAX)):
                f.seek(i * TS)
                r = pcr_de_paquete(f.read(TS))
                if r:
                    ini = (i, r[0], r[1])
                    break
            if ini is None:
                raise ValueError("%s: no se hallo PCR al inicio" % ruta)
            for k in range(min(n, BUSQUEDA_MAX)):
                i = n - 1 - k
                f.seek(i * TS)
                r = pcr_de_paquete(f.read(TS))
                if r and r[0] == ini[1]:
                    fin = (i, r[0], r[1])
                    break
            if fin is None or fin[0] <= ini[0]:
                raise ValueError("%s: no se hallo PCR al final" % ruta)
        self.pid = ini[1]
        self.pcr0 = ini[2]
        ticks = (fin[2] + PCR_MOD - ini[2]) % PCR_MOD
        self.dur_ms = int(ticks / 90.0 * n / (fin[0] - ini[0]) + 0.5)

    def media_ms(self, pcr):
        """Instante (ms desde el inicio del archivo) de un PCR."""
        return ((pcr - self.pcr0) % PCR_MOD) / 90.0


def envolver(x, ciclo):
    """Lleva x al intervalo (-ciclo/2, ciclo/2]."""
    r = (x + ciclo / 2.0) % ciclo - ciclo / 2.0
    return r


class Cierre(Exception):
    pass


def recibir_exacto(s, n):
    b = bytearray()
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d:
            raise Cierre("el servidor cerro la conexion")
        b += d
    return bytes(b)


def recibir_msg(s):
    v, tipo, estado, rid, largo = struct.unpack(">BBHII", recibir_exacto(s, 12))
    carga = recibir_exacto(s, largo) if largo else b""
    return tipo, estado, rid, carga, time.monotonic()


def enviar_msg(s, tipo, rid, carga=b""):
    s.sendall(struct.pack(">BBHII", 1, tipo, 0, rid, len(carga)) + carga)


class Espectador:
    def __init__(self, idx, args, elementos):
        self.idx = idx
        self.args = args
        self.elementos = elementos
        self.error = None
        self.servidor_se_despidio = False
        # EMISION
        self.t_emision = self.pos = self.ciclo = self.nombre = None
        # flujo: (t_llegada, bytes, u_antes, u_despues) por mensaje DATOS
        self.bloques = []
        # seguimiento del PCR -> posicion "desenrollada" en la linea de tiempo
        self._resto = b""
        self._pid = None
        self._i = None
        self._vueltas = 0
        self._ult_pcr = None
        self._m = 0.0
        self.u = None
        self.u_primero = None
        self.t_primero = None

    def _sigue(self, pcr, ciclo, inicios):
        e = self.elementos[self._i]
        if self._ult_pcr is None:
            self._m = e.media_ms(pcr)
        else:
            delta = (pcr - self._ult_pcr) % PCR_MOD
            cont = self._m + delta / 90.0
            if delta <= SALTO_MAX_TICKS and cont <= e.dur_ms + 1500:
                self._m = cont
            else:   # salto o reinicio: empieza el siguiente elemento del ciclo
                self._i = (self._i + 1) % len(self.elementos)
                if self._i == 0:
                    self._vueltas += 1
                self._m = self.elementos[self._i].media_ms(pcr)
        self._ult_pcr = pcr
        return self._vueltas * ciclo + inicios[self._i] + self._m

    def _procesa(self, datos, t, ciclo, inicios):
        buf = self._resto + datos
        # alineacion: el flujo es una secuencia continua de paquetes de 188 bytes
        k = 0
        while len(buf) - k >= TS:
            if buf[k] != 0x47:
                j = buf.find(b"\x47", k + 1)
                if j < 0:
                    k = len(buf)
                    break
                k = j
                continue
            r = pcr_de_paquete(buf[k:k + TS])
            if r:
                if self._pid is None:
                    self._pid = r[0]
                if r[0] == self._pid:
                    self.u = self._sigue(r[1], ciclo, inicios)
                    if self.u_primero is None:
                        self.u_primero = self.u
                        self.t_primero = t
            k += TS
        self._resto = buf[k:]

    def correr(self, t0_global, ciclo_fn):
        a = self.args
        try:
            time.sleep(max(0.0, t0_global + self.idx * a.desfase - time.monotonic()))
            s = socket.create_connection((a.host, a.port), timeout=10)
            s.settimeout(5)
            enviar_msg(s, SALUDO, 1)
            tipo, st, rid, carga, _ = recibir_msg(s)
            if tipo == ERROR:
                raise Cierre("ERROR en el saludo: %s" % carga.decode("utf-8", "replace"))
            if tipo != BIENVENIDA or not (carga[0] & SERVICIO_LIVE):
                raise Cierre("el servidor no ofrece canal en vivo")
            enviar_msg(s, UNIR, 2)
            tipo, st, rid, carga, t = recibir_msg(s)
            if tipo == ERROR:
                raise Cierre("ERROR al unirse: %s" % carga.decode("utf-8", "replace"))
            if tipo != EMISION or rid != 2:
                raise Cierre("se esperaba EMISION (llego tipo %d)" % tipo)
            self.ciclo, self.pos, nlen = struct.unpack(">QQH", carga[:18])
            self.nombre = carga[18:18 + nlen].decode("utf-8", "replace")
            self.t_emision = t
            ciclo, inicios = ciclo_fn(self.ciclo)
            nombres = [e.nombre for e in self.elementos]
            if self.nombre not in nombres:
                raise Cierre("EMISION anuncia '%s', que no esta en --canal %s" %
                             (self.nombre, nombres))
            self._i = nombres.index(self.nombre)

            fin = self.t_emision + a.duracion
            s.settimeout(1.0)
            while time.monotonic() < fin:
                try:
                    tipo, st, rid, carga, t = recibir_msg(s)
                except socket.timeout:
                    continue
                if tipo == DATOS and rid == 2:
                    antes = self.u
                    self._procesa(carga, t, ciclo, inicios)
                    self.bloques.append((t, len(carga), antes, self.u))
                elif tipo == DESPEDIDA:
                    self.servidor_se_despidio = True
                    break
                elif tipo == ERROR:
                    raise Cierre("ERROR durante el canal: %s" %
                                 carga.decode("utf-8", "replace"))
            # salida ordenada
            if not self.servidor_se_despidio:
                s.settimeout(5)
                enviar_msg(s, SALIR, 3)
                for _ in range(2000):
                    tipo, st, rid, carga, _t = recibir_msg(s)
                    if tipo == ACEPTADO and rid == 3:
                        break
                enviar_msg(s, CERRAR, 4)
                for _ in range(2000):
                    tipo, st, rid, carga, _t = recibir_msg(s)
                    if tipo == DESPEDIDA and rid == 4:
                        break
            s.close()
        except (Cierre, OSError, struct.error) as ex:
            self.error = "%s: %s" % (type(ex).__name__, ex)

class Veredicto:
    def __init__(self):
        self.lineas = []
        self.fallas = 0
        self.avisos = 0

    def marca(self, nivel, texto):
        if nivel == "FALLA":
            self.fallas += 1
        elif nivel == "AVISO":
            self.avisos += 1
        self.lineas.append("[%-5s] %s" % (nivel, texto))
        print(self.lineas[-1])


def nivel_abs(valor, ok, aviso):
    a = abs(valor)
    return "OK" if a <= ok else ("AVISO" if a <= aviso else "FALLA")


def regresion(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx


def analizar(esp, elementos, args):
    v = Veredicto()
    tol = float(args.tolerancia)
    ciclo = esp[0].ciclo
    n = len(esp)

    print("\n=== Espectadores ===")
    print("%-3s %-12s %10s %12s %12s %10s %8s" %
          ("#", "elemento", "pos EMISION", "1er dato (s)", "anclaje(ms)", "bloques", "MiB"))
    ref = esp[0]
    for e in esp:
        ancla = envolver(e.u_primero - (e.pos + (e.t_primero - e.t_emision) * 1000.0), ciclo)
        e.ancla = ancla
        print("%-3d %-12s %9.0fms %12.3f %+12.0f %10d %8.2f" %
              (e.idx + 1, e.nombre, e.pos, e.t_primero - ref.t_emision,
               ancla, len(e.bloques), sum(b[1] for b in e.bloques) / 1048576.0))

    print("\n=== 1. Reloj compartido (EMISION coherente entre espectadores) ===")
    peor = 0.0
    for i in range(n):
        for j in range(i + 1, n):
            dt = (esp[j].t_emision - esp[i].t_emision) * 1000.0
            err = envolver((esp[j].pos - esp[i].pos) - dt, ciclo)
            peor = max(peor, abs(err))
            print("  espectador %d -> %d: entraron con %7.0f ms de diferencia, "
                  "posiciones difieren %7.0f ms, error %+6.0f ms" %
                  (i + 1, j + 1, dt, esp[j].pos - esp[i].pos, err))
    v.marca(nivel_abs(peor, 300, tol),
            "reloj compartido: peor error entre pares = %.0f ms (OK <=300, AVISO <=%d)" % (peor, tol))

    print("\n=== 2. Anclaje (primer byte enviado vs posicion anunciada) ===")
    peor = max(abs(e.ancla) for e in esp)
    for e in esp:
        sentido = "adelantado" if e.ancla > 0 else "atrasado"
        print("  espectador %d: el flujo empieza %+0.0f ms (%s respecto a EMISION)" %
              (e.idx + 1, e.ancla, sentido))
    v.marca(nivel_abs(peor, 500, tol),
            "anclaje: peor desviacion = %.0f ms (OK <=500, AVISO <=%d)" % (peor, tol))
    if peor > 500:
        print("  Causa probable: canal_offset_a_byte() traduce posicion->byte con una regla "
              "de tres lineal (supone bitrate constante) y no alinea a fotograma clave.")

    print("\n=== 3. Desfase entre espectadores (reproduciendo desde su primer dato a 1x) ===")
    base = esp[0].u_primero - esp[0].t_primero * 1000.0
    ref_pos = esp[0].pos - esp[0].t_emision * 1000.0
    desf = []
    for e in esp:
        d = envolver((e.u_primero - e.t_primero * 1000.0) - base, ciclo)
        desf.append(d)
        print("  espectador %d: %+8.0f ms respecto al espectador 1" % (e.idx + 1, d))
    dispersion = max(desf) - min(desf)
    v.marca("OK" if dispersion <= tol / 2 else ("AVISO" if dispersion <= tol else "FALLA"),
            "desfase maximo entre espectadores = %.0f ms (RNF-04: <= %d ms)" % (dispersion, tol))

    print("\n=== 4. Ritmo del flujo (tiempo del video / tiempo real) ===")
    pend = []
    for e in esp:
        t_ini = e.t_primero + args.calentamiento
        xs = [b[0] for b in e.bloques if b[3] is not None and b[0] >= t_ini]
        ys = [b[3] for b in e.bloques if b[3] is not None and b[0] >= t_ini]
        m = regresion(xs, ys) if len(xs) >= 5 else None
        pend.append(m)
        print("  espectador %d: %s" % (e.idx + 1,
                                       "pendiente %.3f (%d puntos)" % (m, len(xs)) if m is not None
                                       else "muy pocos datos tras %.1f s de calentamiento" % args.calentamiento))
    validos = [m for m in pend if m is not None]
    if not validos:
        v.marca("AVISO", "ritmo: no hubo datos suficientes (aumente --duracion)")
    else:
        peor = max(validos, key=lambda m: abs(m - 1.0))
        nivel = "OK" if abs(peor - 1) <= 0.03 else ("AVISO" if abs(peor - 1) <= 0.06 else "FALLA")
        v.marca(nivel, "ritmo: pendiente mas alejada de 1.00 = %.3f (OK +-0.03, AVISO +-0.06)" % peor)

    print("\n=== 5. Colchon (flujo por delante de la linea de tiempo, antes de cada bloque) ===")
    peor_colchon = None
    for e in esp:
        # linea de tiempo ideal compartida (reloj del espectador 1)
        def ideal(t):
            return esp[0].pos + (t - esp[0].t_emision) * 1000.0
        # el ancla del primer PCR se compara en el mismo dominio que u
        # (u_primero esta dentro de [0, ciclo); se desenrolla con vueltas)
        corr = round((ideal(e.t_primero) - e.u_primero) / ciclo) * ciclo
        antes = [(b[0], b[2] + corr - ideal(b[0])) for b in e.bloques if b[2] is not None]
        despues = [b[3] + corr - ideal(b[0]) for b in e.bloques if b[3] is not None]
        if not antes:
            continue
        mi = min(antes, key=lambda x: x[1])
        e.colchon_min = mi[1]
        print("  espectador %d: colchon min %+7.0f ms (a los %.1f s), max adelanto %+7.0f ms" %
              (e.idx + 1, mi[1], mi[0] - e.t_emision, max(despues)))
        if peor_colchon is None or mi[1] < peor_colchon:
            peor_colchon = mi[1]
    if peor_colchon is None:
        v.marca("AVISO", "colchon: sin datos suficientes")
    else:
        nivel = "OK" if peor_colchon >= -150 else ("AVISO" if peor_colchon >= -500 else "FALLA")
        v.marca(nivel, "colchon minimo = %.0f ms (OK >=-150: al cambiar de elemento el servidor "
                       "empieza justo en la frontera; AVISO >=-500)" % peor_colchon)

    print("\n=== Huecos entre bloques de datos ===")
    for e in esp:
        ts = [b[0] for b in e.bloques]
        gaps = [(ts[k + 1] - ts[k], ts[k]) for k in range(len(ts) - 1)]
        if gaps:
            g, t = max(gaps)
            print("  espectador %d: hueco mas largo %.0f ms (a los %.1f s). "
                  "Es normal si el servidor ya iba adelantado (fin de elemento/separacion)." %
                  (e.idx + 1, g * 1000, t - e.t_emision))
    return v


def main():
    ap = argparse.ArgumentParser(description="Diagnostico de sincronia del canal en vivo.")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--canal", required=True,
                    help="los mismos .ts, en el mismo orden, que se pasaron al servidor con -c")
    ap.add_argument("--clientes", type=int, default=3)
    ap.add_argument("--duracion", type=float, default=25.0, help="segundos viendo el canal")
    ap.add_argument("--desfase", type=float, default=1.0, help="segundos entre la entrada de cada espectador")
    ap.add_argument("--tolerancia", type=float, default=2000.0, help="ms (RNF-04)")
    ap.add_argument("--calentamiento", type=float, default=4.0,
                    help="s iniciales que se omiten al medir el ritmo (rafaga inicial de adelanto)")
    ap.add_argument("--traza", help="archivo CSV con (espectador, t, bytes, posicion_video_ms)")
    args = ap.parse_args()

    try:
        elementos = [Elemento(r) for r in args.canal.split(",") if r]
    except (OSError, ValueError) as ex:
        print("Error leyendo --canal: %s" % ex, file=sys.stderr)
        return 2
    if args.clientes < 1:
        print("--clientes debe ser >= 1", file=sys.stderr)
        return 2

    def ciclo_fn(ciclo_emision):
        suma = sum(e.dur_ms for e in elementos)
        inicios, acum = [], 0
        for e in elementos:
            inicios.append(acum)
            acum += e.dur_ms
        return ciclo_emision, inicios

    esp = [Espectador(i, args, elementos) for i in range(args.clientes)]
    print("Canal: %s" % ", ".join("%s (%d ms)" % (e.nombre, e.dur_ms) for e in elementos))
    print("Espectadores: %d, desfase de entrada %.1f s, %.0f s de observacion cada uno" %
          (args.clientes, args.desfase, args.duracion))
    t0 = time.monotonic() + 0.3
    hilos = [threading.Thread(target=e.correr, args=(t0, ciclo_fn), daemon=True) for e in esp]
    for h in hilos:
        h.start()
    for h in hilos:
        h.join(args.duracion + args.clientes * args.desfase + 30)

    fallos = [e for e in esp if e.error]
    for e in fallos:
        print("Espectador %d fallo: %s" % (e.idx + 1, e.error), file=sys.stderr)
    if fallos or any(e.u_primero is None for e in esp):
        if not fallos:
            print("Algun espectador no recibio datos con reloj PCR (?el canal es MPEG-TS?).",
                  file=sys.stderr)
        return 2

    suma = sum(e.dur_ms for e in elementos)
    ciclo = esp[0].ciclo
    print("Ciclo anunciado por EMISION: %d ms = %d ms de contenido + %d ms de separacion" %
          (ciclo, suma, ciclo - suma))
    if ciclo < suma or abs(ciclo - suma) > 600000:
        print("AVISO: el ciclo no cuadra con los archivos de --canal; revise el orden/lista.",
              file=sys.stderr)

    if args.traza:
        with open(args.traza, "w") as f:
            f.write("espectador,t_s,bytes,posicion_video_ms\n")
            for e in esp:
                for (t, nb, _a, u) in e.bloques:
                    f.write("%d,%.4f,%d,%s\n" % (e.idx + 1, t - esp[0].t_emision, nb,
                                                 "" if u is None else "%.1f" % u))

    v = analizar(esp, elementos, args)
    print("\n=== Resumen: %d FALLA(S), %d AVISO(S) ===" % (v.fallas, v.avisos))
    return 1 if v.fallas else 0


if __name__ == "__main__":
    sys.exit(main())