/*
 * Buzón de un solo mensaje (pager): guarda el último mensaje recibido.
 * inbox_post() se llama desde la tarea cripto; inbox_take() desde la
 * tarea del teclado (única que dibuja en la pantalla).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define INBOX_SENDER_MAX 16
#define INBOX_TEXT_MAX   241

bool inbox_init(void);

/* Guarda/sobreescribe el último mensaje. Thread-safe. */
bool inbox_post(const char *sender, const char *text, size_t len);

/* ¿Hay un mensaje sin mostrar? */
bool inbox_pending(void);

/* Copia y limpia el mensaje pendiente. false si no había. */
bool inbox_take(char *sender, size_t sender_len, char *text, size_t text_len);
