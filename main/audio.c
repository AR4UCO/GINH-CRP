#include <math.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "audio.h"
#include "board.h"

/*  
    Todo esto esta robado de la docu, para entenderlo bien van a querer leer
    https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/i2s.html
    Se que es medio gd decirles RTFM pero hay posta demasiadas cosas pasando 
    y no soy bueno explicando.
    La docu tiene muy buenos ejemplos.
*/

static const char *TAG = "audio";

#define SAMPLE_RATE_HZ      16000

#define TONE_CHUNK_FRAMES   256
#define MIC_CHUNK_FRAMES    256

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;

static volatile int s_mic_level;

/* Estado de la grabacion: lo escribe la mic_task, lo consulta main.c */
static struct {
    int16_t *dst;
    size_t total;
    volatile size_t done;
    volatile bool active;
} s_rec;

/*
    Captura en vivo: ring buffer SPSC (un productor, un consumidor).
    Lo escribe la mic_task y lo vacía main.c hacia el puerto serie.
    8192 frames = 512 ms de colchón: si el USB o el loop de la PC se resienten
    un rato no se pierde habla.

    Unico productor (mic_task) avanza head, unico consumidor (main) avanza tail:
    asi no hace falta lock y nadie escribe el indice del otro.
*/
#define STREAM_RING_FRAMES 8192u
#define STREAM_RING_MASK   (STREAM_RING_FRAMES - 1)

static int16_t s_ring[STREAM_RING_FRAMES];
static volatile size_t s_ring_head;
static volatile size_t s_ring_tail;
static volatile size_t s_stream_dropped;
static volatile bool s_streaming;

static esp_err_t speaker_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);

    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "new tx channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_SPK_BCLK_IO,
            .ws = BOARD_SPK_LRCLK_IO,
            .dout = BOARD_SPK_DIN_IO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "tx init failed");
    return i2s_channel_enable(s_tx);
}

static esp_err_t mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "new rx channel failed");

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .clk = BOARD_MIC_CLK_IO,
            .din = BOARD_MIC_DOUT_IO,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_rx, &pdm_cfg), TAG, "rx init failed");
    return i2s_channel_enable(s_rx);
}

/* Ganancia digital de la toma. La kiwi board no trae control de ganancia por
   hardware en el mic, y medido en una habitacion muda el pico de ruido es
   -42 dBFS: con voz bajita el nodo ASR responde rejected/no_speech. El ajuste se
   hace aca, con saturacion, en vez de tocar el filtro PDM. Se deja en Kconfig
   (HACK_MIC_GAIN) para poder subirlo a la mañana sin reescribir codigo. No pasa
   de 8: el nodo tiene VAD y amplificar el ruido de fondo hace que INVENTE texto. */
#ifndef CONFIG_HACK_MIC_GAIN
#define CONFIG_HACK_MIC_GAIN 1
#endif

static inline int16_t mezcla_mono(int16_t izq, int16_t der)
{
    int32_t v = ((int32_t)izq + (int32_t)der) / 2;
    if (CONFIG_HACK_MIC_GAIN > 1) {
        v *= CONFIG_HACK_MIC_GAIN;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
    }
    return (int16_t)v;
}

static void mic_task(void *arg)
{
    int16_t buf[MIC_CHUNK_FRAMES * 2];
    float smoothed = 0;

    while (1) {
        size_t bytes = 0;
        if (i2s_channel_read(s_rx, buf, sizeof(buf), &bytes, 1000) != ESP_OK) {
            continue;
        }
        int frames = bytes / (2 * sizeof(int16_t));
        if (frames == 0) {
            continue;
        }

        if (s_rec.active) {
            const size_t remaining = s_rec.total - s_rec.done;
            const size_t copy = ((size_t)frames < remaining) ? (size_t)frames : remaining;
            int16_t *out = s_rec.dst + s_rec.done;

            for (size_t i = 0; i < copy; i++) {
                /* La PDM entrega estereo: promedio L/R para quedar en mono. */
                out[i] = mezcla_mono(buf[i * 2], buf[i * 2 + 1]);
            }
            s_rec.done += copy;
            if (s_rec.done >= s_rec.total) {
                s_rec.active = false;
            }
        }

        if (s_streaming) {
            /* El stream y la toma cerrada no se usan juntos, pero pueden
               coexistir: cada uno tiene su cola. */
            for (int i = 0; i < frames; i++) {
                const size_t head = s_ring_head;
                if (head - s_ring_tail >= STREAM_RING_FRAMES) {
                    /* El drenado va mas lento que el micro: descartamos el chunk
                       nuevo (y lo contamos) antes de pisar lo que el host noch
                       no leyó. */
                    s_stream_dropped++;
                    break;
                }
                s_ring[head & STREAM_RING_MASK] = mezcla_mono(buf[i * 2], buf[i * 2 + 1]);
                s_ring_head = head + 1;
            }
        }

        float best_rms = 0;
        for (int ch = 0; ch < 2; ch++) {
            int64_t sum = 0, sum_sq = 0;
            for (int i = 0; i < frames; i++) {
                int v = buf[i * 2 + ch];
                sum += v;
                sum_sq += (int64_t)v * v;
            }
            float mean = (float)sum / frames;
            float var = (float)sum_sq / frames - mean * mean;
            float rms = var > 0 ? sqrtf(var) : 0;
            if (rms > best_rms) {
                best_rms = rms;
            }
        }

        float level = (20.0f * log10f(best_rms + 1.0f) - 20.0f) * (100.0f / 60.0f);
        level = level < 0 ? 0 : (level > 100 ? 100 : level);

        smoothed = level > smoothed ? level : smoothed * 0.8f + level * 0.2f;
        s_mic_level = (int)(smoothed + 0.5f);
    }
}

esp_err_t audio_init(void)
{
    ESP_RETURN_ON_ERROR(speaker_init(), TAG, "speaker init failed");
    ESP_RETURN_ON_ERROR(mic_init(), TAG, "mic init failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(mic_task, "mic", 4096, NULL, 5, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "mic task create failed");
    ESP_LOGI(TAG, "speaker (I2S1) and microphone (PDM, I2S0) ready; ganancia x%d",
             CONFIG_HACK_MIC_GAIN);
    return ESP_OK;
}

int audio_mic_level(void)
{
    return s_mic_level;
}

esp_err_t audio_record_start(int16_t *dst, size_t frames)
{
    ESP_RETURN_ON_FALSE(dst && frames, ESP_ERR_INVALID_ARG, TAG, "buffer vacío");
    ESP_RETURN_ON_FALSE(s_rx, ESP_ERR_INVALID_STATE, TAG, "mic sin inicializar");
    ESP_RETURN_ON_FALSE(!s_rec.active, ESP_ERR_INVALID_STATE, TAG, "ya grabando");

    s_rec.dst = dst;
    s_rec.total = frames;
    s_rec.done = 0;
    s_rec.active = true;
    return ESP_OK;
}

bool audio_record_active(void)
{
    return s_rec.active;
}

void audio_record_stop(void)
{
    s_rec.active = false;
}

size_t audio_record_done_frames(void)
{
    return s_rec.done;
}

esp_err_t audio_stream_start(void)
{
    ESP_RETURN_ON_FALSE(s_rx, ESP_ERR_INVALID_STATE, TAG, "mic sin inicializar");
    ESP_RETURN_ON_FALSE(!s_streaming, ESP_ERR_INVALID_STATE, TAG, "ya escuchando");

    s_ring_head = 0;
    s_ring_tail = 0;
    s_stream_dropped = 0;
    s_streaming = true;
    return ESP_OK;
}

void audio_stream_stop(void)
{
    s_streaming = false;
}

bool audio_stream_active(void)
{
    return s_streaming;
}

size_t audio_stream_read(int16_t *dst, size_t max_frames)
{
    size_t n = 0;
    const size_t head = s_ring_head;

    while (n < max_frames && s_ring_tail < head) {
        dst[n++] = s_ring[s_ring_tail & STREAM_RING_MASK];
        s_ring_tail++;
    }
    return n;
}

size_t audio_stream_dropped_frames(void)
{
    return s_stream_dropped;
}

esp_err_t audio_play_tone(int freq_hz, int duration_ms)
{
    const int total = SAMPLE_RATE_HZ * duration_ms / 1000;

    const int fade = SAMPLE_RATE_HZ / 200;

    const float amp = 32767.0f * CONFIG_HACK_SPEAKER_VOLUME / 100.0f;
    const float step = 2.0f * (float)M_PI * freq_hz / SAMPLE_RATE_HZ;

    int16_t chunk[TONE_CHUNK_FRAMES * 2];
    float phase = 0;

    for (int done = 0; done < total;) {
        int n = total - done < TONE_CHUNK_FRAMES ? total - done : TONE_CHUNK_FRAMES;
        for (int i = 0; i < n; i++) {
            int pos = done + i;
            float env = 1.0f;
            if (pos < fade) {
                env = (float)pos / fade;
            } else if (total - pos < fade) {
                env = (float)(total - pos) / fade;
            }
            int16_t s = (int16_t)(sinf(phase) * amp * env);
            chunk[i * 2] = s;
            chunk[i * 2 + 1] = s;

            phase += step;
            if (phase > 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }
        size_t written = 0;
        ESP_RETURN_ON_ERROR(i2s_channel_write(s_tx, chunk, n * 2 * sizeof(int16_t), &written, 1000), TAG,
                            "tone write failed");
        done += n;
    }
    return ESP_OK;
}
