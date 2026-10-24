/*
 * Consola interactiva sobre UART para enviar/recibir mensajes cifrados.
 */
#pragma once

/* Arranca el REPL (tarea propia). Comandos: send, sendto, peers, hello, whoami. */
void cli_start(void);
