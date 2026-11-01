#!/usr/bin/env python3
"""Prueba de integración CERRAR durante VOD para el protocolo v1 del proyecto."""
import socket
import struct
import sys

HOST = "127.0.0.1"
PORT = 8080
ARCHIVO = "cancelacion.mp4"
CABECERA = "!BBHII"  # version, tipo, status, request_id, longitud


def leer_exacto(sock, cantidad):
    datos = bytearray()
    while len(datos) < cantidad:
        bloque = sock.recv(cantidad - len(datos))
        if not bloque:
            raise RuntimeError("El servidor cerró TCP antes de completar un mensaje")
        datos.extend(bloque)
    return bytes(datos)


def enviar(sock, tipo, identificador, payload=b""):
    sock.sendall(struct.pack(CABECERA, 1, tipo, 0, identificador, len(payload)) + payload)


def recibir(sock):
    version, tipo, estado, identificador, longitud = struct.unpack(
        CABECERA, leer_exacto(sock, 12)
    )
    if version != 1 or longitud > 65536:
        raise RuntimeError("Cabecera inválida recibida")
    return tipo, estado, identificador, leer_exacto(sock, longitud)


def esperar(tipo_esperado, id_esperado, respuesta):
    tipo, estado, identificador, payload = respuesta
    if (tipo, estado, identificador) != (tipo_esperado, 0, id_esperado):
        raise RuntimeError(
            f"Respuesta inesperada: tipo={tipo}, estado={estado}, id={identificador}, "
            f"mensaje={payload[:100]!r}"
        )
    return payload


def probar():
    with socket.create_connection((HOST, PORT), timeout=10) as sock:
        sock.settimeout(10)
        enviar(sock, 1, 1)  # SALUDO
        esperar(2, 1, recibir(sock))  # BIENVENIDA

        nombre = ARCHIVO.encode("utf-8")
        enviar(sock, 5, 2, struct.pack("!H", len(nombre)) + nombre)  # ABRIR
        info = esperar(6, 2, recibir(sock))  # ACEPTADO
        if len(info) != 9 or info[8] != 1:
            raise RuntimeError("Metadatos ACEPTADO incorrectos")
        tamano = struct.unpack("!Q", info[:8])[0]
        print(f"ACEPTADO: {ARCHIVO} ({tamano} bytes)", flush=True)

        # Enviamos CERRAR inmediatamente, sin consumir primero el video.
        # Esto impide que una descarga local extremadamente rápida termine
        # antes de la prueba, porque el buffer TCP es limitado.
        enviar(sock, 10, 3)  # CERRAR con ID nuevo
        print("CERRAR enviado durante VOD; esperando DESPEDIDA...", flush=True)

        bytes_recibidos = 0
        bloques = 0
        while True:
            tipo, estado, identificador, payload = recibir(sock)
            if tipo == 7:  # DATOS ya emitidos antes de procesar CERRAR
                if estado != 0 or identificador != 2:
                    raise RuntimeError("DATOS con estado o ID incorrecto")
                bytes_recibidos += len(payload)
                bloques += 1
                if bytes_recibidos > tamano:
                    raise RuntimeError("Llegaron más bytes que los anunciados")
            elif tipo == 11:  # DESPEDIDA
                esperar(11, 3, (tipo, estado, identificador, payload))
                if payload:
                    raise RuntimeError("DESPEDIDA no debe tener payload")
                break
            elif tipo == 8:
                raise RuntimeError("Llegó FIN: la descarga terminó antes del cierre")
            else:
                raise RuntimeError(f"Mensaje inesperado durante VOD: {tipo} (estado={estado})")

        if bytes_recibidos >= tamano:
            raise RuntimeError("No se interrumpió la transferencia antes del final")
        if sock.recv(1) != b"":
            raise RuntimeError("El servidor transmitió bytes después de DESPEDIDA")

        print(f"OK: DESPEDIDA recibida, id=3, {bloques} bloques previos, "
              f"{bytes_recibidos}/{tamano} bytes")
        print("OK: conexión TCP cerrada por el servidor y sin FIN posterior")


if __name__ == "__main__":
    try:
        probar()
    except (OSError, RuntimeError, struct.error) as exc:
        print(f"FALLO: {exc}", file=sys.stderr)
        sys.exit(1)
