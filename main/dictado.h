#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
    Dictado: la opcion del menu que convierte voz en texto.

    El audio NO se transcribe en la placa (el S3 no tiene modelo de español y
    PSRAM pelea con el OLED). Lo que hace este modulo es:

      1. abrir el microfono y guardar la toma completa en RAM interna
      2. drainarla por la consola con el protocolo KIWIAUDIO de transfer.c
      3. recibir de vuelta el texto (comando 'texto <base64>' del REPL) y
         mostrarlo en el OLED
      4. dejar ese texto a mano para que el teclado/mensaje lo tome como
         borrador (dictado_texto())

    Se dispara desde el REPL ('grabar') o desde el menu; el drainado puede
    tardar unos segundos y NO bloquea la UI porque corre en su propia task.
*/

/* Marca que hay micro disponible. No reserva memoria: el buffer se pide al
   abrir la toma, que es cuando se puede saber cuanta heap queda libre. */
esp_err_t dictado_iniciar(void);

/* Le dice al dictado si puede pintar. Con el menu del grupo andando (ui.c) la
   pantalla es de la UI, y a dictado se le pasa false para que no se peleen por
   el OLED: el estado y la transcripcion se ven en la pantalla DICTADO. Si no
   hay display, tampoco pinta (display_flush sobre un panel NULL se cuelga). */
void dictado_pantalla(bool disponible);

/* Abre la toma, o la cierra y la drena si ya estaba abierta. Devuelve ESP_ERR
   INVALID_STATE si no hay micro. */
esp_err_t dictado_alternar(void);

bool dictado_grabando(void);

/* Frames grabadas hasta ahora (o de la ultima toma cerrada). */
size_t dictado_frames(void);

/* Cierra y drena si la grabacion se termino sola (se lleno el buffer). Se llama
   desde la task de dictado, no hace falta llamarlo a mano. */
void dictado_mantener(void);

/* Muestra texto en el OLED (debe venir en ASCII: la fonte es 5x7). Guarda el
   ultimo texto para que el compositor del mensaje lo use como borrador. */
void dictado_mostrar(const char *texto);

/* Ultimo texto que mand la PC, o "" si todava no vino ninguno. */
const char *dictado_texto(void);

/* Olvida el ultimo texto dictado (se llama al enviar: el proximo mensaje sale
   limpio en vez de salir con el anterior enganchado). */
void dictado_texto_limpiar(void);

/* Se increments con cada texto nuevo. La UI lo usa para detectar "llego una
   transcripcion" y sembrar el borrador sin pisar una edicion en curso. */
unsigned dictado_texto_version(void);

/* Una linea compacta para el comando 'estado' del REPL. */
void dictado_resumen(char *out, size_t cap);
