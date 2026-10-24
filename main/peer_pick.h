/*
 * Selector de destinatario en la pantalla.
 * Si hay una sola sesión segura, la elige sin preguntar.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Bloquea hasta elegir un peer. Devuelve su MAC en mac_out.
 * false si no hay peers o si se canceló (KEY3). */
bool peer_pick(uint8_t mac_out[6]);
