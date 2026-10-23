#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "board.h"
#include "buttons.h"
#include "display.h"

char letters[] = "abcdefghijklmnopqrstuvwxyz";
char caps[] =    "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
char symb[] =    "!\"#$%&'()*+,-./:;<=>?@[]\\^_`{|}~";
char numb[] =    "0123456789";

#define SIDEBAR_WIDTH       26
#define KEYBOARD_WIDTH      (BOARD_LCD_WIDTH - SIDEBAR_WIDTH)
#define CHARS_SHOWN   7

char read_char(void);

static char *charsets[] = {letters, caps, symb, numb};
static int current_charset_id;
static char *res;

static void draw_text_buf(int x, int y, const char *s, int n, bool invert)
{
    char buf[32];
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;
    }
    memcpy(buf, s, n);
    buf[n] = '\0';
    display_text(x, y, buf, invert);
}

static void draw_chunk(int dir, const char *s, int n)
{
    if (n <= 0) {
        return;
    }

    switch (dir) {
    case 0: { // up
        const int w = n * DISPLAY_FONT_W;
        draw_text_buf((KEYBOARD_WIDTH - w) / 2, 1, s, n, false);
        break;
    }
    case 2: { // down
        const int w = n * DISPLAY_FONT_W;
        draw_text_buf((KEYBOARD_WIDTH - w) / 2, BOARD_LCD_HEIGHT - DISPLAY_FONT_H - 1, s, n, false);
        break;
    }
    default: { // left (3) / right (1), stacked in a small grid
        const int cols = n > 4 ? 4 : n;
        const int rows = (n + cols - 1) / cols;
        const int w = cols * DISPLAY_FONT_W;
        const int h = rows * DISPLAY_FONT_H;
        const int x = dir == 1 ? KEYBOARD_WIDTH - w - 2 : 2;
        const int y = (BOARD_LCD_HEIGHT - h) / 2;
        for (int i = 0; i < n; i++) {
            draw_text_buf(x + (i % cols) * DISPLAY_FONT_W, y + (i / cols) * DISPLAY_FONT_H, &s[i], 1, false);
        }
        break;
    }
    }
}

static void draw_screen(const char *charset, int lo, int hi)
{
    display_clear();

    //diagonales
    display_line(0, KEYBOARD_WIDTH - 1, 0, BOARD_LCD_HEIGHT - 1);
    display_line(0, KEYBOARD_WIDTH - 1, BOARD_LCD_HEIGHT - 1, 0);
    //separador
    for (int y = 0; y < BOARD_LCD_HEIGHT; y++) {
        display_set_pixel(KEYBOARD_WIDTH, y, true);
    }

    const int line_len = res == NULL ? 0 : (int)strlen(res);
    const int shown = line_len < CHARS_SHOWN ? line_len : CHARS_SHOWN;
    const int first = line_len - shown;
    for (int i = 0; i < shown; i++) {
        char c = res[first + i];
        if (c == ' ') {
            c = '_';
        }
        draw_text_buf(KEYBOARD_WIDTH + (SIDEBAR_WIDTH - DISPLAY_FONT_W) / 2, 4 + i * DISPLAY_FONT_H, &c, 1, false);
    }

    const int len = hi - lo + 1;
    if (len == 1) {
        draw_text_buf((KEYBOARD_WIDTH - DISPLAY_FONT_W) / 2, (BOARD_LCD_HEIGHT - DISPLAY_FONT_H) / 2, &charset[lo], 1, true);
    } else {
        const int q = (len + 3) / 4;
        const int chunks = (len + q - 1) / q;
        for (int i = 0; i < chunks; i++) {
            const int start = lo + i * q;
            int n = hi - start + 1;
            if (n > q) {
                n = q;
            }
            draw_chunk(i, &charset[start], n);
        }
    }

    display_flush();
}

char* readline(){
    int ptr = 0;
    int size = 64;
    res = malloc(size);
    if (res == NULL) {
        return NULL;
    }
    res[0] = '\0';

    while (true){
        const char c = read_char();
        if (c == '\b') {
            if (ptr > 0) {
                ptr--;
            }
            res[ptr] = '\0';
            continue;
        }
        if (c == '\n') {
            break;
        }
        if (ptr + 1 >= size) {
            char *grown = realloc(res, size * 2);
            if (grown == NULL) {
                break;
            }
            res = grown;
            size *= 2;
        }
        res[ptr++] = c;
        res[ptr] = '\0';
    }
    return res;
}

static int quarter_for_press(uint32_t new_press)
{
    if (new_press & (1u << BTN_UP)) {
        return 0;
    }
    if (new_press & (1u << BTN_RIGHT)) {
        return 1;
    }
    if (new_press & (1u << BTN_DOWN)) {
        return 2;
    }
    if (new_press & (1u << BTN_LEFT)) {
        return 3;
    }
    return -1;
}

char read_char(void){
    const char *charset = charsets[current_charset_id];
    int lo = 0;
    int hi = (int)strlen(charset) - 1;
    uint32_t prev_pressed = buttons_read();

    while (true) {
        const uint32_t pressed = buttons_read();
        const uint32_t new_press = pressed & ~prev_pressed;
        prev_pressed = pressed;

        if (new_press & (1u << BTN_KEY1)) {
            current_charset_id = (current_charset_id + 1) % 4;
            charset = charsets[current_charset_id];
            lo = 0;
            hi = (int)strlen(charset) - 1;
            continue;
        }
        if (new_press & (1u << BTN_KEY2)) {
            return ' ';
        }
        if (new_press & (1u << BTN_KEY3)) {
            return '\b';
        }
        if (new_press & (1u << BTN_KEY4)) {
            return '\n';
        }

        const int len = hi - lo + 1;
        if (len == 1) {
            draw_screen(charset, lo, hi);
            vTaskDelay(pdMS_TO_TICKS(200));
            return charset[lo];
        }

        const int quarter_size = (len + 3) / 4;
        const int chunks = (len + quarter_size - 1) / quarter_size;
        const int sel = quarter_for_press(new_press);

        if (sel >= 0 && sel < chunks) {
            const int new_lo = lo + sel * quarter_size;
            int new_hi = new_lo + quarter_size - 1;
            if (new_hi > hi) {
                new_hi = hi;
            }
            lo = new_lo;
            hi = new_hi;
            continue;
        }

        draw_screen(charset, lo, hi);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
