# Protocolo binario — Servidor de video en C

**Documento:** v0.4 — contrato de control, VOD y API propuesta; LIVE pendiente de integración  
**Versión de protocolo:** 1  
**Fuente:** enunciado `estructurado.pdf`, sección 7.  
**Estado:** formatos de control y VOD definidos; integración de LIVE pendiente de acuerdo con servidor/reproductor.

## 1. Propósito y principios

Este protocolo de aplicación funciona sobre una conexión TCP entre un cliente y un servidor. Permite consultar un catálogo, descargar videos completos (VOD) e incorporarse a un canal compartido (LIVE). TCP entrega un flujo ordenado de bytes, no mensajes; el receptor debe reconstruir cada unidad leyendo su cabecera y la cantidad exacta de bytes indicada por ella.

Toda sesión empieza con `SALUDO` y `BIENVENIDA` (o `ERROR` si se rechaza). Cada solicitud recibe exactamente una respuesta de control inicial (`BIENVENIDA`, `CATALOGO`, `ACEPTADO`, `EMISION`, `DESPEDIDA`, `CANCELADO` o `ERROR`, según corresponda); después pueden seguir mensajes `DATOS` y, en VOD, `FIN`. Un `ERROR` terminal posterior a `ACEPTADO` indica que una transferencia previamente aceptada fracasó; no es una segunda respuesta de admisión. No se entrelazan dos transferencias en una sesión.

## 2. Formato de cabecera (12 bytes)

| Offset | Campo | Tipo | Bytes |
|---:|---|---|---:|
| 0 | `version` | `u8` | 1 |
| 1 | `type` | `u8` | 1 |
| 2 | `status` | `u16` | 2 |
| 4 | `request_id` | `u32` | 4 |
| 8 | `payload_length` | `u32` | 4 |

- `u8`, `u16`, `u32`, `u64`: enteros sin signo de 8, 16, 32 y 64 bits.
- Todos los enteros de más de un byte se codifican en **big-endian** (byte más significativo primero).
- La cabecera siempre mide 12 bytes. El mensaje completo mide `12 + payload_length` bytes.
- Las estructuras C **no se transmiten con `send(fd, &struct, sizeof struct, ...)`**; cada campo se serializa explícitamente para evitar problemas de padding y endianidad.
- `version = 1`. En solicitudes, `status = 0`. En respuestas exitosas, `status = 0`. `ERROR` usa el código no cero correspondiente.
- `request_id` es `u32` y debe ser distinto de cero. La respuesta y todos los `DATOS`/`FIN` asociados reutilizan el ID de la solicitud. Un ID nuevo no debe coincidir con una solicitud pendiente.
- **Límite de payload por mensaje:** 65 536 bytes. El receptor valida este límite antes de reservar memoria. Se aplica a todos los tipos, no al tamaño total de un archivo.
- Texto: UTF-8, sin terminador NUL en la red. Nombres de catálogo: 1–255 bytes, contados como bytes, no como caracteres.
- `version` y `type` usan `u8` como elección concreta del equipo para los «enteros cortos» abstractos del enunciado.

### Ejemplo de cabecera

Solicitud `LISTAR`, versión 1, ID 7, sin payload:

```text
01 03 00 00 00 00 00 07 00 00 00 00
```

## 3. Códigos de mensajes

| Código | Nombre | Emisor | Payload |
|---:|---|---|---|
| 1 | `SALUDO` | Cliente | Vacío |
| 2 | `BIENVENIDA` | Servidor | `u8 services` |
| 3 | `LISTAR` | Cliente | Vacío |
| 4 | `CATALOGO` | Servidor | Catálogo binario |
| 5 | `ABRIR` | Cliente | Nombre de contenido |
| 6 | `ACEPTADO` | Servidor | Metadatos VOD, o vacío al responder a `SALIR` |
| 7 | `DATOS` | Servidor | Bytes opacos |
| 8 | `FIN` | Servidor | Vacío |
| 9 | `ERROR` | Servidor | Descripción UTF-8 |
| 10 | `CERRAR` | Cliente | Vacío |
| 11 | `DESPEDIDA` | Servidor | Vacío |
| 12 | `UNIR` | Cliente | Vacío |
| 13 | `EMISION` | Servidor | Metadatos LIVE |
| 14 | `SALIR` | Cliente | Vacío |
| 15 | `CANCELAR` | Cliente | Vacío |
| 16 | `CANCELADO` | Servidor | Vacío |

Códigos 0 y 17–255 reservados. Un mensaje con tipo desconocido recibe `ERROR(SOLICITUD)` si es posible responder de forma segura.

## 4. Payloads exactos

### 4.1 Inicio y cierre

- `SALUDO`: payload vacío; la versión se declara en la cabecera.
- `BIENVENIDA`: **1 byte** `services`, máscara de bits: bit 0 (`0x01`) = VOD; bit 1 (`0x02`) = LIVE. Un servidor que ofrece ambos responde `0x03`. Bits 2–7 reservados y deben ser cero en v1.
- `CERRAR`: payload vacío. `DESPEDIDA`: payload vacío. Tras enviar `DESPEDIDA`, el servidor cierra ordenadamente la conexión.

### 4.2 Catálogo

`LISTAR` tiene payload vacío. `CATALOGO` contiene:

```text
u16 entry_count
repeat entry_count times:
    u16 name_length
    u8  name[name_length]   // UTF-8
    u64 file_size_bytes
```

- `entry_count` entre 0 y 255. Nombres entre 1 y 255 bytes; deben ser únicos en el catálogo.
- No se envían rutas internas del servidor. El nombre es un identificador lógico de contenido.
- `file_size_bytes` representa el tamaño real del archivo VOD en bytes. El catálogo anuncia contenidos VOD; el canal LIVE se identifica por `EMISION`.
- Catálogo vacío: payload `00 00`.
- La suma de todas las entradas debe caber en 65 536 bytes. Si no cabe, el servidor devuelve `ERROR(CAPACIDAD)` con explicación; v1 no pagina el catálogo.

### 4.3 VOD

`ABRIR`:

```text
u16 name_length
u8  name[name_length]   // UTF-8, 1..255 bytes
```

`ACEPTADO` como respuesta a `ABRIR`:

```text
u64 file_size_bytes
u8  transfer_format     // 1 = bytes originales del archivo, sin transformación
```

**Nota de revisión:** en v0.1 `ACEPTADO` tenía 8 bytes; ahora son **9 bytes** para satisfacer el requisito del enunciado de anunciar el formato de transferencia. El valor `1` indica que los bytes se transmiten tal como están almacenados, sin recodificación. No debe confundirse con el formato multimedia (MP4, MKV, etc.), que conserva su identidad en el nombre del contenido.

`DATOS`: payload de 1 a 65 536 bytes de contenido original. Un archivo vacío puede completarse sin `DATOS`. Cada bloque lleva el `request_id` del `ABRIR`.

`FIN`: payload vacío, mismo ID. El cliente considera exitosa la transferencia **solo si** recibió `FIN` y la suma de bytes de todos los `DATOS` es exactamente `file_size_bytes`. Un `FIN` prematuro, exceso de bytes o desconexión antes de completar la transferencia es un error; no debe reportarse éxito ni conservar el archivo como descarga completa.

Flujo:

```text
C -> S: ABRIR(nombre), id=20
S -> C: ACEPTADO(tamaño, formato), id=20
S -> C: DATOS(...), id=20    // 0 o más veces
S -> C: FIN, id=20
```

### 4.4 Errores

`ERROR` usa `status` distinto de cero y payload de texto UTF-8 sin terminador NUL (0 a 65 536 bytes; se recomienda una descripción breve). Su `request_id` coincide con la solicitud rechazada cuando puede identificarse.

| Estado | Nombre | Motivo |
|---:|---|---|
| 0 | `EXITO` | Solicitud exitosa |
| 1 | `VERSION` | Versión incompatible |
| 2 | `SOLICITUD` | Tipo, sintaxis o longitud inválida |
| 3 | `NO_HALLADO` | Nombre no presente en catálogo |
| 4 | `NO_SERVIBLE` | Contenido existente pero no disponible |
| 5 | `NO_PERMITIDO` | Operación inválida en el estado actual |
| 6 | `CAPACIDAD` | Límite de clientes o capacidad de respuesta |
| 7 | `INTERNO` | Fallo interno del servidor |

Si `SALUDO` tiene versión incompatible, el servidor intenta responder `ERROR(VERSION)` con **su propia versión admitida** en la cabecera y luego cierra la conexión. Un error de sintaxis recuperable no exige cerrar la sesión; un flujo irrecuperablemente desincronizado sí.

### 4.5 Cancelación y cierre (extensión propia)

`CANCELAR` (tipo 15) y `CANCELADO` (tipo 16) son **extensiones del equipo**, no mensajes obligatorios enumerados por el enunciado. Ambos tienen `payload_length = 0` y `status = 0`. Se emplea el mismo `request_id` de la solicitud `ABRIR` que originó la descarga. Una cancelación sólo es válida durante VOD activo de ese ID; no inicia otra transferencia ni cierra TCP.

- El servidor debe seguir leyendo solicitudes de control durante el envío VOD, con un mecanismo compatible con su modelo de concurrencia. Al procesar `CANCELAR`, deja de producir nuevos `DATOS` para ese ID. Si ya inició la escritura de una unidad `DATOS`, la completa antes de emitir `CANCELADO`, para preservar el encuadre binario.
- `CANCELADO` es la **respuesta de control a `CANCELAR`** y barrera de finalización: después de ella el servidor no envía más `DATOS` ni `FIN` de ese ID. Bytes ya en tránsito antes de esa confirmación pueden llegar antes de `CANCELADO`.
- El cliente considera la descarga cancelada e incompleta, descarta el archivo parcial o lo marca inequívocamente como parcial, y puede emitir otro `ABRIR` después de `CANCELADO`.
- Si la transferencia ya terminó o el ID no corresponde a una descarga VOD activa, responder `ERROR(NO_PERMITIDO)` al ID recibido y mantener la sesión cuando sea seguro.
- `CERRAR` es distinto de `CANCELAR`: tiene su **propio `request_id` nuevo**, detiene cualquier VOD o LIVE activo y concluye la sesión. Se termina de escribir la unidad de protocolo que ya estuviera en curso; después no se generan más `DATOS`, se emite `DESPEDIDA` con el ID de `CERRAR` como último mensaje de aplicación y se cierra TCP ordenadamente. No se garantiza retirar bytes previamente transmitidos.
- Si ocurre un error de lectura tras `ACEPTADO`, el servidor envía `ERROR(INTERNO)` con el ID de `ABRIR` en vez de `FIN`, cuando el transporte todavía lo permite. El cliente marca la descarga como fallida. Si el transporte se perdió, el cliente detecta EOF/error/timeout sin esperar un `ERROR` imposible de entregar.

**Regla de control:** `CANCELAR` y `CERRAR` son excepciones expresas a la prohibición de nuevas operaciones durante `TRANSFIRIENDO`; no permiten dos transferencias simultáneas. El resto de solicitudes de contenido se rechazan con `ERROR(NO_PERMITIDO)`.

### 4.6 LIVE — contrato de control provisional

`UNIR`: payload vacío. No permite elegir una posición arbitraria.

`EMISION`:

```text
u64 cycle_duration_ms
u64 position_ms
u16 name_length
u8  name[name_length]  // nombre lógico del contenido activo, UTF-8
```

- `cycle_duration_ms > 0` y `0 <= position_ms < cycle_duration_ms`.
- El servidor obtiene la posición del reloj compartido del canal, no de un reloj privado por cliente.
- `DATOS` lleva bytes opacos del canal con el ID de `UNIR`.
- `SALIR`: payload vacío; el servidor detiene el envío LIVE y responde **`ACEPTADO` con payload vacío**, usando el ID de `SALIR`. La sesión vuelve a `LISTA` y TCP permanece abierto.
- **Pendiente de integración obligatorio:** acordar con el equipo cómo convertir `position_ms` en un punto reproducible del archivo o flujo y cómo anunciar el códec/contenedor cuando sea necesario. La estructura de `EMISION` podrá ampliarse antes de congelar la versión 1. No debe suponerse que milisegundos equivalen directamente a desplazamientos en bytes de MP4.

## 5. Máquina de estados

Estados del enunciado: `INICIO`, `SALUDADA`, `LISTA`, `TRANSFIRIENDO`, `CERRANDO`.

| Estado | Evento | Acción / siguiente estado |
|---|---|---|
| `INICIO` | `SALUDO` válido | `BIENVENIDA`; `SALUDADA` |
| `INICIO` | Otro mensaje | `ERROR(NO_PERMITIDO)`; permanece o cierra si no recuperable |
| `SALUDADA` | `LISTAR` | `CATALOGO`; `LISTA` |
| `SALUDADA` | `ABRIR` válido | `ACEPTADO`; `TRANSFIRIENDO` |
| `SALUDADA` | `UNIR` válido | `EMISION`; `TRANSFIRIENDO` |
| `SALUDADA` | `CERRAR` | `DESPEDIDA`; `CERRANDO` |
| `LISTA` | `LISTAR` | `CATALOGO`; `LISTA` |
| `LISTA` | `ABRIR` válido | `ACEPTADO`; `TRANSFIRIENDO` |
| `LISTA` | `UNIR` válido | `EMISION`; `TRANSFIRIENDO` |
| `LISTA` | `CERRAR` | `DESPEDIDA`; `CERRANDO` |
| `TRANSFIRIENDO` (VOD) | Transferencia completada con `FIN` | `LISTA` |
| `TRANSFIRIENDO` (VOD) | `CANCELAR` del ID activo | Completar unidad en curso; `CANCELADO`; `LISTA` |
| `TRANSFIRIENDO` (VOD) | Fallo de lectura del contenido | `ERROR(INTERNO)` terminal si es posible; `LISTA` o cierre si falla transporte |
| `TRANSFIRIENDO` (LIVE) | `SALIR` | `ACEPTADO` vacío; `LISTA` |
| `TRANSFIRIENDO` | Nueva solicitud de contenido | `ERROR(NO_PERMITIDO)`; sigue transferencia actual |
| `TRANSFIRIENDO` | `CERRAR` | Detiene VOD/LIVE, `DESPEDIDA` como último mensaje; `CERRANDO` |
| `CERRANDO` | Cualquier solicitud | No inicia operaciones nuevas; cierre de TCP |

`SALUDADA` representa la admisión tras el saludo; `LISTA` es el estado estable después de una primera operación de catálogo o transferencia. Ambos admiten solicitudes de contenido. No se permiten transferencias simultáneas en la misma sesión. Una solicitud durante transferencia se **rechaza**, no se encola.

**Importante para integración:** durante VOD el servidor debe comprobar `CANCELAR` y `CERRAR` según el modelo de concurrencia del equipo; no basta con un bucle que solo envíe datos. Los mensajes de una misma conexión deben escribirse de forma serializada para no intercalar bytes de dos unidades.

## 6. Reglas obligatorias para implementar

1. **Lectura exacta:** reunir 12 bytes de cabecera aunque hagan falta varias llamadas a `recv`; después reunir exactamente `payload_length` bytes. No asumir que una llamada equivale a un mensaje.
2. **Escritura completa:** repetir `send` hasta transmitir todos los bytes o detectar error. No asumir que una llamada envía todo.
3. **EOF:** `recv == 0` indica cierre de la dirección de envío del par. Si ocurre a mitad de cabecera, payload o transferencia, reportar mensaje/transferencia incompleta.
4. **Validar antes de asignar memoria:** versión, tipo, límite de payload, longitud específica del tipo, nombres y estado de sesión. Nunca confiar en longitudes anunciadas por el par.
5. **Nombres seguros:** aceptar únicamente identificadores existentes del catálogo; rechazar nombres vacíos, bytes NUL, separadores `/` y `\\`, y componentes de ruta como `..`. Nunca concatenar directamente una ruta recibida sin validación.
6. **Correlación:** respuestas de control, `DATOS` y `FIN` deben usar el ID correcto. `DATOS` o `FIN` sin transferencia asociada son inválidos.
7. **Contabilidad VOD:** contar bytes efectivos de payload `DATOS`, no bytes de cabecera. Validar tamaño total y `FIN`.
8. **Recuperación:** `ERROR` por solicitud malformada recuperable; cerrar si no es posible delimitar con seguridad el siguiente mensaje o se pierde sincronización.
9. **Límites:** respetar los máximos de nombres, entradas y payload. No exponer rutas ni detalles internos en mensajes de error.
10. **LIVE:** sincronización con reloj monotónico compartido, no acumular deriva con esperas sucesivas; la selección de fragmentos reproducibles depende de la implementación del canal.

## 7. Contrato mínimo previsto para `protocol.h` / `protocol.c`

La interfaz exacta se definirá al implementar, pero debe cubrir:

- Constantes: versión, tamaño de cabecera, límite de payload, tipos, códigos de estado, máscara de servicios y formato de transferencia.
- Estructura lógica de cabecera con campos de tipos enteros de ancho fijo (`stdint.h`).
- Serialización y deserialización de cabeceras a/desde arreglos de **12 bytes**.
- Serialización/deserialización de `u16`, `u32` y `u64` en big-endian, sin depender de padding de `struct`.
- Construcción y validación de payloads de `ABRIR`, `CATALOGO`, `ACEPTADO`, `BIENVENIDA`, `EMISION` y `ERROR`.
- Funciones o utilidades para recibir/enviar una cantidad exacta de bytes, con resultados diferenciados para éxito, EOF, error de E/S y protocolo inválido.
- Gestión explícita de memoria para payloads variables: quién reserva y quién libera.

**Separación de responsabilidades:** `protocol.c` codifica/decodifica y valida unidades del protocolo; el servidor decide qué archivo abrir y qué estado mantener; `timeline.c` calcula la posición LIVE; el cliente decide cómo reproducir los bytes.

## 8. Pruebas mínimas

- Cabecera de 12 bytes y big-endian, con ejemplo conocido.
- Mensajes sin payload, payload variable y longitud máxima permitida.
- `recv` fragmentado (por ejemplo 8 + 4 + payload) y dos mensajes en una misma recepción.
- Longitud excesiva, tipo desconocido, versión incompatible, ID inválido y payload truncado.
- Catálogo vacío y múltiples entradas; nombres inválidos y límites.
- VOD completo, bytes idénticos al archivo origen, `FIN` prematuro y desconexión a mitad.
- Respuestas con IDs correctos; solicitud concurrente rechazada explícitamente.
- LIVE: incorporación tardía y posición compartida; salida y reingreso sin reiniciar el ciclo.

## 9. Pendientes antes de congelar v1

1. **Acordar con responsables de LIVE y reproductor** el mecanismo de inicio reproducible y los metadatos extra que necesite `EMISION`.
2. **Revisar con el equipo** si el indicador `transfer_format=1` basta para los reproductores usados o si se necesita declarar explícitamente contenedor/códec.
3. **Verificar con el profesor/equipo** la elección `u8` para versión/tipo, ya que el enunciado los llama enteros cortos pero deja libre la serialización.
4. Revisar detalles de integración de cierre durante envío en cada modo de concurrencia.

Los puntos anteriores no impiden comenzar la implementación de serialización de cabeceras, catálogo, VOD y validación general. **No presentar el contrato LIVE como completamente cerrado hasta coordinar su reproducción real.**

## 10. Decisiones de implementación y API

- La capa `protocol` **no administra** el estado de sesión, el sistema de archivos, el reloj LIVE ni la reproducción: proporciona tipos, codificación, decodificación y validaciones de formato. El servidor aplica la máquina de estados y coordina la cancelación.
- Una llamada de recepción debe distinguir al menos: mensaje completo, EOF limpio entre mensajes, EOF truncado a mitad de mensaje, error de sistema y error de formato.
- Para payloads variables, la API debe declarar inequívocamente propiedad y liberación de memoria; no reservar más de 65 536 bytes por mensaje.
- Para cancelación, el bucle de servidor debe leer mensajes de control mientras escribe VOD y serializar toda escritura sobre el socket.
- Antes de congelar el contrato LIVE, confirmar la forma de identificar fragmentos reproducibles y metadatos de formato; no inferir offsets de archivo a partir de milisegundos.


## 11. API pública propuesta (cerrada para implementación local)

Esta API no crea hilos, no abre archivos, no decide estados de sesión y no calcula la posición LIVE. Sus funciones de lectura/escritura sobre sockets son bloqueantes; el servidor debe gestionar concurrencia, interrupción y tiempos de espera por separado.

### 11.1 Tipos y códigos de retorno

```c
#define PROTOCOL_VERSION       1u
#define PROTOCOL_HEADER_SIZE  12u
#define PROTOCOL_MAX_PAYLOAD  65536u
#define PROTOCOL_MAX_NAME     255u

typedef struct {
    uint8_t version;
    uint8_t type;
    uint16_t status;
    uint32_t request_id;
    uint32_t payload_length;
} protocol_header_t;

typedef enum {
    PROTOCOL_OK = 0,
    PROTOCOL_EOF,          /* EOF limpio antes de empezar otra cabecera */
    PROTOCOL_TRUNCATED,    /* EOF dentro de una unidad */
    PROTOCOL_INVALID,      /* sintaxis, tamaño, versión o tipo inválido */
    PROTOCOL_IO_ERROR,     /* error de socket, consultar errno */
    PROTOCOL_NO_MEMORY     /* fallo al reservar memoria */
} protocol_result_t;

typedef struct {
    protocol_header_t header;
    uint8_t *payload; /* NULL cuando payload_length == 0 */
} protocol_message_t;
```

Los tipos de mensaje y estados de aplicación se declaran como constantes `enum` con los valores de las tablas anteriores; **no** se transmite un `enum` ni una estructura C cruda por la red.

### 11.2 Firmas públicas

```c
protocol_result_t protocol_encode_header(
    const protocol_header_t *header,
    uint8_t out[PROTOCOL_HEADER_SIZE]);

protocol_result_t protocol_decode_header(
    const uint8_t in[PROTOCOL_HEADER_SIZE],
    protocol_header_t *out);

protocol_result_t protocol_send_message(
    int fd, const protocol_header_t *header, const uint8_t *payload);

protocol_result_t protocol_recv_message(
    int fd, protocol_message_t *out);

void protocol_message_free(protocol_message_t *message);
```

- `encode_header` valida versión, tipo, estado e ID, además del límite general de payload. `decode_header` aplica esas mismas comprobaciones y no reserva memoria.
- `send_message` comprueba la coherencia entre cabecera y payload; `payload == NULL` sólo es válido cuando `payload_length == 0`. Envía cabecera y payload íntegros o informa un fallo; **no** cierra `fd`.
- `recv_message` lee 12 bytes completos y luego exactamente el payload anunciado; sólo reserva memoria tras validar la cabecera. Si falla, deja `out` inicializado a cero y libera cualquier reserva parcial. Si tiene éxito, el llamador es dueño de `out->payload`.
- `protocol_message_free` libera el payload, deja el puntero en `NULL` y pone `payload_length = 0`; no cierra el socket.
- Los parámetros `fd` pertenecen al llamador. Ninguna función de este módulo hace `close(fd)`.
- Las funciones de socket bloqueantes pueden interrumpirse por señales (`EINTR`); se reintenta la operación. El manejo de `EAGAIN`/`EWOULDBLOCK` en sockets no bloqueantes queda fuera de esta API bloqueante y se resolverá en integración si se requieren.
- **Importante:** un error durante `send_message` puede dejar una unidad parcialmente transmitida; el servidor no debe intentar escribir otra unidad sobre ese flujo como si nada. Debe tratar la conexión como no recuperable salvo que una capa superior pueda completar la unidad.

### 11.3 Payloads: interfaz de codificación

Para evitar múltiples convenciones de memoria, los codificadores de payload usarán **buffer suministrado por el llamador**, capacidad explícita y longitud de salida. Los decodificadores recibirán puntero + longitud, nunca asumirán terminación NUL, y devolverán vistas a bytes de entrada cuando sea seguro. Los nombres que el servidor necesite como cadenas C se copiarán a un buffer con espacio para `\0`.

Firmas mínimas sugeridas:

```c
protocol_result_t protocol_encode_name(
    const uint8_t *name, uint16_t name_length,
    uint8_t *out, size_t capacity, size_t *written);

protocol_result_t protocol_decode_name(
    const uint8_t *payload, size_t length,
    const uint8_t **name, uint16_t *name_length);

protocol_result_t protocol_encode_vod_accepted(
    uint64_t file_size, uint8_t transfer_format,
    uint8_t out[9]);

protocol_result_t protocol_decode_vod_accepted(
    const uint8_t *payload, size_t length,
    uint64_t *file_size, uint8_t *transfer_format);

protocol_result_t protocol_validate_payload(
    const protocol_header_t *header, const uint8_t *payload);
```

`protocol_validate_payload` comprueba la forma binaria de los tipos que ya están definidos (payload vacío, longitud de nombre, estructura del catálogo, `ACEPTADO` VOD, etc.). No comprueba si un archivo existe, si un ID está activo o si el usuario tiene permitido cambiar de estado: eso pertenece al servidor. La forma exacta de los payloads LIVE podrá ajustarse antes de implementar sus codificadores específicos.

### 11.4 Reglas de validación cerradas

1. En solicitudes y respuestas exitosas, `status == 0`. En `ERROR`, `status` debe ser uno de los códigos 1–7; un tipo distinto de `ERROR` con estado no cero es inválido.
2. En mensajes normales, `request_id != 0`. Si una cabecera malformada no permite confiar en el ID, el servidor puede cerrar sin responder; **nunca** debe inventar una correlación fiable.
3. Los payloads declarados vacíos deben tener longitud exactamente cero. `BIENVENIDA` debe tener exactamente un byte y sólo admite bits `0x01` y `0x02` (al menos un servicio activo).
4. `ABRIR` ocupa exactamente `2 + name_length` bytes; `name_length` está entre 1 y 255. Se rechazan bytes NUL, `/`, `\`, y el componente `..`; el servidor resuelve nombres sólo contra el catálogo.
5. `ACEPTADO` para VOD ocupa exactamente 9 bytes y exige `transfer_format == 1`. `ACEPTADO` para `SALIR` ocupa cero bytes; **la interpretación depende del estado y de la solicitud pendiente**, no sólo del tipo del mensaje.
6. `DATOS` requiere entre 1 y 65 536 bytes. `FIN`, `CANCELAR`, `CANCELADO`, `SALIR` y demás mensajes de control sin payload exigen longitud cero.
7. `CATALOGO` exige consumir exactamente todos sus bytes al recorrer `entry_count` entradas, sin bytes sobrantes ni campos incompletos. El decodificador valida estructura y límites; la unicidad de nombres puede comprobarla el productor del catálogo.
8. `ERROR` permite texto UTF-8 vacío o no vacío, pero no se debe exponer rutas, contraseñas ni mensajes internos del sistema operativo.
9. `protocol_recv_message` sólo valida **sintaxis y formato**. La política de errores recuperables/fatales se decide en la sesión. Si una cabecera tiene una longitud mayor al máximo, no se reserva memoria ni se intenta confiar en la siguiente cabecera: se cierra el flujo o se descarta explícitamente todo el cuerpo con un límite de seguridad definido por el servidor.
10. Un `CANCELAR` o `CERRAR` recibido durante VOD no puede interrumpir los bytes de un `DATOS` ya empezado. La capa de servidor debe terminar esa unidad antes de responder y debe coordinar la escritura de la conexión.

### 11.5 Ejemplos de bytes comprobables

- `LISTAR`, id=7, payload vacío: `01 03 00 00 00 00 00 07 00 00 00 00`.
- `ABRIR`, id=20, nombre `a.mp4` (5 bytes): cabecera `01 05 00 00 00 00 00 14 00 00 00 07`; payload `00 05 61 2E 6D 70 34`.
- `ACEPTADO` VOD, id=20, tamaño 10 000 bytes, formato 1: cabecera `01 06 00 00 00 00 00 14 00 00 00 09`; payload `00 00 00 00 00 00 27 10 01`.
- `FIN`, id=20: `01 08 00 00 00 00 00 14 00 00 00 00`.
- `CANCELAR`, id=20: `01 0F 00 00 00 00 00 14 00 00 00 00`.

## 12. Puntos que requieren coordinación externa (no bloquear serialización local)

- LIVE: traducción de `position_ms` a fragmentos reproducibles y metadatos adicionales para el reproductor.
- Servidor: estrategia de lectura de control mientras envía VOD, serialización de escrituras y cierre sin truncar una unidad.
- Cliente/reproductor: confirmar si `transfer_format=1` y nombre del archivo proporcionan información suficiente sobre contenedor/códec.
- Equipo/profesor: ratificar que los códigos concretos de campos y las extensiones `CANCELAR`/`CANCELADO` son aceptables como diseño propio.

**Criterio de salida de la sección 01:** se puede implementar ahora la codificación de cabecera, los mensajes de control y VOD, y sus pruebas unitarias. La compatibilidad extremo a extremo y LIVE no se declaran cerradas hasta completar la coordinación anterior.
