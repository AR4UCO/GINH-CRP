#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "buttons.h"
#include "display.h"

char letters[] = "abcdefghijklmnopqrstuvwxyz";
char caps[] =    "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
char symb[] =    "!\"#$%&'()*+,-./:;<=>?@[]\\^_`{|}~";
char numb[] =    "0123456789";

#define SIDEBAR_WIDTH  26
#define KEYBOARD_WIDTH (BOARD_LCD_WIDTH - SIDEBAR_WIDTH)
#define QUARTER_WIDTH  (KEYBOARD_WIDTH / 2)
#define QUARTER_HEIGHT (BOARD_LCD_HEIGHT / 2)
#define CHARS_SHOWN    7

char read_char(void);

static char *charsets[] = {letters, caps, symb, numb};
static int current_charset_id;
static char *res;

static int min(int a, int b){
    return a > b ? b : a;
}

static void draw_text_buf(int x, int y, const char *s, int n, bool invert)
{
    char buf[QUARTER_WIDTH / DISPLAY_FONT_W + 1];
    memcpy(buf, s, n);
    buf[n] = '\0';
    display_text(x, y, buf, invert);
}

static void draw_quadrant(int quadrant, const char *s, int n)
{
    if (n <= 0) {
        return;
    }

    const int right = quadrant == 1 || quadrant == 2;
    const int bottom = quadrant >= 2;
    const int x = right ? QUARTER_WIDTH : 0;
    const int y = bottom ? QUARTER_HEIGHT : 0;
    const int w = n * DISPLAY_FONT_W;

    draw_text_buf(x + (QUARTER_WIDTH - w) / 2, y + (QUARTER_HEIGHT - DISPLAY_FONT_H) / 2, s, n, false);
}

static int quarter_offset(int range, int i)
{
    const int base = range / 4;
    const int extra = range % 4;
    return i * base + (i < extra ? i : extra);
}

static int quarter_size_at(int range, int i)
{
    return range / 4 + (i < range % 4 ? 1 : 0);
}

static void draw_screen(const char *charset, int lo, int hi)
{
    display_clear();

    display_fill_rect(0, QUARTER_HEIGHT, KEYBOARD_WIDTH, 1, true);
    display_fill_rect(QUARTER_WIDTH, 0, 1, BOARD_LCD_HEIGHT, true);
    display_fill_rect(KEYBOARD_WIDTH, 0, 1, BOARD_LCD_HEIGHT, true);

    const int line_len = (int)strlen(res);
    const int shown = min(line_len, CHARS_SHOWN);
    const int first = line_len - shown;
    for (int i = 0; i < shown; i++) {
        char c = res[first + i];
        if (c == ' ') {
            c = '_';
        }
        draw_text_buf(KEYBOARD_WIDTH + (SIDEBAR_WIDTH - DISPLAY_FONT_W) / 2, 4 + i * DISPLAY_FONT_H, &c, 1, false);
    }

    const int range = hi - lo + 1;
    if (range == 1) {
        draw_text_buf((KEYBOARD_WIDTH - DISPLAY_FONT_W) / 2, (BOARD_LCD_HEIGHT - DISPLAY_FONT_H) / 2, &charset[lo], 1, true);
    } else {
        const int quadrants = min(4, range);
        for (int i = 0; i < quadrants; i++) {
            draw_quadrant(i, &charset[lo + quarter_offset(range, i)], quarter_size_at(range, i));
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

static int joystick_dir_pressed(uint32_t new_press)
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

        const int range = hi - lo + 1;
        if (range == 1) {
            draw_screen(charset, lo, hi);
            vTaskDelay(pdMS_TO_TICKS(200));
            return charset[lo];
        }

        const int quadrants = min(4,range);
        const int selected = joystick_dir_pressed(new_press);

        if (selected >= 0 && selected < quadrants) {
            lo = lo + quarter_offset(range, selected);
            hi = lo + quarter_size_at(range, selected) - 1;
            continue;
        }

        draw_screen(charset, lo, hi);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
