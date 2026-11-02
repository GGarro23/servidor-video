#!/usr/bin/env python3
"""Prueba TCP de negociacion de version del protocolo binario.

Levanta el servidor real en un puerto local disponible, sin depender de videos
ni de un servidor iniciado manualmente. Devuelve codigo != 0 si algo falla.
"""

import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SERVIDOR = ROOT / "servidor"
HEADER = struct.Struct("!BBHII")
TIMEOUT = 3.0


def puerto_libre():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def recibir_exacto(sock, cantidad):
    datos = bytearray()
    while len(datos) < cantidad:
        trozo = sock.recv(cantidad - len(datos))
        if not trozo:
            raise AssertionError(
                f"conexion cerrada tras {len(datos)}/{cantidad} bytes"
            )
        datos.extend(trozo)
    return bytes(datos)


def esperar_servidor(proc, puerto):
    limite = time.monotonic() + 4.0
    while time.monotonic() < limite:
        if proc.poll() is not None:
            raise AssertionError(
                f"servidor termino antes de escuchar (codigo {proc.returncode})"
            )
        try:
            with socket.create_connection(("127.0.0.1", puerto), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise AssertionError("servidor no inicio a tiempo")


def probar_error_version(puerto):
    with socket.create_connection(("127.0.0.1", puerto), timeout=TIMEOUT) as sock:
        sock.settimeout(TIMEOUT)
        sock.sendall(HEADER.pack(2, 1, 0, 7, 0))

        version, tipo, estado, req_id, longitud = HEADER.unpack(
            recibir_exacto(sock, HEADER.size)
        )
        assert (version, tipo, estado, req_id) == (1, 9, 1, 7), (
            "respuesta ERROR(VERSION) incorrecta: ",
            (version, tipo, estado, req_id),
        )
        assert 0 <= longitud <= 65536, "longitud de error invalida"
        if longitud:
            descripcion = recibir_exacto(sock, longitud)
            assert b"\0" not in descripcion, "descripcion ERROR contiene NUL"
        assert sock.recv(1) == b"", "servidor no cerro tras ERROR(VERSION)"
    print("OK: version incompatible -> ERROR(VERSION), ID 7 y cierre TCP")


def probar_cabecera_invalida(puerto, request_id, longitud, nombre):
    with socket.create_connection(("127.0.0.1", puerto), timeout=TIMEOUT) as sock:
        sock.settimeout(TIMEOUT)
        sock.sendall(HEADER.pack(2, 1, 0, request_id, longitud))
        assert sock.recv(1) == b"", (
            f"{nombre}: el servidor respondio a una cabecera invalida"
        )
    print(f"OK: {nombre} -> cierre sin respuesta")


def probar_version_valida(puerto):
    with socket.create_connection(("127.0.0.1", puerto), timeout=TIMEOUT) as sock:
        sock.settimeout(TIMEOUT)
        sock.sendall(HEADER.pack(1, 1, 0, 11, 0))
        version, tipo, estado, req_id, longitud = HEADER.unpack(
            recibir_exacto(sock, HEADER.size)
        )
        assert (version, tipo, estado, req_id, longitud) == (1, 2, 0, 11, 1), (
            "respuesta BIENVENIDA incorrecta: ",
            (version, tipo, estado, req_id, longitud),
        )
        servicios = recibir_exacto(sock, longitud)
        assert servicios[0] & 1, "BIENVENIDA no anuncia VOD"
    print("OK: version valida -> BIENVENIDA")


def main():
    if not SERVIDOR.is_file():
        raise AssertionError("no existe ./servidor: ejecute make servidor")

    with tempfile.TemporaryDirectory(prefix="protocolo-version-") as almacen:
        puerto = puerto_libre()
        proc = subprocess.Popen(
            [str(SERVIDOR), "-m", "FIFO", "-d", "127.0.0.1",
             "-p", str(puerto), "-a", almacen],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        try:
            esperar_servidor(proc, puerto)
            probar_error_version(puerto)
            probar_cabecera_invalida(puerto, 0, 0, "request_id cero")
            probar_cabecera_invalida(
                puerto, 7, 65537, "payload_length excesivo"
            )
            probar_version_valida(puerto)
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
    print("Todas las pruebas TCP de version pasaron.")


if __name__ == "__main__":
    main()
