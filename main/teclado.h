/*
 * Teclado radial (display + botones).
 * readline() bloquea hasta que se presiona Enter (KEY4) y devuelve un
 * string malloc'd que el llamador debe liberar con free().
 *
 * readline_seed() arranca la linea con un texto previo. Es la pieza que hace
 * que el dictado y el teclado sumen: lo que devolvio la PC se ve editable con
 * el teclado radial antes de enviar.
 */
#pragma once

char *readline(void);
char *readline_seed(const char *seed);

/* Girar el selector de cuadrantes del teclado (0-3 pasos de 90 grados, horario)
   y guardarlo en NVS. Sirve porque cada placa viene con el joystick girado de
   otra manera; sin esto hay que reflashear para probar los 4 valores. */
void teclado_set_giro(int pasos);
int teclado_giro(void);
