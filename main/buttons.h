#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    BTN_UP,
    BTN_DOWN,
    BTN_LEFT,
    BTN_RIGHT,
    BTN_CENTER,
    BTN_KEY1,
    BTN_KEY2,
    BTN_KEY3,
    BTN_KEY4,
    BTN_COUNT,
} button_id_t;

typedef struct {
    button_id_t id;
    bool pressed;
} button_event_t;

esp_err_t buttons_init(void);

QueueHandle_t buttons_event_queue(void);

/* Estado ACTUAL de los botones (bit = apretado). Cada pulsacion queda "latcheada"
   LATCH_MS aunque se suelte antes, porque la UI y el teclado muestrean cada
   30-50 ms: con el estado puro, un toque rapido se perde y el boton 'no responde'
   (medido en la placa: toques cortos que no hacian nada). */
uint32_t buttons_read(void);

/* Giro del joystick, en pasos de 90 grados horarios. Se puede cambiar EN VIVO
   (comando 'joystick <0-3>' del REPL) y queda en NVS: cada placa puede venir con
   el joystick girado distinto, y reflashear para probar 0/1/2/3 cuesta 3 minutos. */
void buttons_set_giro(int pasos);
int buttons_giro(void);

const char *buttons_name(button_id_t id);

/* Que GPIO esta detras de cada boton. Sirve para el log del menu
   ('presion: UP (io 9)') y asi poder verificar el mapa del joystick contra el
   tablero sin adivinar (board.h tiene el joystick rotado 45 grados). */
int buttons_io(button_id_t id);

/* Inyecta una pulsacion 'sin dedos' (la usa el comando 'tecla' del REPL para
   poder probar el menu y el pager desde la PC). */
bool buttons_pulsar(button_id_t id);
/* Busca un boton por el nombre que usa el log ('KEY1', 'UP', 'CENTER'). */
button_id_t buttons_buscar(const char *nombre);
