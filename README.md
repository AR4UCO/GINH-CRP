# ESP-NOW E2EE — Cifrado extremo a extremo para ESP32-S3

Mismo firmware para todas las placas. Las placas se auto-descubren por
broadcast y establecen **canales cifrados punto a punto** sin ninguna clave
grabada en el firmware.

## Cómo funciona

1. Al arrancar, cada placa genera un **par de claves X25519 efímero** (solo
   vive en RAM) y lo anuncia por broadcast en un mensaje `HELLO`.
2. Cada **par de placas** hace ECDH y deriva su propia **clave de sesión
   AES-256-GCM** (ConcatKDF-SHA-256). TOFU: se acepta la primera clave
   pública vista de cada MAC.
3. Todo el tráfico de datos va cifrado y autenticado con AES-256-GCM, con
   contador monotónico contra replay. AAD = MAC del emisor, así nadie puede
   reetiquetar un mensaje como de otra placa.
4. Al establecerse cada sesión se imprime una **huella** por el puerto serie
   (idéntica en ambas placas): comparándola se detecta un man-in-the-middle
   durante el emparejado.

## Formato de paquetes (capa de aplicación sobre ESP-NOW)

```
HELLO:  [ver=1][tipo=1][pub X25519:32][boot_nonce:8][nombre:8]   (sin cifrar)
DATA:   [ver=1][tipo=2][nonce:12][AES-256-GCM(seq:8 || payload)][tag:16]
        nonce = salt_sesión:4 || seq:8  (bit de dirección en el salt)
```

Propiedades: confidencialidad e integridad por par, secreto hacia adelante
entre reinicios (claves efímeras), anti-replay, anti-suplantación de emisor.

## Uso

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # repetir en cada placa
```

Al emparejarse, ambas placas muestran la misma huella (compárenla):

```
I (xxx) E2EE: E2EE listo. Soy ESP-4A1B (24:ec:4a:11:22:33)
I (xxx) E2EE: Sesión segura establecida con ESP-9C2D (48:ca:43:aa:bb:cc)
I (xxx) E2EE: Huella del par 48:ca:43:aa:bb:cc: 7F3A91C2-04BE88D1
```

### Conexión de la consola

Este proyecto usa la consola **USB-Serial/JTAG nativa del ESP32-S3**
(`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`): un solo cable USB al conector
"USB" de la placa y aparece como `/dev/ttyACM0`, `/dev/ttyACM1`, etc.

```bash
idf.py -p /dev/ttyACM0 flash monitor   # una placa
idf.py -p /dev/ttyACM1 flash monitor   # la otra (en otra terminal)
```

Si en cambio usas el conector "UART" (puente CP2102/CH340), ese aparece como
`/dev/ttyUSB0` y hay que volver a poner la consola en UART0:
`idf.py menuconfig` → *Component config → ESP System Settings → Channel for
console output* → **UART0** (GPIO43=TX, GPIO44=RX, 115200 8N1).

### Mensajería interactiva (consola por USB/UART)

Cada placa tiene un prompt `e2ee>`. Comandos:

| Comando | Acción |
|---|---|
| `send <texto>` | Cifra y envía a **todos** los peers (un unicast por peer) |
| `sendto <índice\|MAC> <texto>` | Envía a un peer concreto (índice de `peers` o MAC) |
| `peers` | Lista peers: nombre, MAC y huella |
| `hello` | Re-anuncia por broadcast (fuerza descubrimiento) |
| `whoami` | Muestra nombre y MAC propios |
| `help` | Ayuda |

Ejemplo de conversación entre las dos placas:

```
e2ee> send hola desde la placa A
-> ESP-9C2D (48:ca:43:aa:bb:cc)
```
```
I (xxx) ESP-NOW-E2EE: MSG de ESP-4A1B: hola desde la placa A
```

Los HELLO periódicos son silenciosos cuando ya hay sesión: solo se ven
mensajes, huellas y errores de autenticación/replay.

## Límites (diseño de laboratorio)

- Payload de aplicación: **212 bytes** por mensaje (`E2EE_MAX_PAYLOAD`).
- Máx. **8 peers** simultáneos (`MAX_PEERS` en `e2ee.c`).
- TOFU: la ventana de ataque MITM es solo durante el primer HELLO; verifiquen
  la huella por serie si importa.
- Un atacante puede forzar re-negociación (DoS) reenviando HELLOs viejos;
  no obtiene ninguna clave, pero puede interrumpir el enlace.

## Archivos

| Archivo | Contenido |
|---|---|
| `main/e2ee.c` / `e2ee.h` | Protocolo: handshake X25519, KDF, AES-GCM, tabla de sesiones |
| `main/cli.c` / `cli.h` | Consola interactiva: `send`, `sendto`, `peers`, `hello`, `whoami` |
| `main/hello_world_main.c` | App: WiFi/ESP-NOW, cola RX + tarea cripto, re-anuncio periódico |
