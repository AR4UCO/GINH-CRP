/*
 * Teclado radial (display + botones).
 * readline() bloquea hasta que se presiona Enter (KEY4) y devuelve un
 * string malloc'd que el llamador debe liberar con free().
 */
#pragma once

char *readline(void);
