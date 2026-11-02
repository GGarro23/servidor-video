# Servidor de Videos

Tercer proyecto, Lenguajes de Programación, Instituto Tecnológico de Costa Rica.

**Integrantes:**
- Gabriel Elias Garro Cruz
- Anyelina Hernandez Villalobos
- Diego Josue Rojas Castro
- Bryan Steven Castro Madrigal

**Fecha de entrega:** 28/10/26

Aplicación cliente-servidor en C (C11) para Linux, sobre sockets TCP. El servidor ofrece dos servicios:

- **Canal en vivo:** un contenido que se repite indefinidamente sobre una línea de tiempo compartida. Quien se conecta tarde recibe desde el punto vigente, no desde el inicio.
- **Video bajo demanda (VOD):** el cliente lista el catálogo y descarga un video completo por nombre.

El servidor puede iniciarse en tres modos de concurrencia: `FIFO`, `FORKED` y `THREADED`.

## Contenido

1. [Estructura del repositorio](#estructura-del-repositorio)
2. [Requisitos e instalación](#requisitos-e-instalación)
3. [Construcción](#construcción)
4. [Servidor](#servidor)
5. [Cliente](#cliente)
6. [Ejemplos por servicio y por modo](#ejemplos-por-servicio-y-por-modo)
7. [Reproductor de video](#reproductor-de-video)
8. [Pruebas](#pruebas)
9. [Limitaciones y notas](#limitaciones-y-notas)

## Estructura del repositorio

| Ruta | Contenido |
|---|---|
| `include/` | Archivos de cabecera. |
| `src/common/` | Protocolo (`protocol.c`), compartido por servidor y cliente. |
| `src/servidor/` | Servidor: sesión, catálogo, VOD, canal, línea de tiempo y modos de concurrencia. |
| `src/cliente/` | Cliente: argumentos, red, operaciones y lanzamiento del reproductor. |
| `docs/` | Documentación (protocolo, diseño, pruebas). |
| `tests/` | Pruebas unitarias y scripts de prueba. |
| `videos/` | Archivos multimedia de ejemplo. |

## Requisitos e instalación

- Linux de 64 bits (se usa la interfaz POSIX).
- `gcc` con soporte C11 y `make`.
- Un reproductor de video para el cliente, uno de: `mpv`, `ffplay` (paquete `ffmpeg`), `vlc` o `cvlc`. No es obligatorio si el cliente se usa con `-n`.

En Debian/Ubuntu:

```bash
sudo apt install build-essential ffmpeg
```

No se requieren bibliotecas externas ni servicios en ejecución.

## Construcción

Una sola orden produce los dos ejecutables (`servidor` y `cliente`) en la raíz del repositorio:

```bash
make
```

| Orden | Efecto |
|---|---|
| `make` | Producción (`-O2`, sin avisos). |
| `make debug` | Depuración (`-g -O0`, AddressSanitizer y UBSan). |
| `make test` | Compila y ejecuta las pruebas unitarias. |
| `make clean` | Elimina `build/`, `servidor` y `cliente`. |

## Servidor

```
./servidor -m <MODO> -p <PUERTO> -a <DIRECTORIO> [opciones]
```

| Opción | Valor | Obligatoria | Descripción |
|---|---|---|---|
| `-m`, `--modo` | `FIFO`, `FORKED` o `THREADED` | Sí | Modo de concurrencia. Rige toda la ejecución. |
| `-p`, `--puerto` | 1 a 65535 | Sí | Puerto de escucha. |
| `-a`, `--almacen` | Ruta de directorio | Sí | Directorio con los videos ofrecidos bajo demanda. |
| `-d`, `--direccion` | IPv4 o IPv6 | No | Interfaz de escucha. Por omisión, todas. |
| `-c`, `--canal` | Archivo o lista separada por comas (máx. 16) | No | Contenido del canal en vivo, en formato MPEG-TS (`.ts`). Sin esta opción solo se ofrece VOD. |
| `-s`, `--separacion` | Segundos, 0 a 86400 | No | Pausa entre el final de un ciclo y el inicio del siguiente. Por omisión, 0. |
| `-n`, `--max-clientes` | 1 a 1024 | No | Máximo de clientes activos en `FORKED` y `THREADED`. Por omisión, 64. En `FIFO` no aplica. |
| `-b`, `--backlog` | Entero positivo | No | Profundidad de la cola de admisión del sistema. Por omisión, 16. |
| `-h`, `--help` | | No | Muestra la ayuda y termina. |

### Códigos de terminación del servidor

| Código | Significado |
|---|---|
| 0 | Éxito (incluye la detención con Ctrl+C). |
| 1 | Error de argumentos. |
| 2 | Error de red (por ejemplo, puerto ocupado). |
| 3 | Error de contenido (catálogo no inicializable). |
| 4 | Capacidad. |
| 5 | Error interno. |

### Detención

`Ctrl+C` (SIGINT) o `SIGTERM` detienen el servidor de forma ordenada: los clientes del canal reciben un mensaje de despedida, se cierran las sesiones y se liberan los recursos.

### Catálogo

El catálogo se construye al arrancar con los archivos regulares del directorio indicado en `-a`. Se aceptan las extensiones `.mp4`, `.mkv`, `.webm`, `.mov`, `.avi`, `.m4v`, `.mpeg` y `.mpg` (sin distinguir mayúsculas en la extensión). Los nombres deben ser UTF-8 válido de hasta 255 bytes. El nombre que pide el cliente debe coincidir exactamente con el del catálogo. Los nombres con separadores de ruta o `..` se rechazan.

## Cliente

```
./cliente -d <HOST> -p <PUERTO> -a <ACCION> [opciones]
```

| Opción | Valor | Obligatoria | Descripción |
|---|---|---|---|
| `-d`, `--direccion` | IP o nombre resoluble | Sí | Servidor al que conectarse. |
| `-p`, `--puerto` | 1 a 65535 | Sí | Puerto del servidor. |
| `-a`, `--accion` | `canal`, `listar` o `descargar` | Sí | Operación solicitada. |
| `-f`, `--archivo` | Nombre del catálogo | Con `descargar` | Contenido a descargar. |
| `-o`, `--destino` | Archivo o directorio | Con `descargar` | Dónde guardar lo recibido. |
| `-t`, `--timeout` | Segundos (0 = sin límite) | No | Espera máxima de conexión y respuesta. Por omisión, 10. En `canal` no limita los silencios entre ciclos. |
| `-c`, `--tiempo-canal` | Segundos | No | Solo `canal`: duración máxima de la conexión. Sin esta opción es indefinida. |
| `-r`, `--reproductor` | Comando | No | Reproductor a usar. Debe leer de entrada estándar (terminar en `-` o `pipe:0`). Por omisión se detecta mpv, ffplay o vlc. |
| `-n`, `--sin-reproductor` | | No | No abre reproductor: los datos se reciben y se descartan (en `descargar` se guardan). |
| `-v`, `--verboso` | | No | Más detalle en la salida. |
| `-h`, `--help` | | No | Muestra la ayuda y termina. |

### Códigos de terminación del cliente

| Código | Significado |
|---|---|
| 0 | Éxito. |
| 1 | Error de argumentos. |
| 2 | Error de red o de conexión. |
| 3 | El servidor rechazó el contenido (por ejemplo, no hallado). |
| 4 | El servidor no tiene capacidad. |
| 5 | Error interno. |
| 130 | Interrumpido con Ctrl+C. |

Las descargas se escriben primero como `<destino>.part` y solo se renombran al nombre final cuando se recibió el FIN y los bytes exactos. Si se cancela o falla, el parcial se elimina.

## Ejemplos por servicio y por modo

Todos los ejemplos usan el directorio `videos/` incluido en el repositorio, con el servidor y el cliente en la misma máquina. Para clientes en otra máquina, sustituyan `127.0.0.1` por la IP del servidor.

### Servidor en cada modo

```bash
# FIFO: una conexión a la vez
./servidor -m FIFO -p 8080 -a videos -c videos/ciudad.ts

# FORKED: un proceso hijo por conexión, hasta 10 clientes simultáneos
./servidor -m FORKED -p 8080 -a videos -c videos/ciudad.ts,videos/flower.ts -s 2 -n 10

# THREADED: un hilo por conexión, hasta 10 clientes simultáneos
./servidor -m THREADED -p 8080 -a videos -c videos/ciudad.ts,videos/flower.ts -s 2 -n 10
```

### Servicio bajo demanda

```bash
# Listar el catálogo
./cliente -d 127.0.0.1 -p 8080 -a listar

# Descargar un video a un directorio (conserva el nombre)
./cliente -d 127.0.0.1 -p 8080 -a descargar -f ciudad.mp4 -o /tmp/

# Descargar a un archivo concreto
./cliente -d 127.0.0.1 -p 8080 -a descargar -f ciudad.mp4 -o /tmp/copia.mp4
```

Con `Ctrl+C` durante una descarga, el cliente envía la cancelación al servidor y borra el parcial.

### Canal en vivo

```bash
# Ver el canal con reproductor detectado automáticamente
./cliente -d 127.0.0.1 -p 8080 -a canal

# Ver el canal con ffplay
./cliente -d 127.0.0.1 -p 8080 -a canal -r "ffplay -autoexit -i pipe:0"

# Conectarse 30 segundos sin reproductor (útil para pruebas)
./cliente -d 127.0.0.1 -p 8080 -a canal -n -c 30
```

Un segundo cliente que se conecte unos segundos después ve el mismo punto del canal que el primero, no el inicio.

### Demostración de cada modo con canal y VOD a la vez

En `FORKED` y `THREADED`, con el servidor en una terminal:

```bash
# Terminal 2: espectador del canal
./cliente -d 127.0.0.1 -p 8080 -a canal -r "ffplay -autoexit -i pipe:0"

# Terminal 3: descarga mientras el canal sigue activo
./cliente -d 127.0.0.1 -p 8080 -a descargar -f ciudad.mp4 -o /tmp/
```

En `FIFO`, la Terminal 3 queda esperando hasta que el espectador del canal salga. Es el comportamiento esperado de ese modo.

## Reproductor de video

El cliente intenta, en este orden: `mpv`, `ffplay`, `cvlc` y `vlc`. Para el canal, envía los datos al reproductor por su entrada estándar. Con `-r` se puede indicar otro comando. Debe leer de la entrada estándar.

En equipos sin aceleración gráfica (por ejemplo WSL) `mpv` puede cerrarse solo al abrir su ventana. Alternativas:

```bash
./cliente -d 127.0.0.1 -p 8080 -a canal -r "ffplay -autoexit -i pipe:0"
./cliente -d 127.0.0.1 -p 8080 -a canal -r "mpv --vo=x11 --hwdec=no --force-window=yes -"
```

## Pruebas

```bash
make test                                   # pruebas unitarias
bash tests/test_concurrencia.sh             # los tres modos con servidor y cliente reales
bash tests/test_concurrencia.sh FORKED      # un solo modo
bash tests/diagnostico_sincronia.sh         # sincronía del canal, sin ventanas
bash tests/diagnostico_sincronia.sh --visual
```

## Limitaciones y notas

- **Sin cifrado ni autenticación.** El sistema está pensado para redes controladas (laboratorio o uso interno). No debe exponerse a redes no confiables.
- **FIFO y el canal:** una sesión del canal es indefinida, así que en `FIFO` mantiene ocupado al servidor hasta que el cliente salga. Es una consecuencia aceptada del modo.
- **Formato del canal:** solo MPEG-TS (`.ts`). Otros formatos se rechazan al arrancar y el servidor ofrece solo VOD.
- **Entrada tardía al canal:** al incorporarse, el reproductor puede mostrar uno o dos segundos de imagen negra o con errores hasta recibir el siguiente fotograma clave.
- **Pausa entre ciclos:** con `-s` el servidor no envía datos durante esa pausa. Si el reproductor se queda sin datos, la ventana puede detenerse hasta el siguiente ciclo.
- **Inactividad:** el servidor cierra una sesión sin saludo tras 30 s y una sesión inactiva tras 600 s.
- **Límite de clientes:** al alcanzar `-n`, la conexión nueva recibe `ERROR(CAPACIDAD)` y se cierra.