#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define DISPLAY_FONT_W  6
#define DISPLAY_FONT_H  8

esp_err_t display_init(void);

void display_clear(void);

void display_set_pixel(int x, int y, bool on);
void display_fill_rect(int x, int y, int w, int h, bool on);
void display_draw_rect(int x, int y, int w, int h);

void display_text(int x, int y, const char *str, bool invert);

void display_line(int x0, int x1, int y0, int y1);

/*
    Instantanea del framebuffer para poder VER la pantalla desde la PC (sin
    camara): KIWIGFX la manda por consola y tools/pantalla_png.py la pasa a PNG.
    Layout: page-major, byte = x + (y/8)*W, bit = y%8 (bit0 arriba), tal cual lo
    escribe display_set_pixel().
*/
void display_snapshot(const uint8_t **fb, size_t *len, int *w, int *h);

esp_err_t display_flush(void);
