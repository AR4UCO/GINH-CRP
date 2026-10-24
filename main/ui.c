/*
 * Menu del mensajero mergiado (kiwi-os).
 *
 * Dibuja y controla las cuatro pantallas. Es la respuesta a los dos TODOs que
 * habia dejado la rama graficos:
 *   - "inicializar Wi-Fi/ESP-NOW aca"  -> ya lo hace app_main(); la pantalla
 *     IDLE se limita a MOSTRAR lo que la capa E2EE ya sabe (pares y huellas),
 *     que es la forma honesta de medir que la red esta andando.
 *   - "escribir el esp_now_send en COMPOSE" -> ui_enviar_borrador(), que usa
 *     el mismo e2ee_send() de la CLI (AES-GCM + anti-replay), no un envio
 *     crudo.
 *
 * Regla que hay que respetar (nos costó una hora): la pantalla SOLO se repinta
 * cuando algo cambio de verdad. El teclado del grupo repintaba cada 20 ms y
 * tapaba cualquier overlay; ademas es SPI de mas.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio.h"
#include "buttons.h"
#include "dictado.h"
#include "display.h"
#include "e2ee.h"
#include "inbox.h"
#include "peer_pick.h"
#include "teclado.h"
#include "ui.h"

static const char TAG[] = "ui";

/* 128 px / 6 px por caracter = 21 columnas utiles (la fonte es 5x7 + 1 de gap). */
#define UI_COLS      21
#define BORRADOR_MAX UI_BORRADOR_MAX
#define AVISO_MS     3000u

static SemaphoreHandle_t s_lock;
static char s_borrador[BORRADOR_MAX + 1];
static ui_state_t s_estado = UI_IDLE;

/* Aviso de un mensaje recibido: se guarda una copia para poder mostrarla en
   IDLE despues (el inbox es de un solo mensaje y se limpia al tomarlo). */
static char s_ultimo_de[INBOX_SENDER_MAX + 16];
static char s_ultimo_txt[INBOX_TEXT_MAX];
static bool s_ultimo_visto;

/* Aviso de accion ("enviado a ..."), se pisa solo a los AVISO_MS. */
static char s_aviso[UI_COLS * 2];
static TickType_t s_aviso_hasta;

/* Pagina del pager que se fuerza desde el REPL ('ui aviso ... --pag=N'), para
   poder fotografiar todas las paginas de un mensaje largo sin dedos. -1 = no
   forzar (comportamiento normal: arranca en la primera). */
static int s_aviso_pagina = -1;

/* Página del texto largo en las dos pantallas de escritura (MENSAJE y DICTADO).
   Reportado por el usuario (24/10 08:4x): un dictado largo NO se podia leer
   entero; ahi se pintaban solo las ULTIMAS lineas (pintar_envuelto es una cola)
   y no habia forma de ver el principio. Se hojea apretando el joystick (en esas
   dos pantallas el CENTER esta libre; en el teclado radial es Enter) y el REPL
   la puede forzar con 'ui pag N' para poder fotografiar cada pagina. */
static int s_texto_pagina = 0;
static int s_texto_paginas = 1;

/* 'botones' del REPL: loguea cada presion tal cual la ve la placa, para
   verificar el mapa del joystick contra el hardware sin adivinar. */
static bool s_log_presiones;

/* Destinatario elegido con peer_pick() desde CONFIG. Si no se eligio, el envio
   va a TODOS los pares seguros (con dos placas en la mesa es lo que uno
   espera). Con varios pares, elegir evita mandarle a todo el laboratorio. */
static uint8_t s_destino[6];
static bool s_tiene_destino;

/* ------------------------------------------------------------------ */
/* Borrador compartido                                                 */
/* ------------------------------------------------------------------ */

static void candado(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void destrabo(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void ui_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
}

/* El puntero interno SOLO lo usa la task de la UI, que es la que lo escribe;
   cualquier otra task tiene que copiar bajo el mutex. */
static const char *borrador_interno(void)
{
    return s_borrador;
}

size_t ui_borrador_copia(char *out, size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    candado();
    strncpy(out, s_borrador, cap - 1);
    out[cap - 1] = '\0';
    const size_t n = strlen(out);
    destrabo();
    return n;
}

void ui_borrador_set(const char *txt)
{
    if (!txt) {
        return;
    }
    candado();
    strncpy(s_borrador, txt, sizeof(s_borrador) - 1);
    s_borrador[sizeof(s_borrador) - 1] = '\0';
    destrabo();
}

void ui_borrador_limpiar(void)
{
    candado();
    s_borrador[0] = '\0';
    destrabo();
}

/* Corta sin partir un codepoint UTF-8: los bytes de continuacion empiezan con
   10xxxxxx, asi que retrocedemos hasta encontrar un byte que no lo sea. */
size_t ui_recorte_utf8(const char *src, size_t max_bytes)
{
    size_t n = strlen(src);
    if (n <= max_bytes) {
        return n;
    }
    n = max_bytes;
    while (n > 0 && ((unsigned char)src[n] & 0xC0u) == 0x80u) {
        n--;
    }
    /* Y preferimos no partir una palabra: retrocedemos hasta un espacio, pero
       solo un poco (si la "palabra" ocupa 150 bytes, cortar al medio es mejor
       que devolver un mensaje casi vacio). */
    size_t espacio = 0;
    for (size_t i = n; i > 0 && n - i < 24; i--) {
        if (src[i] == ' ') {
            espacio = i;
            break;
        }
    }
    return espacio ? espacio : n;
}

/* El destino elegido sigue teniendo sesion segura? Las claves viven en RAM y
   un HELLO re-anunciado puede reemplazar al par, asi que siempre se comprueba.
   Copia la MAC bajo el mutex: la task de la UI elige el destino y la del REPL
   puede estar enviando en ese mismo instante. */
static bool destino_actual(uint8_t mac_out[6])
{
    if (!s_tiene_destino) {
        return false;
    }
    candado();
    memcpy(mac_out, s_destino, 6);
    destrabo();

    const int n = e2ee_established_count();
    for (int i = 0; i < n; i++) {
        e2ee_peer_info_t p = {0};
        if (e2ee_get_peer_info(i, &p) && memcmp(p.mac, mac_out, 6) == 0) {
            return true;
        }
    }
    candado();
    s_tiene_destino = false;
    destrabo();
    return false;
}

int ui_enviar_borrador(char *resumen, size_t cap)
{
    char texto[BORRADOR_MAX + 1];

    candado();
    memcpy(texto, s_borrador, sizeof(texto));
    destrabo();
    texto[sizeof(texto) - 1] = '\0';

    if (!texto[0]) {
        if (resumen && cap) {
            snprintf(resumen, cap, "borrador vacio");
        }
        return 1;
    }

    const int pares = e2ee_established_count();
    if (pares == 0) {
        if (resumen && cap) {
            snprintf(resumen, cap, "sin par seguro");
        }
        return 1;
    }

    /* Un par puede haber caido desde que se eligio; si ya no esta, se vuelve
       al comportamiento por defecto (mandar a todos) en vez de fallar. */
    uint8_t destino_mac[6];
    const bool apuntado = destino_actual(destino_mac);
    const size_t n = ui_recorte_utf8(texto, E2EE_MAX_PAYLOAD);

    int enviados = 0;
    const char *a_quien = "?";
    for (int i = 0; i < pares; i++) {
        e2ee_peer_info_t p = {0};
        if (!e2ee_get_peer_info(i, &p)) {
            continue;
        }
        if (apuntado && memcmp(p.mac, destino_mac, 6) != 0) {
            continue;
        }
        const esp_err_t err = e2ee_send(p.mac, (const uint8_t *)texto, n);
        if (err == ESP_OK) {
            enviados++;
            a_quien = p.name;
        } else {
            ESP_LOGW(TAG, "envio a %s fallo: %s", p.name, esp_err_to_name(err));
        }
    }

    if (resumen && cap) {
        if (enviados) {
            snprintf(resumen, cap, "enviado a %s (%u B)", a_quien, (unsigned)n);
        } else {
            snprintf(resumen, cap, "no se pudo enviar");
        }
    }

    if (enviados) {
        snprintf(s_aviso, sizeof(s_aviso), "ENVIADO %u B", (unsigned)n);
        ESP_LOGI(TAG, "menu -> %s: %u bytes cifrados", a_quien, (unsigned)n);
        /* Despues de enviar, el borrador se limpia. Si no, el siguiente mensaje
           sale con el anterior enganchado: en la demo la placa que recibia veia
           un texto que crecia y crecia toma tras toma. Para armar un mensaje por
           partes sigue estando --acumular (la PC re-empuja el acumulado). */
        ui_borrador_set("");
        dictado_texto_limpiar();
    } else {
        snprintf(s_aviso, sizeof(s_aviso), "ERROR AL ENVIAR");
    }
    s_aviso_hasta = xTaskGetTickCount() + pdMS_TO_TICKS(AVISO_MS);
    return enviados ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Dibujo                                                              */
/* ------------------------------------------------------------------ */

/* Envuelve y pinta la ventana de 'filas' lineas de la pagina 'pagina' (0-based),
   cortando en espacios para no partir palabras. Devuelve cuantas paginas hay.
   Lo usa el pager de mensajes recibidos: un mensaje de 211 bytes son ~10 lineas
   de 21, y en 4 filas solo entra un tercio -> se hojea. */
static int pintar_envuelto_desde(int x, int y, int filas, const char *p, int pagina)
{
    if (filas > 6) {
        filas = 6;
    }
    char linea[UI_COLS + 1];
    /* Anillo de 12: con el payload maximo (211 B) nunca salen mas de ~11 lineas,
       asi la pagina pedida siempre esta dentro de lo que guardamos. */
    char ret[12][UI_COLS + 1];
    int total = 0;

    while (*p) {
        int n = 0;
        while (n < UI_COLS && p[n]) {
            n++;
        }
        if (p[n]) {
            int corte = n;
            while (corte > 0 && p[corte] != ' ') {
                corte--;
            }
            if (corte > 0) {
                n = corte;
            }
        }
        memcpy(linea, p, n);
        linea[n] = '\0';
        snprintf(ret[total % 12], sizeof(ret[0]), "%s", linea);
        total++;
        p += n;
        while (*p == ' ') {
            p++;
        }
    }

    if (total == 0) {
        return 1;
    }
    const int ultima_pagina = (total - 1) / filas;
    if (pagina > ultima_pagina) {
        pagina = ultima_pagina;
    }
    const int desde = pagina * filas;
    const int hasta = (desde + filas < total) ? desde + filas : total;
    for (int i = desde; i < hasta; i++) {
        char out[UI_COLS + 1];
        /* Sin marcador '..': el pie ya dice 'pagina/total', y pintar '..'
           nos comia dos caracteres utiles de la ultima fila de la pagina. */
        snprintf(out, sizeof(out), "%s", ret[i % 12]);
        display_text(x, y + (i - desde) * 10, out, false);
    }
    return ultima_pagina + 1;
}

/* Envuelve en hasta 'filas' lineas de UI_COLS y pinta las ultimas, cortando en
   espacios para no partir palabras. */
static void pintar_envuelto(int x, int y, int filas, const char *p)
{
    if (filas > 6) {
        filas = 6; /* es el tamaño del anillo de abajo */
    }
    char linea[UI_COLS + 1];
    /* Anillo con las ULTIMAS filas: lo que uno esta escribiendo o dictando esta
       al final del borrador, y en una pantalla de 3 lineas mostrar el principio
       es mostrar siempre lo mismo. Por eso la ventana es una cola. */
    char ret[6][UI_COLS + 1];
    int total = 0;

    while (*p) {
        int n = 0;
        while (n < UI_COLS && p[n]) {
            n++;
        }
        if (p[n]) {
            int corte = n;
            while (corte > 0 && p[corte] != ' ') {
                corte--;
            }
            if (corte > 0) {
                n = corte;
            }
        }
        memcpy(linea, p, n);
        linea[n] = '\0';
        snprintf(ret[total % 6], sizeof(ret[0]), "%s", linea);
        total++;
        p += n;
        while (*p == ' ') {
            p++;
        }
    }

    if (total == 0) {
        return;
    }
    const int primera = (total > filas) ? total - filas : 0;
    for (int i = primera; i < total; i++) {
        char out[UI_COLS + 1];
        if (i == primera && primera > 0) {
            /* '..' avisa que hay texto mas arriba (el mensaje sigue). */
            snprintf(out, sizeof(out), "..%.19s", ret[i % 6]);
        } else {
            snprintf(out, sizeof(out), "%s", ret[i % 6]);
        }
        display_text(x, y + (i - primera) * 10, out, false);
    }
}

/* Pie de pagina en tira: primera etiqueta al margen izquierdo y la otra al
   derecho. Medido: la fonte 5x7 mas 1 px de espacio = 6 px por caracter, o sea
   21 columnas utiles (128/6); concatenate dos etiquetas de 11 y 11 se cortaba.
   Alineando a los bordesganamos una columna y nunca se trunca la derecha. */
static void pie_pair(int y, const char *izq, const char *der)
{
    if (izq && izq[0]) {
        display_text(0, y, izq, false);
    }
    if (der && der[0]) {
        display_text((UI_COLS - (int)strlen(der)) * DISPLAY_FONT_W, y, der, false);
    }
}

static void pintar(void)
{
    char fila[64]; /* mas grande que las 21 columnas del OLED: snprintf no debe
                      truncar por formato y la fonte igual recorta en pantalla */
    uint8_t mac[6];
    char nick[E2EE_NICK_MAX + 1];
    e2ee_get_self(mac, nick, sizeof(nick));

    const int pares = e2ee_established_count();
    e2ee_peer_info_t par = {0};
    const bool hay_par = e2ee_get_peer_info(0, &par);
    const bool grabando = dictado_grabando();
    const bool aviso_vivo = s_aviso[0] && xTaskGetTickCount() < s_aviso_hasta;

    display_clear();

    switch (s_estado) {
        case UI_IDLE:
            display_text(0, 0, "[ MOD: SNIFFER ]", false);
            snprintf(fila, sizeof(fila), "NICK %s P:%d", nick, pares);
            display_text(0, 10, fila, false);
            if (hay_par) {
                /* 21 columnas: el nick (15) + la huella entera no caben juntos,
                   asi que en IDLE se muestra un recorte y en CONFIG la huella
                   completa, que es donde uno la verifica contra la otra placa. */
                snprintf(fila, sizeof(fila), "PAR %.8s %02X%02X-%02X", par.name,
                         par.fingerprint[0], par.fingerprint[1], par.fingerprint[2]);
            } else {
                snprintf(fila, sizeof(fila), "SIN PAR AUN");
            }
            display_text(0, 22, fila, false);
            if (s_ultimo_visto) {
                display_text(0, 30, s_ultimo_de, true);
                /* y=40 y no 42: display_text ocupa y..y+7, y el pie esta en 48.
                   Con 42 la ultima linea del mensaje se comfa media etiqueta. */
                pintar_envuelto(0, 40, 1, s_ultimo_txt);
            }
            /* Dos lineas: con 21 columnas (fonte de 6 px sobre 128) no entran las
               dos etiquetas juntas y se cortaba la ultima letra. No meto 'Hola' junto
               a Hello: en SNIFFER SW1 manda el HELLO, y el usuario pidio que SW3 diga
               config porque es a donde te lleva. */
            pie_pair(48, "[SW1]Hello", "[SW2]Ver");
            display_text(0, 56, "[SW3]Config", false);
            break;

        case UI_CONFIG:
            display_text(0, 0, "--- CONFIGURACION ---", false);
            snprintf(fila, sizeof(fila), " NICK: %s", nick);
            display_text(0, 10, fila, false);
            snprintf(fila, sizeof(fila), MACSTR, MAC2STR(mac));
            display_text(0, 20, fila, false);
            if (hay_par) {
                snprintf(fila, sizeof(fila), "HUELLA %02X%02X-%02X%02X", par.fingerprint[0],
                         par.fingerprint[1], par.fingerprint[2], par.fingerprint[3]);
                display_text(0, 30, fila, false);
            } else {
                display_text(0, 30, "sin par seguro", false);
            }
            pie_pair(48, "[SW1]Nick", "[SW2]Par");
            display_text(0, 56, "[SW3]Mensaje", false);
            break;

        case UI_COMPOSE: {
            display_text(0, 0, "--- NUEVO MENSAJE ---", false);
            const char *b = borrador_interno();
            if (b[0]) {
                if (s_texto_pagina < 0) {
                    s_texto_pagina = 0;
                }
                s_texto_paginas = pintar_envuelto_desde(0, 10, 3, b, s_texto_pagina);
                if (s_texto_pagina > s_texto_paginas - 1) {
                    s_texto_pagina = s_texto_paginas - 1;
                }
            } else {
                s_texto_paginas = 1;
                s_texto_pagina = 0;
                display_text(2, 10, "borrador vacio", false);
            }
            /* Linea de estado: cuantos bytes viajan (la trama E2EE corta en
               212), a quien va si se eligio destinatario, y si el micro esta
               abierto en este momento. */
            char a_quien[E2EE_NICK_MAX + 1] = "";
            uint8_t destino_mac[6];
            if (destino_actual(destino_mac)) {
                e2ee_lookup_name(destino_mac, a_quien, sizeof(a_quien));
            } else {
                snprintf(a_quien, sizeof(a_quien), "todos");
            }
            char pag[40] = ""; /* grande por gcc: en la pantalla nunca pasa de ~9 */
            if (s_texto_paginas > 1) {
                /* Numeros de un digito a proposito (como en el pager del aviso): si
                   no, gcc se queja de que '%d' puede truncar en un buffer chico. */
                const int tot = s_texto_paginas > 9 ? 9 : s_texto_paginas;
                const int act = (s_texto_pagina + 1) > 9 ? 9 : (s_texto_pagina + 1);
                snprintf(pag, sizeof(pag), " p%d/%d+joy", act, tot);
            }
            if (pag[0]) {
                /* Con el indicador de pagina no entra el destinatario (21 columnas),
                   y poder leer el resto del dictado es mas importante en ese momento. */
                snprintf(fila, sizeof(fila), " %u/%dB%s%s", (unsigned)strlen(b),
                         E2EE_MAX_PAYLOAD, pag, grabando ? " GRAB" : "");
            } else {
                snprintf(fila, sizeof(fila), " %u/%d a:%.8s%s", (unsigned)strlen(b),
                         E2EE_MAX_PAYLOAD, a_quien, grabando ? " GRAB" : "");
            }
            display_text(0, 40, fila, false);
            /* Lo que pidio el usuario (08:1x): arriba las dos acciones de escritura
               y ABAJO 'Config' (a donde te lleva SW3) y 'Enviar'. Con [SWx] entre
               corchetes 'Config' + 'Enviar' no caben en 21 columnas, por eso el pie
               va alineado a los bordes. */
            pie_pair(48, "[SW1]Teclear", "[SW2]Voz");
            /* El usuario quiere leer 'Enviar' completo (no 'Envia'). Con corchetes en
               las dos etiquetas serian 22 caracteres y la pantalla tiene 21 columnas
               (fonte 5x7 + 1 px = 6 px por caracter sobre 128), asi que aca se le
               sacan los corchetes a la etiqueta de la derecha y la palabra queda entera. */
            pie_pair(56, "[SW3]Config", "SW4 Enviar");
            break;
        }

        case UI_DICTADO: {
            display_text(0, 0, "--- DICTADO ---", false);
            /* La linea de estado lleva el cronometro y el nivel adentro: en la
               pantalla solo hay lugar para las filas de texto y dos de pie. */
            const int nivel = audio_mic_level(); /* 0..100, medido en mic_task */
            char pag[16] = "";
            if (s_texto_paginas > 1) {
                const int tot = s_texto_paginas > 9 ? 9 : s_texto_paginas;
                const int act = (s_texto_pagina + 1) > 9 ? 9 : (s_texto_pagina + 1);
                snprintf(pag, sizeof(pag), " p%d/%d", act, tot);
            }
            if (grabando) {
                /* Con la toma abierta la linea de estado NO repite la tecla (eso ya
                   esta en el pie, '[SW1]Para') y lo que importa es cuanto dura la toma
                   y si el micro te esta escuchando. 21 columnas: con 'joy' no entraba. */
                snprintf(fila, sizeof(fila), "grab %.1fs %d%%%s",
                         (float)dictado_frames() / 16000.0f, nivel, pag);
            } else if (pag[0]) {
                snprintf(fila, sizeof(fila), "listo%s +joy", pag);
            } else {
                snprintf(fila, sizeof(fila), "listo para hablar");
            }
            display_text(0, 10, fila, true);
            /* Con la toma abierta usamos la franja libre de VU-meter: quien dicta
               tiene que VER si la placa lo esta escuchando. Medido en placa con el
               REPL ('estado' -> NIVEL mic=N): habitacion muda = 0..4. La formula de
               audio.c es logaritmica (20*log10(rms)-20 escalado a 0..100), asi que la
               voz medida en los WAV (rms ~800..11000, ver tools/niveles_wav.py)
               tendria que dar 40..100. */
            const int filas_txt = grabando ? 2 : 3;
            if (s_texto_pagina < 0) {
                s_texto_pagina = 0;
            }
            s_texto_paginas = pintar_envuelto_desde(0, 20, filas_txt, borrador_interno(),
                                                   s_texto_pagina);
            if (s_texto_pagina > s_texto_paginas - 1) {
                s_texto_pagina = s_texto_paginas - 1;
            }
            if (grabando) {
                const int ancho = UI_COLS * DISPLAY_FONT_W;
                display_fill_rect(0, 40, ancho, 4, false);
                display_fill_rect(0, 40, (ancho * nivel) / 100, 4, true);
            }
            /* Pie en dos pares alineados a los bordes (lo mismo que las otras
               pantallas). Before: '[SW2]Borra [SW4]Envia' iba concatenate y, con 21
               columnas utiles, la ultima letra se cortaba. Acercate si la barra no
               sube es el unico consejo que sirve y va en la linea de arriba. */
            pie_pair(48, grabando ? "[SW1]Para" : "[SW1]Hablar", "[SW2]Borra");
            pie_pair(56, "[SW3]Atras", "[SW4]Enviar");
            break;
        }

        default:
            break;
    }

    if (aviso_vivo) {
        display_text(0, 56, s_aviso, true);
    }

    display_flush();
}

/* Aviso de mensaje recibido: se ve del principio y se hojea (SW1 siguiente
   pagina, SW4 salir, otras teclas salen). Sin esto, un mensaje de 211 bytes
   mostraba solo las ultimas 4 lineas y quien recibe no puede leerlo. */
static void mostrar_mensaje(const char *de, const char *texto)
{
    uint32_t prev = buttons_read();
    const TickType_t inicio = xTaskGetTickCount();
    int pagina = s_aviso_pagina;
    if (pagina < 0) {
        pagina = 0;
    }
    s_aviso_pagina = -1; /* el forzado es de un solo uso (es para las capturas) */
    int paginas = 1;
    bool sucio = true;

    for (;;) {
        if (sucio) {
            display_clear();
            display_text(0, 0, de, true);
            paginas = pintar_envuelto_desde(0, 12, 4, texto, pagina);
            if (pagina >= paginas) {
                /* El '--pag=N' del REPL puede pasarse de la ultima pagina (la
                   pantalla ya se pinto clampada; falta ajustar el pie). */
                pagina = paginas - 1;
            }
            char pie[64];
            /* Los numeros se achican a un digito a proposito: el pie tiene 21
               columnas (y gcc exige que el buffer lo soporte en el peor caso). */
            const int p_tot = paginas > 9 ? 9 : paginas;
            const int p_act = pagina + 1 > 9 ? 9 : pagina + 1;
            snprintf(pie, sizeof(pie), "%d/%d [SW1]sig[SW4]x", p_act, p_tot);
            display_text(0, 56, pie, false);
            display_flush();
            sucio = false;
            ESP_LOGI(TAG, "pager pagina %d/%d", pagina + 1, paginas);
        }

        if (xTaskGetTickCount() - inicio > pdMS_TO_TICKS(8000)) {
            return;
        }
        const uint32_t ahora = buttons_read();
        const uint32_t nuevas = ahora & ~prev;
        prev = ahora;
        if (nuevas & (1u << BTN_KEY4)) {
            return;
        }
        if (nuevas & (1u << BTN_KEY1)) {
            pagina = (pagina + 1 >= paginas) ? 0 : pagina + 1;
            sucio = true;
            continue;
        }
        if (nuevas != 0) {
            return; /* cualquier otra tecla cierra, como antes */
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* ------------------------------------------------------------------ */
/* Control                                                             */
/* ------------------------------------------------------------------ */

void ui_forzar(ui_state_t estado)
{
    if (estado < UI_COUNT) {
        s_estado = estado;
    }
}

/* Forzar la pagina del texto largo desde el REPL ('ui pag N'): con esto se le
   puede sacar el PNG a cada pagina del dictado sin necesitar un dedo. */
void ui_texto_pagina(int pagina)
{
    s_texto_pagina = pagina < 0 ? 0 : pagina;
}

void ui_aviso_pagina(int pagina)
{
    s_aviso_pagina = pagina < 0 ? -1 : pagina;
}

ui_state_t ui_estado(void)
{
    return s_estado;
}

const char *ui_nombre(ui_state_t estado)
{
    switch (estado) {
        case UI_IDLE: return "idle";
        case UI_CONFIG: return "config";
        case UI_COMPOSE: return "compose";
        case UI_DICTADO: return "dictado";
        default: return "?";
    }
}

/* Firma de TODO lo que se ve en pantalla. Si la firma no cambia, NO se repinta
   (ver el comentario del encabezado). Las frames se agrupan de a ~0,5 s para
   que el cronometro de la toma no dispare un repintado cada 20 ms. */
static void firma_actual(char *out, size_t cap, unsigned version_dictado)
{
    uint8_t mac[6];
    char nick[E2EE_NICK_MAX + 1];
    e2ee_get_self(mac, nick, sizeof(nick));

    snprintf(out, cap, "%d|%s|%s|%d|%u|%d|%u|%u|%s|%s|%d|%d|%d", (int)s_estado, nick, borrador_interno(),
             e2ee_established_count(), (unsigned)(dictado_frames() / 8000),
             dictado_grabando() ? 1 : 0, version_dictado, s_tiene_destino ? 1 : 0, s_ultimo_txt,
             (s_aviso[0] && xTaskGetTickCount() < s_aviso_hasta) ? s_aviso : "",
             s_ultimo_visto ? 1 : 0, s_texto_pagina, s_texto_paginas);
}

/* El joystick recorre solo las TRES pantallas principales. DICTADO no esta en el
   ciclo: es una sub-pantalla de COMPOSE a la que se entra con SW2 ('Voz'), asi no
   aparece de la nada cuando uno esta navegando. Desde dictado, el joystick te
   devuelve a compose. */
static ui_state_t ciclar_main(ui_state_t e, int direccion)
{
    if (e == UI_DICTADO) {
        return UI_COMPOSE;
    }
    int i = (int)e + direccion;
    if (i < 0) {
        i = 2;
    }
    if (i > 2) {
        i = 0;
    }
    return (ui_state_t)i;
}

void ui_run(void)
{
    char firma[512];
    char firma_prev[512] = "";
    char de[INBOX_SENDER_MAX];
    char txt[INBOX_TEXT_MAX];
    unsigned version_dictado = 0;

    uint32_t prev = buttons_read();
    bool primera_vez = true;

    for (;;) {
        /* 1) Mensaje cifrado que llego de la otra placa. */
        if (inbox_take(de, sizeof(de), txt, sizeof(txt))) {
            /* El pie de pagina del OLED tiene 21 columnas: el nick llega a 15 y
               el fallback es la MAC (17), así que el encabezado se recorta. */
            snprintf(s_ultimo_de, sizeof(s_ultimo_de), "DE %.10s", de);
            snprintf(s_ultimo_txt, sizeof(s_ultimo_txt), "%s", txt);
            s_ultimo_visto = true;
            mostrar_mensaje(s_ultimo_de, s_ultimo_txt);
            prev = buttons_read(); /* no tragarse la tecla que cerr el aviso */
        }

        /* 2) Texto que devolvio la PC (dictado): entra al borrador comun.
              La PC empuja el texto y sube la version; aca lo tomamos una sola
              vez por version para no pisar lo que el usuario esta editando. */
        if (dictado_texto_version() != version_dictado) {
            version_dictado = dictado_texto_version();
            char texto_dictado[256];
            snprintf(texto_dictado, sizeof(texto_dictado), "%s", dictado_texto());
            if (texto_dictado[0]) {
                ui_borrador_set(texto_dictado);
            }
        }

        /* 3) Botones: borde de subida, como en teclado.c. */
        const uint32_t pressed = buttons_read();
        const uint32_t nuevas = pressed & ~prev;
        prev = pressed;

        if (nuevas && s_log_presiones) {
            for (int b = 0; b < BTN_COUNT; b++) {
                if (nuevas & (1u << b)) {
                    ESP_LOGI(TAG, "presion: %s (io %d)", buttons_name((button_id_t)b),
                             buttons_io((button_id_t)b));
                }
            }
        }

        if (nuevas & (1u << BTN_KEY3)) {
            /* SW3 ya NO cicla las 4 pantallas (por eso el dictado aparecfa solo).
               Es contextual y el pie de cada pantalla dice a donde va:
               sniffer -> config, config -> mensaje, mensaje -> atras (config),
               dictado -> atras (mensaje). */
            switch (s_estado) {
                case UI_IDLE:
                    s_estado = UI_CONFIG;
                    break;
                case UI_CONFIG:
                    s_estado = UI_COMPOSE;
                    break;
                case UI_COMPOSE:
                    s_estado = UI_CONFIG;
                    break;
                case UI_DICTADO:
                    /* DICTADO es sub-pantalla de COMPOSE: 'Atras' vuelve a COMPOSE,
                       no a CONFIG (y asi SW2 'Voz' sigue siendo la entrada). */
                    s_estado = UI_COMPOSE;
                    break;
                default:
                    s_estado = UI_IDLE;
                    break;
            }
        } else if (nuevas & (1u << BTN_LEFT)) {
            s_estado = ciclar_main(s_estado, -1);
        } else if (nuevas & (1u << BTN_RIGHT)) {
            s_estado = ciclar_main(s_estado, +1);
        } else if (nuevas & (1u << BTN_KEY1)) {
            switch (s_estado) {
                case UI_IDLE: {
                    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                    /* Que hace SW1 aca: manda el HELLO, que es el unico paquete que
                       viaja SIN cifrar (broadcast). Lleva mi clave publica X25519, mi
                       nick y un nonce. La otra placa contesta, las dos derivan el
                       secreto compartido y ahi nace la sesion: sin sesion, e2ee_send()
                       devuelve ESP_ERR_NOT_FOUND y NINGUN mensaje puede salir. Por eso
                       el Hello existe: es 'presentate y armemos la clave'. */
                    e2ee_send_hello(bcast);
                    /* Y el aviso tiene que mostrar el resultado, porque si la sesion ya
                       estaba armada no cambia nada en pantalla y parece que el boton
                       no hace nada. La huella es identica en las dos placas: es lo que
                       se puede mostrar en la demo para probar que hablan cifrado. */
                    const int n = e2ee_established_count();
                    e2ee_peer_info_t p;
                    if (n > 0 && e2ee_get_peer_info(0, &p)) {
                        snprintf(s_aviso, sizeof(s_aviso), "PAR %.10s %02X%02X-%02X%02X",
                                 p.name, p.fingerprint[0], p.fingerprint[1], p.fingerprint[2],
                                 p.fingerprint[3]);
                    } else if (n > 0) {
                        snprintf(s_aviso, sizeof(s_aviso), "HELLO (pares=%d)", n);
                    } else {
                        snprintf(s_aviso, sizeof(s_aviso), "HELLO: sin par");
                    }
                    s_aviso_hasta = xTaskGetTickCount() + pdMS_TO_TICKS(AVISO_MS);
                    break;
                }
                case UI_CONFIG: {
                    /* Se copia el borrador antes: readline_seed() lo recorre sin
                       candado y la PC lo puede estar pisando por el REPL. */
                    char semilla[BORRADOR_MAX + 1];
                    ui_borrador_copia(semilla, sizeof(semilla));
                    char *nuevo = readline_seed(semilla);
                    if (nuevo && nuevo[0] && e2ee_set_nick(nuevo) == ESP_OK) {
                        snprintf(s_aviso, sizeof(s_aviso), "NICK %s", nuevo);
                        s_aviso_hasta = xTaskGetTickCount() + pdMS_TO_TICKS(AVISO_MS);
                    }
                    free(nuevo);
                    prev = buttons_read();
                    break;
                }
                case UI_COMPOSE: {
                    char semilla[BORRADOR_MAX + 1];
                    ui_borrador_copia(semilla, sizeof(semilla));
                    char *editado = readline_seed(semilla);
                    if (editado) {
                        ui_borrador_set(editado);
                        free(editado);
                    }
                    prev = buttons_read();
                    break;
                }
                case UI_DICTADO:
                    dictado_alternar();
                    break;
                default:
                    break;
            }
        } else if (s_estado == UI_IDLE && (nuevas & (1u << BTN_KEY2))) {
            /* 'Ver' del pie: vuelve a abrir el ultimo mensaje recibido, con su
               paginador. Sin esto, un mensaje largo se leia solo mientras duraba
               el aviso (4 s) y despues no habia forma de ver el resto. */
            if (s_ultimo_visto && s_ultimo_txt[0]) {
                mostrar_mensaje(s_ultimo_de, s_ultimo_txt);
                prev = buttons_read();
            } else {
                snprintf(s_aviso, sizeof(s_aviso), "sin mensajes") ;
                s_aviso_hasta = xTaskGetTickCount() + pdMS_TO_TICKS(2500);
            }
        } else if (nuevas & (1u << BTN_KEY2)) {
            /* SW2 es la accion secundaria, y significa algo distinto en cada
               pantalla (lo que dice el pie de pagina):
               CONFIG   -> elegir el destinatario con el peer_pick del grupo
               COMPOSE  -> abre/cierra el dictado sin salir del teclado
               DICTADO  -> limpia el borrador (por si la PC transcribio basura) */
            if (s_estado == UI_CONFIG) {
                uint8_t elegido[6];
                if (peer_pick(elegido)) {
                    candado();
                    memcpy(s_destino, elegido, 6);
                    s_tiene_destino = true;
                    destrabo();
                    char nombre[E2EE_NICK_MAX + 1] = "?";
                    e2ee_lookup_name(elegido, nombre, sizeof(nombre));
                    snprintf(s_aviso, sizeof(s_aviso), "PARA %.12s", nombre);
                } else {
                    snprintf(s_aviso, sizeof(s_aviso), "sin par seguro");
                }
                s_aviso_hasta = xTaskGetTickCount() + pdMS_TO_TICKS(AVISO_MS);
                prev = buttons_read(); /* la tecla que cerro la eleccion no cuenta */
            } else if (s_estado == UI_COMPOSE) {
                /* MEDIDO con los dedos del usuario (08:0x): SW2 ('Voz') entraba a la
                   pantalla de dictado PERO ademas llamaba a dictado_alternar(), o sea
                   arrancaba a grabar sin que nadie lo pidiera; en pantalla solo se veia
                   'GRAB' a la derecha y parecia que no hacia nada. SW2 ahora ONLY abre
                   la pantalla; el micro se arranca con SW1, que es lo que dice el pie. */
                s_estado = UI_DICTADO;
                snprintf(s_aviso, sizeof(s_aviso), "sw1 para hablar");
                s_aviso_hasta = xTaskGetTickCount() + pdMS_TO_TICKS(AVISO_MS);
                /* Antes solo aparecia "GRAB" a la derecha y no se entendia nada:
                   avisamos que la toma se cierra sola y que falta la PC escuchando
                   (sin el listener de transcribir_vivo.py no hay texto de vuelta). */
                snprintf(s_aviso, sizeof(s_aviso), dictado_grabando() ? "HABLA (4 s)" : "procesando..");
                s_aviso_hasta = xTaskGetTickCount() + pdMS_TO_TICKS(AVISO_MS);
            } else if (s_estado == UI_DICTADO) {
                ui_borrador_limpiar();
            }
        } else if (nuevas & (1u << BTN_CENTER)) {
            /* Apretar el joystick hojea el texto largo en las pantallas donde se
               escribe (lo que falta del dictado o del mensaje). En el teclado radial
               el joystick apretado es Enter; aca esta libre y es la unica tecla que
               no tiene etiqueta en el pie. */
            if (s_estado == UI_COMPOSE || s_estado == UI_DICTADO) {
                if (s_texto_paginas > 1) {
                    s_texto_pagina = (s_texto_pagina + 1) % s_texto_paginas;
                    ESP_LOGI(TAG, "pager texto pagina %d/%d", s_texto_pagina + 1,
                             s_texto_paginas);
                } else {
                    snprintf(s_aviso, sizeof(s_aviso), "el texto entra entero");
                    s_aviso_hasta = xTaskGetTickCount() + pdMS_TO_TICKS(2500);
                }
            }
        } else if (nuevas & (1u << BTN_KEY4)) {
            /* SW4 = Enter (en el teclado radial tambien es Enter): manda el
               borrador cifrado. Es la respuesta al TODO de esp_now_send. */
            if (s_estado == UI_COMPOSE || s_estado == UI_DICTADO) {
                ui_enviar_borrador(NULL, 0);
            }
        }

        /* 4) Repintar solo si cambio algo. */
        firma_actual(firma, sizeof(firma), version_dictado);
        if (primera_vez || strcmp(firma, firma_prev) != 0) {
            primera_vez = false;
            memcpy(firma_prev, firma, sizeof(firma_prev));
            pintar();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ----Hooks para el REPL---- */

void ui_log_presiones(bool activar)
{
    s_log_presiones = activar;
}

bool ui_log_presiones_estado(void)
{
    return s_log_presiones;
}
