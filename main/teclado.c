#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "buttons.h"
#include "nvs.h"
#include "dictado.h"
#include "display.h"
#include "inbox.h"
#include "esp_log.h"

char letters[] = "abcdefghijklmnopqrstuvwxyz";
char caps[] =    "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
char symb[] =    "!\"#$%&'()*+,-./:;<=>?@[]\\^_`{|}~";
char numb[] =    "0123456789";

#define SIDEBAR_WIDTH  26
#define KEYBOARD_WIDTH (BOARD_LCD_WIDTH - SIDEBAR_WIDTH)
#define QUARTER_WIDTH  (KEYBOARD_WIDTH / 2)
#define QUARTER_HEIGHT (BOARD_LCD_HEIGHT / 2)
#define CHARS_SHOWN    7

/* Centinela de read_char: el boton del medio del joystick pide salir del teclado
   sin enviar. No es un caracter imprimible, asi que no puede colisionar. */
#define TECLADO_SALIR  '\x01'

char read_char(void);
char *readline_seed(const char *seed);

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

char *readline(void)
{
    return readline_seed("");
}

char *readline_seed(const char *seed)
{
    int ptr = 0;
    int size = 64;
    res = malloc(size);
    if (res == NULL) {
        return NULL;
    }
    res[0] = '\0';

    /* Semilla: solo caracteres imprimibles ASCII, que es lo que la fonte 5x7
       puede dibujar. Un byte >= 0x80 (acento UTF-8 del dictado) meteria un
       glifo basura en el cuadrante. */
    if (seed) {
        for (const char *p = seed; *p; p++) {
            const unsigned char c = (unsigned char)*p;
            if (c >= 0x20 && c < 0x7f) {
                if (ptr + 1 >= size) {
                    char *grown = realloc(res, size * 2);
                    if (!grown) {
                        break;
                    }
                    res = grown;
                    size *= 2;
                }
                res[ptr++] = (char)c;
                res[ptr] = '\0';
            }
        }
    }

    while (true){
        const char c = read_char();
        if (c == TECLADO_SALIR) {
            /* Salir sin guardar: el llamador (ui.c) deja el borrador como estaba. */
            return NULL;
        }
        if (c == '\b') {
            if (ptr > 0) {
                ptr--;
            }
            res[ptr] = '\0';
            continue;
        }
        if (c == '\n') {
            /* SW4 con el mensaje VACIO = para atras, no enviar. Era lo unico que
               habia para salir y confundia (los dos casos se veian iguales). */
            if (ptr == 0 && !res[0]) {
                return NULL;
            }
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

/* Geometria real (leida, no supuesta): draw_quadrant() pinta con
   right = (q==1 || q==2) y bottom = (q>=2), o sea 0=arriba-izq, 1=arriba-der,
   2=ABAJO-DER, 3=ABAJO-IZQ. Con eso, el mapa de abajo es el coherente con la
   pantalla: cada direccion del joystick toma el cuadrante que esta de ese lado
   en diagonal. Con el set completo (26 letras) eso es:
     arriba-izq: abcdefg   arriba-der: hijklmn
     abajo-izq:  uvwxyz    abajo-der:  opqrst

 MEDIDO con los dedos del usuario (24/10 09:1x): en sus placas el joystick esta
 corrido un cuadrante ('el movimiento que deberia seleccionar a selecciona h,
 el de h la o, el de o la u, el de u la a'), o sea un giro horario de uno. Por
 eso el desplazamiento se puede ajustar en el REPL con 'teclas <0-3>' y queda en
 NVS ('kbd.giro'): NO es lo mismo que 'joystick <0-3>', que gira el driver y
 tambien mueve la navegacion del menu. El default (3) es el valor que deja el
 mapa como lo ve el usuario; si alguna placa viene girada al reves, se cambia sin
 reflashear. */
static int s_giro_teclado = -1;

static int giro_teclado(void)
{
    if (s_giro_teclado < 0) {
        int32_t v = 3;
        nvs_handle_t h;
        if (nvs_open("app", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_i32(h, "kbd.giro", &v);
            nvs_close(h);
        }
        s_giro_teclado = ((v % 4) + 4) % 4;
        ESP_LOGI("teclado", "giro del teclado: %d (up->cuadrante %d)", s_giro_teclado,
                 s_giro_teclado);
    }
    return s_giro_teclado;
}

void teclado_set_giro(int pasos)
{
    s_giro_teclado = ((pasos % 4) + 4) % 4;
    nvs_handle_t h;
    if (nvs_open("app", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "kbd.giro", s_giro_teclado);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI("teclado", "giro del teclado ahora: %d", s_giro_teclado);
}

int teclado_giro(void)
{
    return giro_teclado();
}

static int joystick_dir_pressed(uint32_t new_press)
{
    int direccion = -1;
    if (new_press & (1u << BTN_UP)) {
        direccion = 0;
    } else if (new_press & (1u << BTN_RIGHT)) {
        direccion = 1;
    } else if (new_press & (1u << BTN_DOWN)) {
        direccion = 2;
    } else if (new_press & (1u << BTN_LEFT)) {
        direccion = 3;
    }
    if (direccion < 0) {
        return -1;
    }
    return (direccion + giro_teclado()) & 3;
}

/* Aviso de mensaje recibido: se muestra encima del teclado y vuelve
 * solo a los 4 s o al presionar cualquier tecla. */
#define MSG_COLS  21
#define MSG_LINES 5

static void show_incoming(const char *sender, const char *text)
{
    display_clear();
    display_text(0, 0, "Mensaje de:", false);
    display_text(0, 10, sender, true);

    int y = 22;
    int col = 0;
    char line[MSG_COLS + 1];

    for (const char *p = text; *p && y < 22 + MSG_LINES * DISPLAY_FONT_H; p++) {
        line[col++] = *p;
        if (col == MSG_COLS || p[1] == '\0') {
            line[col] = '\0';
            display_text(0, y, line, false);
            y += DISPLAY_FONT_H;
            col = 0;
        }
    }
    display_flush();

    uint32_t prev = buttons_read();
    TickType_t start = xTaskGetTickCount();
    while (xTaskGetTickCount() - start < pdMS_TO_TICKS(4000)) {
        const uint32_t now = buttons_read();
        if ((now & ~prev) != 0) {
            break;
        }
        prev = now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Tecla KEY1: un toque corta cambiar el juego de caracteres (lo que ya hac
   el teclado del grupo); mantenerla 700 ms o mas abre la escucha del dictado,
   y volver a mantenerla la cierra y manda la toma a la PC.

   Se hace aca y no en un boton aparte porque los 4 botones ya estan tomados
   por el teclado y el joystick por la navegacion de cuadrantes. Devuelve true
   si el dictado tomo el toque, para que read_char no lo interprete como tecla. */
static bool key1_largo_como_dictado(uint32_t *prev_pressed)
{
    TickType_t desde = xTaskGetTickCount();

    /* Mientras siga apretada, contamos cuanto. Si se suelta antes del umbral,
       no fue dictado: dejo que read_char haga el cambio de juego. */
    while (xTaskGetTickCount() - desde < pdMS_TO_TICKS(700)) {
        vTaskDelay(pdMS_TO_TICKS(20));
        const uint32_t ahora = buttons_read();
        if (!(ahora & (1u << BTN_KEY1))) {
            *prev_pressed = ahora;
            return false;
        }
    }

    ESP_LOGI("teclado", "KEY1 largo: dictado (abre o cierra la escucha)");
    dictado_alternar();

    /* Esperamos a que suelte, para no soltar la tecla a mitad. */
    while (buttons_read() & (1u << BTN_KEY1)) {
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    *prev_pressed = buttons_read();
    return true;
}

char read_char(void){
    const char *charset = charsets[current_charset_id];
    int lo = 0;
    int hi = (int)strlen(charset) - 1;
    uint32_t prev_pressed = buttons_read();

    /* El teclado pinta solo cuando algo cambio. Antes repintaba cada 20 ms
       aunque estuviera quieto, y eso borraba cualquier cosa que se quisiera
       mostrar encima (la pantalla del dictado, por ejemplo). Ademas es traffic
       SPI de mas para un OLED que solo cambia cuando el usuario elige. */
    bool redibujar = true;

    while (true) {
        char in_sender[INBOX_SENDER_MAX];
        char in_text[INBOX_TEXT_MAX];
        if (inbox_take(in_sender, sizeof(in_sender), in_text, sizeof(in_text))) {
            show_incoming(in_sender, in_text);
            prev_pressed = buttons_read(); // no inyectar la tecla que cierra el aviso
            draw_screen(charset, lo, hi);
            redibujar = false;
            continue;
        }

        const uint32_t pressed = buttons_read();
        const uint32_t new_press = pressed & ~prev_pressed;
        prev_pressed = pressed;

        if (new_press & (1u << BTN_KEY1)) {
            if (key1_largo_como_dictado(&prev_pressed)) {
                /* A proposito NO repintamos el teclado: si lo hacemos tapamos el
                   overlay del dictado que acaba de pintar dictado_alternar. El
                   teclado vuelve solo cuando se mueva algo. */
                continue;
            }
            current_charset_id = (current_charset_id + 1) % 4;
            charset = charsets[current_charset_id];
            lo = 0;
            hi = (int)strlen(charset) - 1;
            redibujar = true;
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
        /* El boton del MEDIO del joystick no lo usaba nadie: es la salida del
           teclado sin tener que enviar. readline_seed lo traduce a NULL y el menu
           conserva el borrador que habia antes de entrar. */
        if (new_press & (1u << BTN_CENTER)) {
            return TECLADO_SALIR;
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
            redibujar = true;
            continue;
        }

        if (redibujar) {
            draw_screen(charset, lo, hi);
            redibujar = false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
