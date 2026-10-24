#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/usb_serial_jtag.h"
#include "transfer.h"

/* 576 bytes = 192 caracteres base64: multiplo de 3, asi ningun chunk necesita
   padding y el host puede concatenar directo. */
#define CHUNK_BYTES 576
#define CHUNK_CHARS ((CHUNK_BYTES / 3) * 4)

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char TAG[] = "transfer";

/* Mientras se mandan chunks de audio no se puede loguear nada: un log a mitad
   de una linea base64 hace que el host decodifique basura (nos paso: una toma
   de 10 s llego como ruido). Se prende en los dos modos de envio. */
static bool s_stream_active;

static size_t b64_encode(const uint8_t *in, size_t in_len, char *out)
{
    size_t o = 0;

    while (in_len >= 3) {
        const uint32_t v = (in[0] << 16) | (in[1] << 8) | in[2];
        out[o++] = B64[(v >> 18) & 0x3f];
        out[o++] = B64[(v >> 12) & 0x3f];
        out[o++] = B64[(v >> 6) & 0x3f];
        out[o++] = B64[v & 0x3f];
        in += 3;
        in_len -= 3;
    }

    if (in_len == 1) {
        const uint32_t v = in[0] << 16;
        out[o++] = B64[(v >> 18) & 0x3f];
        out[o++] = B64[(v >> 12) & 0x3f];
        out[o++] = '=';
        out[o++] = '=';
    } else if (in_len == 2) {
        const uint32_t v = (in[0] << 16) | (in[1] << 8);
        out[o++] = B64[(v >> 18) & 0x3f];
        out[o++] = B64[(v >> 12) & 0x3f];
        out[o++] = B64[(v >> 6) & 0x3f];
        out[o++] = '=';
    }

    return o;
}

/*
    Silenciamos los logs mientras sale un bloque base64 por la consola. No
    alcanza con que NOSOTROS no logueemos: en el merge hay otras tasks hablando
    (Wi-Fi/ESP-NOW marca 'MSG de ...', el HELLO periodico, coex) y un solo log a
    mitad de una linea de ~780 caracteres desfasa todo el PCM (medido en placa:
    llegaron 61078 de 63488 bytes y el crc no cerraba). Guarda el nivel anterior
    para restaurarlo; devuelve ese nivel por si el llamador lo quiere ver.
*/
static int s_nivel_log_previo = -1;

/* Esta saliendo un bloque por la consola ahora mismo? Ver transfer.h. */
bool transfer_ocupada(void)
{
    return s_stream_active;
}

static void silencio_de_logs(bool silenciar)
{
    if (silenciar) {
        if (s_nivel_log_previo < 0) {
            s_nivel_log_previo = (int)esp_log_level_get("*");
            esp_log_level_set("*", ESP_LOG_NONE);
        }
        return;
    }
    if (s_nivel_log_previo >= 0) {
        esp_log_level_set("*", (esp_log_level_t)s_nivel_log_previo);
        s_nivel_log_previo = -1;
    }
}

esp_err_t transfer_send_pcm16(const int16_t *samples, size_t frames, uint32_t rate_hz,
                              void (*on_progress)(int percent))
{
    char line[sizeof("KIWIAUDIO D n=4294967295 ") + CHUNK_CHARS + 2];
    char pref[sizeof("KIWIAUDIO D n=4294967295 ")];
    const size_t total_bytes = frames * sizeof(int16_t);
    uint32_t crc = 0;
    int chunk = 0;

    s_stream_active = true;
    silencio_de_logs(true);

    printf("KIWIAUDIO BEGIN frames=%u rate=%" PRIu32 " bits=16 ch=1 bytes=%u\n", (unsigned)frames, rate_hz,
           (unsigned)total_bytes);
    fflush(stdout);

    for (size_t done = 0; done < total_bytes;) {
        const size_t n = (total_bytes - done < CHUNK_BYTES) ? (total_bytes - done) : CHUNK_BYTES;
        const uint8_t *src = (const uint8_t *)samples + done;

        crc = esp_rom_crc32_le(crc, src, (uint32_t)n);

        /* Cada linea lleva CUANTOS bytes de PCM trae. Nos costo una tanda de
           mediciones descubrir que a veces una linea salia truncada (el CDC del
           USB-Serial-JTAG comparte el canal con la consola): sin el 'n=' el host
           decodificaba la mitad del chunk y lo aceptaba como audio valido, así
           que una toma muda llegaba con picos de full-scale y el nodo la
           rechazaba. Con 'n=' el host tira el chunk malo y el hueco se ve en el
           crc, en vez de contaminar el audio. */
        const int pref_len = snprintf(pref, sizeof(pref), "KIWIAUDIO D n=%u ", (unsigned)n);
        memcpy(line, pref, (size_t)pref_len);
        const size_t chars = b64_encode(src, n, line + pref_len);
        line[pref_len + chars] = '\n';

        fwrite(line, 1, (size_t)pref_len + chars + 1, stdout);
        fflush(stdout);

        /* El CDC del USB-Serial-JTAG tiene un buffer TX corto; sin este respiro
           el host no alcanza a leer y se pierden bytes. */
        vTaskDelay(1);

        done += n;

        /* Un repaint por cada ~16 chunks: alcanza para la barra y no castiga el caudal. */
        if (on_progress && (++chunk % 16) == 0) {
            on_progress((int)(done * 100 / total_bytes));
        }
    }

    if (on_progress) {
        on_progress(100);
    }

    printf("KIWIAUDIO END crc32=%08" PRIx32 " bytes=%u\n", crc, (unsigned)total_bytes);
    fflush(stdout);

    s_stream_active = false;
    silencio_de_logs(false);
    ESP_LOGI(TAG, "drenado: %u bytes", (unsigned)total_bytes);
    return ESP_OK;
}

/* ---------------- modo en vivo ---------------- */

static struct {
    uint32_t crc;
    uint32_t bytes;
    uint32_t chunks;
} s_stream;

esp_err_t transfer_stream_begin(uint32_t rate_hz)
{
    s_stream.crc = 0;
    s_stream.bytes = 0;
    s_stream.chunks = 0;
    s_stream_active = true;

    printf("KIWIAUDIO BEGIN stream=1 frames=0 rate=%" PRIu32 " bits=16 ch=1 bytes=0\n", rate_hz);
    fflush(stdout);
    return ESP_OK;
}

esp_err_t transfer_stream_chunk(const int16_t *samples, size_t frames)
{
    /* 22 de prefijo (lleva el n=) + base64 + \n + 0. */
    char line[24 + ((TRANSFER_CHUNK_FRAMES * 2) / 3 + 1) * 4 + 2];
    const size_t n = frames * sizeof(int16_t);

    s_stream.crc = esp_rom_crc32_le(s_stream.crc, (const uint8_t *)samples, (uint32_t)n);

    /* n=<bytes de PCM> por linea: el host puede tirar un chunk que vengan
       corrupto (un log que se interpone) en vez de decodificar basura y
       desfasar toda la toma. */
    const int head = snprintf(line, sizeof(line), "KIWIAUDIO D n=%u ", (unsigned)n);
    const size_t chars = b64_encode((const uint8_t *)samples, n, line + head);
    line[head + chars] = '\n';

    fwrite(line, 1, (size_t)head + chars + 1, stdout);
    fflush(stdout);

    /* Un respiro por chunk: sin esto el buffer TX del USB-Serial-JTAG se llena y
       se pierden bytes (medido: 4 KB menos en 10 s de stream). Con 1 ms entre
       chunks el techo queda en ~500 KB/s, muy por encima de los 43 KB/s que
       demanda el audio en tiempo real. */
    vTaskDelay(1);

    s_stream.bytes += (uint32_t)n;
    s_stream.chunks++;
    return ESP_OK;
}

esp_err_t transfer_stream_end(uint32_t *out_bytes, uint32_t *out_crc32)
{
    printf("KIWIAUDIO END stream=1 crc32=%08" PRIx32 " bytes=%" PRIu32 " chunks=%" PRIu32 "\n", s_stream.crc,
           s_stream.bytes, s_stream.chunks);
    fflush(stdout);

    ESP_LOGI(TAG, "toma en vivo: %" PRIu32 " bytes en %" PRIu32 " chunks", s_stream.bytes, s_stream.chunks);

    if (out_bytes) {
        *out_bytes = s_stream.bytes;
    }
    if (out_crc32) {
        *out_crc32 = s_stream.crc;
    }
    s_stream_active = false;
    return ESP_OK;
}

/* ------- lo que la PC escribe hacia la placa ------- */

/* Linea entrante: un T con el texto completo del partial puede tener ~330
   caracteres de base64; el colchon es para no partir nunca una linea. */
#define IN_LINE_MAX 1200

static char s_in[IN_LINE_MAX];
static size_t s_in_len;
static bool s_in_checked;
static bool s_in_ok;

static int b64_val(char c)
{
    if (!c || c == '=') {
        return -1;
    }
    const char *p = strchr(B64, c);
    return p ? (int)(p - B64) : -1;
}

/* base64 -> texto; escribe a lo sumo cap-1 bytes y siempre cierra con 0. */
static size_t b64_decode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (const char *p = in; *p; p++) {
        const int v = b64_val(*p);
        if (v < 0) {
            continue;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < cap - 1) {
                out[o++] = (char)((acc >> bits) & 0xff);
            }
        }
    }
    out[o] = '\0';
    return o;
}

/* Version publica: la usan los comandos del REPL que reciben texto codificado
   (texto <base64>). Escribe a lo sumo cap-1 bytes, siempre cierra con 0. */
size_t transfer_b64_a_texto(const char *b64, char *out, size_t cap)
{
    if (!b64 || !out || cap == 0) {
        return 0;
    }
    return b64_decode(b64, out, cap);
}

static void handle_in_line(transfer_msg_t *msg)
{
    static const char PREFIX[] = "KIWIAUDIO ";

    if (s_in_len < sizeof(PREFIX) - 1 || memcmp(s_in, PREFIX, sizeof(PREFIX) - 1) != 0) {
        return; /* cualquier residuo del monitor: lo ignoramos */
    }

    const char *rest = s_in + sizeof(PREFIX) - 1;

    if (rest[0] == 'T' && rest[1] == ' ') {
        b64_decode(rest + 2, msg->text, sizeof(msg->text));
        /* Logueamos solo cuando no estamos mandando audio: un log a mitad de un
           chunk base64 desfasa toda la toma (ya nos paso). */
        if (!s_stream_active) {
            ESP_LOGI(TAG, "texto de la PC: %u chars", (unsigned)strlen(msg->text));
        }
    } else if (strcmp(rest, "GO") == 0) {
        msg->toggle = true;
    } else if (strcmp(rest, "GFX") == 0) {
        msg->dump = true;
    }
}

/*
    Leer de la PC: la early console (la del ROM) hace eco de lo que escribe el
    host y NUNCA lo entrega por stdin, por eso el "GO" y el texto nunca llegaban.

    Se prende el driver del USB-Serial-JTAG pero SOLO para el RX, y se lee con
    usb_serial_jtag_read_bytes(). A proposito NO se llama a
    usb_serial_jtag_vfs_use_driver(): medido en placa, pasar el TX por el VFS del
    driver cuesta bytes (una toma de 256000 salio con 252436 y crc mal), mientras
    que el camino del ROM con printf/fwrite viene andando perfecto desde siempre.
    O sea: salida por el ROM, entrada por el driver.
*/
static bool rx_del_driver_listo(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        cfg.rx_buffer_size = 2048;
        cfg.tx_buffer_size = 4096; /* no lo usamos para escribir, pero el driver lo pide > 0 */
        if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
            ESP_LOGW(TAG, "no se pudo prender el driver USB-Serial-JTAG: sin texto en el OLED");
            return false;
        }
    }
    return true;
}

bool transfer_poll(transfer_msg_t *msg)
{
    msg->text[0] = '\0';
    msg->toggle = false;
    msg->dump = false;

    if (!s_in_checked) {
        s_in_ok = rx_del_driver_listo();
        s_in_checked = true;
        ESP_LOGI(TAG, "lectura del host: %s", s_in_ok ? "ok" : "no disponible (sin texto en el OLED)");
    }
    if (!s_in_ok) {
        return false;
    }

    char raw[256];
    for (int lectura = 0; lectura < 8; lectura++) {
        /* Varias lecturas por llamada: la PC manda ~300 caracteres de golpe y si
           el anillo RX del USB-JTAG se llena se pierden caracteres del texto.
           ticks_to_wait=0 => nunca bloquea, no congela el loop de drenado. */
        const int n = usb_serial_jtag_read_bytes(raw, sizeof(raw), 0);
        if (n <= 0) {
            break;
        }

        for (int i = 0; i < n; i++) {
            const char c = raw[i];
            if (c == '\n' || c == '\r') {
                if (s_in_len) {
                    s_in[s_in_len] = '\0';
                    handle_in_line(msg);
                    s_in_len = 0;
                }
            } else if (s_in_len < IN_LINE_MAX - 1) {
                s_in[s_in_len++] = c;
            } else {
                s_in_len = 0; /* una linea kilometrica no la vamos a parsear */
            }
        }
    }

    return msg->text[0] || msg->toggle || msg->dump;
}

/* 96 bytes = 128 caracteres base64: multiplo de 3, asi solo el ultimo chunk
   puede llevar padding y el host concatena directo. */
#define GFX_CHUNK_BYTES 96

esp_err_t transfer_send_framebuffer(const uint8_t *fb, size_t len, int w, int h)
{
    static const char HEAD[] = "KIWIGFX D ";
    char line[sizeof(HEAD) + ((GFX_CHUNK_BYTES / 3) * 4) + 4];
    uint32_t crc = 0;

    if (!fb || !len) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Como con el audio: un log metido en el medio de un chunk base64 arruina
       la instantanea, asi que silenciamos hasta el END. */
    s_stream_active = true;
    silencio_de_logs(true);

    printf("KIWIGFX BEGIN w=%d h=%d bytes=%u\n", w, h, (unsigned)len);
    fflush(stdout);

    for (size_t done = 0; done < len;) {
        const size_t n = (len - done < GFX_CHUNK_BYTES) ? (len - done) : GFX_CHUNK_BYTES;

        crc = esp_rom_crc32_le(crc, fb + done, (uint32_t)n);

        memcpy(line, HEAD, sizeof(HEAD) - 1);
        const size_t chars = b64_encode(fb + done, n, line + sizeof(HEAD) - 1);
        line[chars + sizeof(HEAD) - 1] = '\n';

        /* Ojo con el largo: hay que incluir el '\n' que esta en line[chars+10],
           o sea sizeof(HEAD) y NO sizeof(HEAD)-1 (con eso las lineas salian
           pegadas y el parser nunca veia el bloque completo). */
        fwrite(line, 1, chars + sizeof(HEAD), stdout);
        fflush(stdout);

        vTaskDelay(1);
        done += n;
    }

    printf("KIWIGFX END crc32=%08" PRIx32 " bytes=%u\n", crc, (unsigned)len);
    fflush(stdout);

    s_stream_active = false;
    silencio_de_logs(false);
    return ESP_OK;
}
