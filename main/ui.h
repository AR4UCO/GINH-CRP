/*
 * Menu del mensajero (la UX de la rama graficos + el dictado + la cripto).
 *
 * La rama graficos tenia tres pantallas cíclicas (IDLE / CONFIG / COMPOSE) con
 * dos TODOs abiertos: inicializar ESP-NOW y mandar el mensaje. Acá esa misma
 * máquina de estados corre sobre el firmware mergiado, entonces:
 *
 *   - IDLE     muestra lo que la cripto ya sabe: nick propio, cantidad de
 *              sesiones seguras y la huella del par (TOFU a la vista).
 *   - CONFIG   alias + MAC + huella; SW1 edita el alias con el teclado radial.
 *   - COMPOSE  borrador del mensaje. SW1 teclado radial, SW2 dictado rápido,
 *              SW4 envía cifrado (esa es la respuesta al TODO de esp_now_send).
 *   - DICTADO  la opción de voz: SW1 abre/cierra la escucha, el texto que
 *              devuelve la PC entra al MISMO borrador de COMPOSE.
 *
 * La idea de fondo: voz y teclado escriben un único borrador, y el envío es el
 * mismo camino AES-GCM que usa la CLI. SW3 cicla las pantallas (como en el
 * código del grupo) y el joystick también cicla (Izq/Der), que sirve de plan B
 * si un botón del tablero falla.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    UI_IDLE,
    UI_CONFIG,
    UI_COMPOSE,
    UI_DICTADO,
    UI_COUNT
} ui_state_t;

/* Bytes del borrador (mas que los 212 que caben en la trama E2EE, para poder
   editar de sobra y recortar al enviar). */
#define UI_BORRADOR_MAX 300

/* Crea el mutex del borrador. Llamar antes de arrancar la task de la UI. */
void ui_init(void);

/* Bucle de la task de UI: dibuja y lee botones. No vuelve nunca. */
void ui_run(void);

/* --- Borrador compartido (voz + teclado + PC) --- */
/* Copia el borrador a 'out' bajo mutex (devuelve cuantos bytes utiles copio).
   Es la unica forma segura de leerlo desde una task distinta de la de la UI. */
size_t ui_borrador_copia(char *out, size_t cap);
void ui_borrador_set(const char *txt);
void ui_borrador_limpiar(void);

/* Manda el borrador CIFRADO a todos los pares. Escribe un resumen legible en
   resumen (puede ser NULL). Devuelve 0 si mandó al menos uno. */
int ui_enviar_borrador(char *resumen, size_t cap);

/* Recorta a max_bytes SIN partir un codepoint UTF-8 (la trama E2EE son 212
   bytes y cortar a mitad de un acento se lee como basura). */
size_t ui_recorte_utf8(const char *src, size_t max_bytes);

/* Gancho de prueba: el REPL fuerza una pantalla para poder sacarle el PNG a
   cada una sin necesidad de dedos (tools/pantalla_png.py). */
void ui_forzar(ui_state_t estado);
ui_state_t ui_estado(void);
const char *ui_nombre(ui_state_t estado);

/* Fuerza la pagina inicial del pager de mensajes recibidos (-1 = no forzar).
   Es para poder sacarle el PNG a TODAS las paginas de un mensaje largo: la
   placa que recibe no tiene USB, asi que sin esto esa pantalla no se ve. */
void ui_aviso_pagina(int pagina);
/* Fuerza la pagina del texto largo en MENSAJE/DICTADO (REPL: 'ui pag N'). */
void ui_texto_pagina(int pagina);

/* 'botones' del REPL: loguea cada presion con su GPIO ('presion: UP (io 9)'),
   unico modo de verificar contra el hardware el mapa del joystick (board.h
   tiene el joystick rotado 45 grados y hay dos versiones del pinout). */
void ui_log_presiones(bool activar);
