#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
    Transferencia PCM por la consola (USB-Serial-JTAG).

    Es un protocolo de lineas para poder convivir con los logs: el host
    descarta todo lo que no empiece con el prefijo. Cada chunk va en base64
    para que ningun byte binario rompa el parseo.

    Modo toma cerrada (grabamos N segundos y drenamos al final):

        KIWIAUDIO BEGIN frames=<n> rate=<hz> bits=16 ch=1 bytes=<n>
        KIWIAUDIO D n=<bytes de PCM> <base64>
        ...
        KIWIAUDIO END crc32=<hex> bytes=<n>

    Modo en vivo (KEY1 abre el micro, KEY1 lo cierra). Cambia solo el BEGIN:
    el largo total no se conoce todavia, asi que(bytes=0 y stream=1) y los
    chunks salen a medida que se graban:

        KIWIAUDIO BEGIN stream=1 frames=0 rate=<hz> bits=16 ch=1 bytes=0
        KIWIAUDIO D <base64>          <- cada ~19 ms de audio
        ...
        KIWIAUDIO END stream=1 crc32=<hex> bytes=<n> chunks=<n>

    En los dos sentidos: la PC puede escribirle a la placa por el mismo puerto.

        KIWIAUDIO T <base64(utf8)>   <- texto para el OLED
        KIWIAUDIO GO                 <- abrir/cerrar el micro (para probar sin dedo)
*/
esp_err_t transfer_send_pcm16(const int16_t *samples, size_t frames, uint32_t rate_hz,
                             void (*on_progress)(int percent));

/* true mientras esta saliendo un bloque (audio, GFX o stream) por la consola.
   Sirve para no arranchar una toma nueva encima del buffer que se esta drenando. */
bool transfer_ocupada(void);

/* Frames maximo por chunk de transfer_stream_chunk. 300 frames = 600 bytes,
   multiplo de 3: el base64 sale sin padding y el host puede concatenar. */
#define TRANSFER_CHUNK_FRAMES 300

esp_err_t transfer_stream_begin(uint32_t rate_hz);

esp_err_t transfer_stream_chunk(const int16_t *samples, size_t frames);

/* Cierra la toma y avisa el crc32 de todo el PCM mandar por consola.
   out_bytes/out_crc32 pueden ser NULL. */
esp_err_t transfer_stream_end(uint32_t *out_bytes, uint32_t *out_crc32);

typedef struct {
    char text[256]; /* texto que mando la PC para el OLED (vacio = no vino nada) */
    bool toggle;    /* "GO": la PC pide abrir/cerrar el microfono */
    bool dump;      /* "GFX": pedir instantanea del OLED (tools/pantalla_png.py) */
} transfer_msg_t;

/* Lee lo que llega del host sin bloquear nunca. Devuelve true si hay mensaje.  */
bool transfer_poll(transfer_msg_t *msg);

/*
    Instantanea del framebuffer hacia la PC, por el mismo canal de la consola:

        KIWIGFX BEGIN w=128 h=64 bytes=1024
        KIWIGFX D <base64>       <- 96 bytes por linea (128 chars, sin padding)
        KIWIGFX END crc32=<hex> bytes=1024

    Sirve para ver la pantalla sin camara (tools/pantalla_png.py). Se llama solo
    desde el loop principal, nunca a mitad del drenado de una toma.
*/
esp_err_t transfer_send_framebuffer(const uint8_t *fb, size_t len, int w, int h);

/*
    Base64 -> texto, para los comandos que manda la PC (texto <b64>). Escribe a
    lo sumo cap-1 bytes, siempre cierra con 0 y devuelve cuantos escribio.
*/
size_t transfer_b64_a_texto(const char *b64, char *out, size_t cap);
