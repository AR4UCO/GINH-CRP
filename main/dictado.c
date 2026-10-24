#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio.h"
#include "dictado.h"
#include "display.h"
#include "transfer.h"

static const char TAG[] = "dictado";

#define SAMPLE_RATE_HZ 16000

/* Techo de la toma en segundos (Kconfig). Con Wi-Fi + mbedTLS + ESP-NOW la heap
   interna anda mucho mas apretada que en el grabador solo, por eso el default
   del merge es mas corto y ademas recortamos a la mitad si no entra. */
#ifndef CONFIG_HACK_RECORD_SECONDS
#define CONFIG_HACK_RECORD_SECONDS 6
#endif

static bool s_audio_ok;
static bool s_pantalla_ok;
static int16_t *s_buf;
static size_t s_buf_frames;
static size_t s_frames_max;
static bool s_grabando;
static size_t s_ultimas_frames;

static char s_texto[256];
static unsigned s_texto_version;

/* El OLED: 128 px / 6 px por caracter = 21 columnas utiles. */
#define COLS 20
#define FILAS 3

static int16_t *allocar_buffer(size_t frames)
{
    const size_t bytes = frames * sizeof(int16_t);

    /* RAM interna primero: la PSRAM octal reclama GPIO 35/36/37 y el OLED est
       en 36/37 (se cuelga el sistema). Si algun dia se habilita PSRAM,
       sirve como respaldo para tomas largas. */
    int16_t *b = heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!b) {
        ESP_LOGW(TAG, "sin RAM interna para %" PRIu32 " bytes, probando PSRAM", (uint32_t)bytes);
        b = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    }
    return b;
}

/* Pinta el estado del dictado en el OLED. No intenta ser bonito: el mensajero
   ya tiene sus pantallas y aqui solo importa que se vea si esta grabando y que
   dijo la transcripcion. */
static void pintar(void)
{
    char fila[COLS + 1];

    if (!s_pantalla_ok) {
        return;
    }

    display_clear();
    display_text(2, 0, s_grabando ? "DICTADO GRABANDO" : "DICTADO LISTO", true);

    if (s_texto[0]) {
        /* Envuelve el texto en 3 filas de 20 caracteres, partiendo en espacios
           para no partir palabras (la fonte no tiene acentos ni eñe). */
        const char *p = s_texto;
        for (int f = 0; f < FILAS && *p; f++) {
            int n = 0;
            while (n < COLS && p[n]) {
                n++;
            }
            if (p[n]) {
                /* hay mas texto: retroceder hasta un espacio para no partir */
                int corte = n;
                while (corte > 0 && p[corte] != ' ') {
                    corte--;
                }
                if (corte == 0) {
                    corte = n;
                }
                n = corte;
            }
            memcpy(fila, p, n);
            fila[n] = '\0';
            display_text(2, 12 + f * 11, fila, false);
            p += n;
            while (*p == ' ') {
                p++;
            }
        }
    } else if (s_grabando) {
        display_text(2, 23, "habla ahora...", false);
    } else {
        display_text(2, 23, "decir 'grabar'", false);
    }

    snprintf(fila, sizeof(fila), "%.1fs %uKB", (float)s_ultimas_frames / SAMPLE_RATE_HZ,
             (unsigned)(s_ultimas_frames * 2 / 1024));
    display_text(2, 54, fila, false);
    display_flush();
}

void dictado_pantalla(bool disponible)
{
    s_pantalla_ok = disponible;
}

esp_err_t dictado_iniciar(void)
{
    s_audio_ok = (audio_init() == ESP_OK);
    if (!s_audio_ok) {
        ESP_LOGE(TAG, "sin micro: el dictado no va a andar");
        return ESP_FAIL;
    }

    /* La reserva sale aca, con la heap limpia. Si se deja para despues de
       prender Wi-Fi/ESP-NOW, TLS y la pila de red fragmentan la RAM interna y
       el bloque contiguo ya no aparece (medido: con 196 KB libres la toma de 6 s
       no entraba y caia a la mitad). */
    s_frames_max = (size_t)CONFIG_HACK_RECORD_SECONDS * SAMPLE_RATE_HZ;
    s_buf = allocar_buffer(s_frames_max);
    if (!s_buf) {
        s_frames_max /= 2;
        s_buf = allocar_buffer(s_frames_max);
        ESP_LOGW(TAG, "no entraba la toma de %d s, reservando %u s", CONFIG_HACK_RECORD_SECONDS,
                 (unsigned)(s_frames_max / SAMPLE_RATE_HZ));
    }
    if (!s_buf) {
        ESP_LOGE(TAG, "sin RAM para el dictado");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "dictado listo: toma de %u s (%" PRIu32 " KB reservados)",
             (unsigned)(s_frames_max / SAMPLE_RATE_HZ),
             (uint32_t)(s_frames_max * sizeof(int16_t) / 1024));
    return ESP_OK;
}

bool dictado_grabando(void)
{
    return s_grabando;
}

size_t dictado_frames(void)
{
    return s_grabando ? audio_record_done_frames() : s_ultimas_frames;
}

static void abrir(void)
{
    if (!s_audio_ok || s_grabando) {
        return;
    }

    /* No se puede arrancar una toma mientras sale la anterior por el puerto:
       las dos usan el MISMO buffer reservado, y la toma nueva pisaria los bytes
       que todavia se estan drenando (medido: la PC mandaba dos 'grabar' seguidos
       y la toma salia con huecos). */
    if (transfer_ocupada()) {
        ESP_LOGW(TAG, "todavia sale la toma anterior por la consola; esperar");
        return;
    }

    /* El buffer se reservo al arrancar (dictado_iniciar). Solo lo reintentamos
       si por algn motivo no esta ms. */
    if (!s_buf) {
        s_frames_max = (size_t)CONFIG_HACK_RECORD_SECONDS * SAMPLE_RATE_HZ;
        s_buf = allocar_buffer(s_frames_max);
        if (!s_buf) {
            s_frames_max /= 2;
            s_buf = allocar_buffer(s_frames_max);
        }
    }
    if (!s_buf) {
        ESP_LOGE(TAG, "no hay RAM para la toma");
        dictado_mostrar("");
        s_grabando = false;
        pintar();
        return;
    }

    const size_t frames = s_frames_max;
    if (audio_record_start(s_buf, frames) != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo arrancar la grabacion");
        return;
    }

    s_buf_frames = frames;
    s_grabando = true;
    ESP_LOGI(TAG, "grabando: techo %u s (%" PRIu32 " KB)", (unsigned)(frames / SAMPLE_RATE_HZ),
             (uint32_t)(frames * sizeof(int16_t) / 1024));
    ESP_LOGI(TAG, "RAM interna libre: %" PRIu32 " KB, bloque mayor %" PRIu32 " KB",
             (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
             (uint32_t)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
    pintar();
}

static void cerrar_y_drenar(void)
{
    if (!s_grabando) {
        return;
    }

    const size_t done = audio_record_done_frames();
    s_grabando = false;
    s_ultimas_frames = done;

    ESP_LOGI(TAG, "toma cerrada: %u frames (%.1f s), drenando por consola", (unsigned)done,
             (float)done / SAMPLE_RATE_HZ);
    pintar();

    if (s_buf && done) {
        transfer_send_pcm16(s_buf, done, SAMPLE_RATE_HZ, NULL);
    }

    /* NO se libera: es el bloque reservado al arrancar. Soltarlo significaria
       no poder volver a encontrar uno contiguo con la heap ya fragmentada. */
}

esp_err_t dictado_alternar(void)
{
    if (!s_audio_ok) {
        ESP_LOGW(TAG, "dictado_alternar sin micro");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_grabando) {
        audio_record_stop();
        cerrar_y_drenar();
    } else {
        abrir();
    }
    return ESP_OK;
}

void dictado_mantener(void)
{
    /* La toma se corta sola cuando se llena el buffer: ahi hay que drenar, si no
       el audio se queda tirado en RAM. */
    if (s_grabando && !audio_record_active()) {
        cerrar_y_drenar();
    }
}

void dictado_mostrar(const char *texto)
{
    if (!texto) {
        return;
    }

    /* La PC ya manda ASCII plegado, pero por las dudas: cualquier byte >= 0x80
       se suelta, porque la fonte 5x7 no lo tiene y desordena la lnea. */
    size_t o = 0;
    for (const char *p = texto; *p && o < sizeof(s_texto) - 1; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c >= 0x20 && c < 0x7f) {
            s_texto[o++] = (char)c;
        }
    }
    s_texto[o] = '\0';
    s_texto_version++;

    ESP_LOGI(TAG, "texto del dictado: %u chars (version %u)", (unsigned)o, s_texto_version);
    pintar();
}

/* Cada texto nuevo sube la version: la UI toma el texto una sola vez por
   version, asi no pisa lo que el usuario esta editando con el teclado. */
unsigned dictado_texto_version(void)
{
    return s_texto_version;
}

const char *dictado_texto(void)
{
    return s_texto;
}

/* Olvida el ultimo texto dictado. Se llama despues de enviar, para que el
   proximo mensaje no salga con el anterior pegado. NO sube la version: la UI
   no tiene que volver a sembrar el borrador. */
void dictado_texto_limpiar(void)
{
    s_texto[0] = '\0';
}

void dictado_resumen(char *out, size_t cap)
{
    snprintf(out, cap,
             "ESTADO audio=%d grabando=%d frames=%u ultimas=%u texto=%u heap=%u KB",
             s_audio_ok ? 1 : 0, s_grabando ? 1 : 0, (unsigned)dictado_frames(),
             (unsigned)s_ultimas_frames, (unsigned)strlen(s_texto),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
}
