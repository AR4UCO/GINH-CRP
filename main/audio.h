#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t audio_init(void);

int audio_mic_level(void);

esp_err_t audio_play_tone(int freq_hz, int duration_ms);

/*
    Grabacion: la PDM RX ya la usa la task del medidor de nivel, asi que en vez
    de abrir un segundo lector (que se pelearia con el DMA) la propia task baja
    mix a mono y llena el buffer que le pases aca.

   Frames en mono a SAMPLE_RATE_HZ (16 kHz), 16 bit.
*/
esp_err_t audio_record_start(int16_t *dst, size_t frames);

bool audio_record_active(void);

size_t audio_record_done_frames(void);

/* Corta la grabacion antes del maximo: lo que ya esta en el buffer queda valido
   y audio_record_done_frames() dice cuantas frames son. */
void audio_record_stop(void);

/*
    Captura en vivo: la mic_task escribe en un ring buffer yvos lo vas sacando
    con audio_stream_read() apenas se va llenando, asi el audio puede salir por
    el puerto a medida que se graba (transcripcion en tiempo real) en vez de
    esperar a que termine la toma.

    No hay largo total: la toma la cerrás vos con audio_stream_stop().
*/
esp_err_t audio_stream_start(void);

void audio_stream_stop(void);

bool audio_stream_active(void);

/* Copia hasta max_frames mono al buffer que pases; devuelve las frames copiadas. */
size_t audio_stream_read(int16_t *dst, size_t max_frames);

/* Frames que se perdieron por no vaciar la cola a tiempo. */
size_t audio_stream_dropped_frames(void);
