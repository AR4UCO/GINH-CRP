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

static void draw_text_centered(int x,int y,const char *s){
    display_text(x-strlen(s)*DISPLAY_FONT_W/2, y-DISPLAY_FONT_H/2, s, false);
}


static void draw_screen(const char choices[4][9])
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
        const char s[2] = {c, '\0'};
        draw_text_centered(KEYBOARD_WIDTH + SIDEBAR_WIDTH / 2, 8 + i * DISPLAY_FONT_H, s);
    }

    if (choices[1][0]=='\0') {
        draw_text_centered(KEYBOARD_WIDTH/2, BOARD_LCD_HEIGHT/2, choices[0]);
    } else {
        draw_text_centered(KEYBOARD_WIDTH/4, BOARD_LCD_HEIGHT/4, choices[0]);
        draw_text_centered(KEYBOARD_WIDTH*3/4, BOARD_LCD_HEIGHT/4, choices[1]);
        draw_text_centered(KEYBOARD_WIDTH/4, BOARD_LCD_HEIGHT*3/4, choices[3]);
        draw_text_centered(KEYBOARD_WIDTH*3/4, BOARD_LCD_HEIGHT*3/4, choices[2]);
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

static void split_charset(const char *charset, char choices[4][9]){
    const int len = strlen(charset);
    const int base = len / 4;
    const int extra = len % 4;
    int offset = 0;
    for (int i = 0; i < 4; i++){
        const int newlen = base + (i < extra ? 1 : 0);
        memcpy(choices[i], charset + offset, newlen);
        choices[i][newlen] = '\0';
        offset += newlen;
    }
}

char read_char(void){
    // menos eficiente que usando indices high y low pero el str es chico 
    // y el código quedaba horrible sino

    char current[33];
    char choices[4][9];
    strcpy(current, charsets[current_charset_id]);
    split_charset(current, choices);

    uint32_t prev_pressed = buttons_read();

    while (true) {
        const uint32_t pressed = buttons_read();
        const uint32_t new_press = pressed & ~prev_pressed;
        prev_pressed = pressed;

        if (new_press & (1u << BTN_KEY1)) {
            current_charset_id = (current_charset_id + 1) % 4;
            strcpy(current, charsets[current_charset_id]);
            split_charset(current, choices);
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

        const int range = strlen(current);
        if (range == 1) {
            draw_screen(choices);
            vTaskDelay(pdMS_TO_TICKS(200));
            return current[0];
        }

        const int quadrants = min(4, range);
        const int selected = joystick_dir_pressed(new_press);

        if (selected < 0 || selected >= quadrants) {
            draw_screen(choices);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        strcpy(current, choices[selected]);
        split_charset(current, choices);
    }
}
