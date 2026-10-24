# Ghost in the HAL — pager cifrado punto a punto (kiwi board, ESP32-S3)

Dos placas KIWI que se mandan mensajes **cifrados de extremo a extremo** por
**ESP-NOW**, sin Wi-Fi ni servidores en el medio. Teclado radial de 4 cuadrantes,
menú de tres pantallas en el OLED y un agregado de **voz a texto**.

Una sola imagen para las dos placas: no hay rol emisor ni receptor.

## Hardware

kit KIWI (ESP32-S3, 512 KB RAM, sin PSRAM útil en este board): OLED SSD1306
128x64 por I2C/SPI, micrófono PDM, 4 botones + joystick de 5 direcciones,
consola por USB-Serial-JTAG.

## Menú

| Pantalla | sw1 | sw2 | sw3 | sw4 |
|---|---|---|---|---|
| SNIFFER | HELLO | Ver último | CONFIG | — |
| CONFIG | nick | par | Enviar mensaje | — |
| MENSAJE | teclado | voz | CONFIG | enviar |
| teclado | capa | espacio | borrar | enviar (vacío: atrás) |
| dictado | hablar | borrar | atrás | send |

El joystick mueve el cuadrante del teclado y cicla entre las tres pantallas
principales. El pie de cada pantalla dice qué hace cada botón.

## Cifrado (E2EE)

- HELLO en claro de 50 B: clave pública X25519, nonce y nick.
- ECDH + KDF SHA-256 por par y por arranque; la clave privada no sale de la placa.
- AES-256-GCM: nonce 12 B + contador 8 B anti-replay + tag 16 B
  (212 B útiles de los 250 B de payload ESP-NOW).
- Huella de 8 hex (hash de MACs y claves) visible en CONFIG: las dos placas
  tienen que mostrar el mismo número.

## Build y flash

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`sdkconfig.defaults` trae lo que necesita esta placa (16 MB de flash, partición
única grande, consola USB-Serial-JTAG, FreeRTOS 1 kHz). Las opciones del board
(pins, seconds de toma, ganancia del mic) están en `main/Kconfig.projbuild`.

Detalle no negociable: **abrir el puerto pulsea reset** (RTS está mapeado a EN),
así que toda la lectura tiene que pasar en una sola sesión serie.

## Voz a texto (parcial en este repo)

El modelo de ASR (~0,6 B) no entra en la placa: la placa captura el PDM, lo
etiqueta y lo manda por el puerto USB con el protocolo con CRC32; la PC hace de
gateway contra el nodo ASR y el texto vuelve al mismo borrador que usa el teclado,
para editarse y viajar cifrado igual.

Del lado de la placa está todo (`audio.c`, `dictado.c`, `transfer.c`). El puente
de PC y el nodo ASR **no** van en este repo por ahora.
