#!/usr/bin/env python3
"""Servidor simulado (mock) para probar el cliente (VOD y canal en vivo).

El servidor real todavia no implementa ABRIR/catalogo/UNIR, asi que este
script habla el protocolo v0.4 a mano y provoca, segun el NOMBRE pedido,
cada caso feliz y de falla que el cliente debe manejar. Mientras transmite
sigue leyendo mensajes de control (CANCELAR, SALIR, CERRAR), como exige la
seccion 4.5 del protocolo.

Uso: mock_servidor_vod.py <puerto> <directorio_de_trabajo> [despedida]
  - deja en <directorio>/orig.bin los bytes que sirve para "ok.bin"
  - registra en <directorio>/eventos.log los mensajes que recibe
  - con "despedida", el canal en vivo termina solo enviando DESPEDIDA
"""
import select, socket, struct, sys, threading, time, os

PORT, WORK = int(sys.argv[1]), sys.argv[2]
DESPEDIDA_EN_CANAL = len(sys.argv) > 3 and sys.argv[3] == "despedida"
LOCK = threading.Lock()

def log(s):
    with LOCK:
        open(os.path.join(WORK, "eventos.log"), "a").write(s + "\n")

def hdr(t, rid, plen, status=0):
    return struct.pack(">BBHII", 1, t, status, rid, plen)

def recv_exact(c, n):
    b = b""
    while len(b) < n:
        d = c.recv(n - len(b))
        if not d: raise EOFError
        b += d
    return b

def read_msg(c):
    v, t, s, rid, plen = struct.unpack(">BBHII", recv_exact(c, 12))
    p = recv_exact(c, plen) if plen else b""
    log(f"RECV tipo={t} id={rid}")
    return t, rid, p

DATA = os.urandom(200_000)          # > 3 DATOS de 65536
open(os.path.join(WORK, "orig.bin"), "wb").write(DATA)
CAT = {"ok.bin": DATA, "vacio.bin": b""}

def poll_control(c, rid_activo, ok_type, ok_reply):
    """Lee un mensaje de control pendiente (sin bloquear). Devuelve:
       'siguiente' si no hay nada o se ignoro, 'detener' si hay que dejar de
       transmitir (la respuesta ya fue enviada), 'cerrar' si CERRAR."""
    r, _, _ = select.select([c], [], [], 0)
    if not r:
        return "siguiente"
    t, rid, _p = read_msg(c)
    if t == 15 and rid == rid_activo:            # CANCELAR
        c.sendall(hdr(16, rid, 0)); return "detener"
    if t == 14 and ok_type == 14:                # SALIR
        c.sendall(hdr(6, rid, 0)); return "detener"
    if t == 10:                                  # CERRAR
        c.sendall(hdr(11, rid, 0)); return "cerrar"
    msg = b"transferencia en curso"
    c.sendall(hdr(9, rid, len(msg), 5) + msg)    # NO_PERMITIDO
    return "siguiente"

def serve(c):
    try:
        while True:
            t, rid, p = read_msg(c)
            if t == 1:   c.sendall(hdr(2, rid, 1) + b"\x03")      # VOD + LIVE
            elif t == 3: c.sendall(hdr(4, rid, 2) + b"\x00\x00")
            elif t == 10:
                c.sendall(hdr(11, rid, 0)); return
            elif t == 12:                                          # UNIR
                nm = b"canal-demo"
                c.sendall(hdr(13, rid, 18 + len(nm)) +
                          struct.pack(">QQH", 60000, 1234, len(nm)) + nm)
                n = 0
                while True:
                    c.sendall(hdr(7, rid, 4096) + b"c" * 4096); n += 1
                    time.sleep(0.02)
                    if DESPEDIDA_EN_CANAL and n >= 25:
                        c.sendall(hdr(11, rid, 0)); return
                    ctl = poll_control(c, rid, 14, None)
                    if ctl == "cerrar": return
                    if ctl == "detener": break
            elif t == 5:                                           # ABRIR
                n = struct.unpack(">H", p[:2])[0]; name = p[2:2+n].decode()
                accept = lambda size: c.sendall(hdr(6, rid, 9) + struct.pack(">QB", size, 1))
                if name in CAT:
                    d = CAT[name]; accept(len(d))
                    for i in range(0, len(d), 65536):
                        ch = d[i:i+65536]; m = hdr(7, rid, len(ch)) + ch
                        c.sendall(m[:5]); time.sleep(0.01); c.sendall(m[5:])
                    c.sendall(hdr(8, rid, 0))
                elif name == "prematuro.bin":
                    accept(1000); c.sendall(hdr(7, rid, 400) + b"x"*400); c.sendall(hdr(8, rid, 0))
                elif name == "exceso.bin":
                    accept(100); c.sendall(hdr(7, rid, 150) + b"x"*150); c.sendall(hdr(8, rid, 0))
                elif name == "corte.bin":
                    accept(100000); c.sendall(hdr(7, rid, 30000) + b"x"*30000); return
                elif name == "truncado.bin":
                    accept(100000); c.sendall(hdr(7, rid, 30000) + b"x"*100); return
                elif name == "errmedio.bin":
                    accept(100000); c.sendall(hdr(7, rid, 30000) + b"x"*30000)
                    msg = b"fallo de lectura \x1b[31mROJO"
                    c.sendall(hdr(9, rid, len(msg), 7) + msg)
                elif name == "idmal.bin":
                    accept(100); c.sendall(hdr(7, rid+1, 100) + b"x"*100); c.sendall(hdr(8, rid+1, 0))
                elif name == "colgado.bin":
                    accept(100000); c.sendall(hdr(7, rid, 1000) + b"x"*1000); time.sleep(60); return
                elif name == "nohallado.bin":
                    msg = b"no existe"; c.sendall(hdr(9, rid, len(msg), 3) + msg)
                elif name == "lleno.bin":
                    msg = b"capacidad"; c.sendall(hdr(9, rid, len(msg), 6) + msg)
                elif name == "grande.bin":   # ~8 MB, atiende CANCELAR/CERRAR entre bloques
                    accept(8*1024*1024)
                    ch = b"z"*65536
                    for _ in range(128):
                        c.sendall(hdr(7, rid, 65536) + ch); time.sleep(0.02)
                        ctl = poll_control(c, rid, 15, None)
                        if ctl == "cerrar": return
                        if ctl == "detener": break
                    else:
                        c.sendall(hdr(8, rid, 0))
    except (EOFError, OSError):
        pass
    finally:
        try: c.close()
        except OSError: pass

s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", PORT)); s.listen(8)
while True:
    c, _ = s.accept(); threading.Thread(target=serve, args=(c,), daemon=True).start()
