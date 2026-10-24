/*
 * Consola interactiva para el mensajero E2EE.
 *
 *   send <texto>             envía cifrado a todos los peers (1 unicast c/u)
 *   sendto <índice|MAC|nick> <texto>  envía a un peer concreto
 *   peers                   lista sesiones seguras y huellas
 *   nick [nuevo]            muestra o cambia tu nick (persistente en NVS)
 *   hello                   re-anuncia por broadcast para descubrir placas
 *   whoami                  muestra nick/MAC propios
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cli.h"
#include "audio.h"
#include "e2ee.h"
#include "dictado.h"
#include "display.h"
#include "buttons.h"
#include "teclado.h"
#include "ui.h"
#include "transfer.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "inbox.h"

static const char *TAG = "CLI";

/* Une argv[first..] separándolos con un espacio.
 * Devuelve la longitud, 0 si no hay texto, o (size_t)-1 si no cabe. */
static size_t join_text(int argc, char **argv, int first, char *out, size_t out_size)
{
    if (first >= argc)
    {
        out[0] = '\0';
        return 0;
    }
    size_t pos = 0;
    for (int i = first; i < argc; i++)
    {
        size_t l = strlen(argv[i]);
        size_t sep = (i > first) ? 1 : 0;
        if (pos + sep + l + 1 > out_size)
            return (size_t)-1;
        if (sep)
            out[pos++] = ' ';
        memcpy(out + pos, argv[i], l);
        pos += l;
    }
    out[pos] = '\0';
    return pos;
}

static int cmd_send(int argc, char **argv)
{
    char text[E2EE_MAX_PAYLOAD + 1];
    size_t len = join_text(argc, argv, 1, text, sizeof(text));
    if (len == 0)
    {
        printf("Uso: send <texto>  (máx %d bytes)\n", E2EE_MAX_PAYLOAD);
        return 1;
    }
    if (len == (size_t)-1)
    {
        printf("Texto demasiado largo (máx %d bytes).\n", E2EE_MAX_PAYLOAD);
        return 1;
    }

    int n = e2ee_established_count();
    if (n == 0)
    {
        printf("Sin sesión segura todavía. Espera a que la otra placa aparezca.\n");
        return 1;
    }

    int enviados = 0;
    for (int i = 0; i < n; i++)
    {
        e2ee_peer_info_t p = {0};
        if (!e2ee_get_peer_info(i, &p))
            continue;
        esp_err_t err = e2ee_send(p.mac, (const uint8_t *)text, len);
        if (err == ESP_OK)
        {
            printf("-> %s (" MACSTR ")\n", p.name, MAC2STR(p.mac));
            enviados++;
        }
        else
        {
            printf("-> %s: error %s\n", p.name, esp_err_to_name(err));
        }
    }
    return enviados > 0 ? 0 : 1;
}

/* Acepta "aa:bb:cc:dd:ee:ff" (o mayúsculas). */
static bool parse_mac(const char *s, uint8_t mac[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++)
    {
        if (v[i] > 0xFF)
            return false;
        mac[i] = (uint8_t)v[i];
    }
    return true;
}

static int cmd_sendto(int argc, char **argv)
{
    if (argc < 3)
    {
        printf("Uso: sendto <índice|MAC> <texto>  (índices con 'peers')\n");
        return 1;
    }

    e2ee_peer_info_t p = {0};
    uint8_t mac[6];
    if (parse_mac(argv[1], mac))
    {
        // Destino explícito por MAC
        memcpy(p.mac, mac, 6);
        if (!e2ee_lookup_name(mac, p.name, sizeof(p.name)))
            snprintf(p.name, sizeof(p.name), "%s", argv[1]);
    }
    else
    {
        char *end = NULL;
        long idx = strtol(argv[1], &end, 10);
        bool por_indice = (end != argv[1] && *end == '\0');

        if (por_indice)
        {
            if (idx < 0 || !e2ee_get_peer_info((int)idx, &p))
            {
                printf("No existe el peer %d. Usa 'peers'.\n", (int)idx);
                return 1;
            }
        }
        else if (e2ee_find_peer_by_name(argv[1], mac))
        {
            memcpy(p.mac, mac, 6);
            if (!e2ee_lookup_name(mac, p.name, sizeof(p.name)))
                snprintf(p.name, sizeof(p.name), "%s", argv[1]);
        }
        else
        {
            printf("Peer '%s' no encontrado. Usa 'peers' (índice, MAC o nick).\n", argv[1]);
            return 1;
        }
    }

    char text[E2EE_MAX_PAYLOAD + 1];
    size_t len = join_text(argc, argv, 2, text, sizeof(text));
    if (len == 0 || len == (size_t)-1)
    {
        printf("Texto vacío o demasiado largo (máx %d bytes).\n", E2EE_MAX_PAYLOAD);
        return 1;
    }

    esp_err_t err = e2ee_send(p.mac, (const uint8_t *)text, len);
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("-> %s (" MACSTR "): %s\n", p.name, MAC2STR(p.mac), text);
    return 0;
}

static int cmd_peers(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int n = e2ee_established_count();
    if (n == 0)
    {
        printf("Ningún peer seguro todavía.\n");
        return 0;
    }

    printf("%-3s %-16s %-17s %-17s\n", "#", "Nick", "MAC", "Huella");
    for (int i = 0; i < n; i++)
    {
        e2ee_peer_info_t p = {0};
        if (!e2ee_get_peer_info(i, &p))
            continue;
        const uint8_t *f = p.fingerprint;
        printf("%-3d %-16s " MACSTR " %02X%02X%02X%02X-%02X%02X%02X%02X\n",
               i, p.name, MAC2STR(p.mac),
               f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
    }
    return 0;
}

static int cmd_hello(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_err_t err = e2ee_send_hello(bcast);
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("HELLO enviado por broadcast.\n");
    return 0;
}

static int cmd_whoami(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint8_t mac[6];
    char name[E2EE_NICK_MAX + 1];
    e2ee_get_self(mac, name, sizeof(name));
    printf("%s (" MACSTR ") - %d peer(s) seguro(s)\n",
           name, MAC2STR(mac), e2ee_established_count());
    return 0;
}

static int cmd_nick(int argc, char **argv)
{
    if (argc == 1)
    {
        uint8_t mac[6];
        char name[E2EE_NICK_MAX + 1];
        e2ee_get_self(mac, name, sizeof(name));
        printf("Nick actual: %s (MAC " MACSTR ")\n", name, MAC2STR(mac));
        return 0;
    }

    char nick[E2EE_NICK_MAX + 1];
    size_t len = join_text(argc, argv, 1, nick, sizeof(nick));
    if (len == 0 || len == (size_t)-1)
    {
        printf("Nick inválido: 1 a %d caracteres ASCII imprimibles.\n", E2EE_NICK_MAX);
        return 1;
    }

    esp_err_t err = e2ee_set_nick(nick);
    if (err != ESP_OK)
    {
        printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Nick guardado en NVS y anunciado: %s\n", nick);
    return 0;
}

static int cmd_free(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    size_t interno = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t mayor = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    printf("Heap libre: %u B (mínimo histórico: %u B)\n",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size());
    printf("  interno 8-bit: %u B libres, bloque mayor %u B\n",
           (unsigned)interno, (unsigned)mayor);
#if CONFIG_SPIRAM
    printf("  PSRAM: %u B libres\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#else
    printf("  PSRAM: deshabilitada (todo en SRAM interna)\n");
#endif
    return 0;
}

/* ---------- dictado (voz -> texto con la PC) ---------- */

/* 'grabar' abre la toma y 'grabar' de nuevo la cierra y la manda por consola.
   Es el mismo boton del menu, pero desde la PC: asi se puede probar y demo-
   trar sin depender de los dedos. El drenado tarda unos segundos: la placa
   queda ocupada ahi, no la UI. */
static int cmd_grabar(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_err_t err = dictado_alternar();
    if (err != ESP_OK) {
        printf("Sin microfono: %s\n", esp_err_to_name(err));
        return 1;
    }

    char linea[192];
    dictado_resumen(linea, sizeof(linea));
    printf("%s\n", linea);
    return 0;
}

/* 'texto <base64>': la PC devuelve la transcripcion y va directo al OLED.
   Se mandan en base64 para que ningun espacio o caracter rompa el parseo del
   REPL (que parte por espacios). */
static int cmd_texto(int argc, char **argv)
{
    if (argc < 2) {
        printf("Uso: texto <base64(utf8)>\n");
        return 1;
    }

    char buf[256];
    const size_t n = transfer_b64_a_texto(argv[1], buf, sizeof(buf));
    if (n == 0) {
        printf("Base64 vacio o invalido.\n");
        return 1;
    }

    dictado_mostrar(buf);
    printf("Texto en pantalla: %u chars\n", (unsigned)n);
    return 0;
}

/* 'pantalla': volcado del framebuffer para ver el OLED desde la PC (sin
   camara). Clave para poder probar los graficos sin estar mirando la placa. */
static int cmd_pantalla(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const uint8_t *fb = NULL;
    size_t len = 0;
    int w = 0, h = 0;
    display_snapshot(&fb, &len, &w, &h);
    if (!fb || !len) {
        printf("Sin framebuffer (el OLED no contesto).\n");
        return 1;
    }

    transfer_send_framebuffer(fb, len, w, h);
    printf("Pantalla enviada (%u bytes %dx%d).\n", (unsigned)len, w, h);
    return 0;
}

/* 'estado': una linea maquina para que las tools de la PC sepan qu esta
   pasando sin parsear logs. */
static int cmd_estado(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char linea[192];
    dictado_resumen(linea, sizeof(linea));
    printf("%s peers=%d\n", linea, e2ee_established_count());
    /* Linea aparte (no se toca la de arriba, que parsean las tools): lo que ve
       el operador en el OLED. */
    char borrador[UI_BORRADOR_MAX + 1];
    const size_t n_borrador = ui_borrador_copia(borrador, sizeof(borrador));
    printf("UI %s borrador=%u\n", ui_nombre(ui_estado()), (unsigned)n_borrador);
    /* Nivel del micro (0..100, el mismo que pinta la barra del dictado): sirve
       para decidir HACK_MIC_GAIN sin tener que bajar un WAV entero. */
    printf("NIVEL mic=%d grabando=%d\n", audio_mic_level(), dictado_grabando() ? 1 : 0);
    return 0;
}

/* 'mandar': el puente entre el dictado y el mensajero. Manda CIFRADO el
   borrador del menu (el mismo que ve y puede editar el operador en la placa),
   por eso usa ui_enviar_borrador(): una sola ruta de envio para el menu y para
   la PC, con el mismo AES-GCM y el mismo recorte de 212 bytes. */
static int cmd_mandar(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char borrador[UI_BORRADOR_MAX + 1];
    if (ui_borrador_copia(borrador, sizeof(borrador)) == 0 && dictado_texto()[0]) {
        /* La PC acaba de mandar 'texto' y la task de la UI todavia no lo tomó
           (tarda ~50 ms). Se siembra aca para que 'mandar' no dependa del
           timing de la UI. */
        ui_borrador_set(dictado_texto());
    } else if (dictado_texto()[0] && strcmp(borrador, dictado_texto()) != 0) {
        /* MEDIDO (03:0x): con 'texto' e 'mandar' a 5 ms de distancia, 'mandar'
           mandaba el mensaje ANTERIOR (la UI tarda hasta 50 ms en tomar el
           texto nuevo). Se espera a que el borrador concuerde, y si nunca
           concuerda (el usuario lo editó con el teclado) se manda lo que hay. */
        for (int intento = 0; intento < 8; intento++) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (ui_borrador_copia(borrador, sizeof(borrador)) != 0
                && strcmp(borrador, dictado_texto()) == 0) {
                break;
            }
        }
        if (strcmp(borrador, dictado_texto()) != 0) {
            ESP_LOGW(TAG, "el borrador no concuerda con el ultimo texto de la PC: se manda el de la UI");
        }
    }

    char resumen[64];
    const int rc = ui_enviar_borrador(resumen, sizeof(resumen));
    printf("%s\n", resumen);
    return rc == 0 ? 0 : 1;
}

/* 'ui': pantalla forzada del menu. Sirve para dos cosas: dejar el dispositivo
   en la pantalla que uno quiere en la demo, y sacarle el PNG a cada pantalla
   con tools/pantalla_png.py sin necesitar dedos. */
static int cmd_ui(int argc, char **argv)
{
    static const struct { const char *nombre; ui_state_t estado; } pantallas[] = {
        {"idle", UI_IDLE}, {"config", UI_CONFIG}, {"compose", UI_COMPOSE}, {"dictado", UI_DICTADO}
    };

    if (argc >= 2) {
        if (strcmp(argv[1], "borrar") == 0) {
            ui_borrador_limpiar();
            printf("Borrador limpiado.\n");
            return 0;
        }
        if (strcmp(argv[1], "pag") == 0) {
            /* Hojea el texto largo de MENSAJE/DICTADO sin dedos (en la placa es
               apretar el joystick). Sirve para fotografiar cada pagina. */
            const int p = (argc >= 3) ? atoi(argv[2]) : 0;
            ui_texto_pagina(p);
            printf("pagina de texto forzada a %d\n", p);
            return 0;
        }
        if (strcmp(argv[1], "aviso") == 0) {
            /* Deja encolado un mensaje recibido FALSO para poder ver (y sacarle
               el PNG a) la pantalla de aviso, que es lo que ven en la OTRA
               placa cuando llega un mensaje. Sin esto, esa pantalla solo se ve
               si una segunda placa manda algo, y su pantalla no tiene USB.

               '--pag=N' fuerza la pagina N del pager: asi se pueden fotografar
               TODAS las paginas de un mensaje largo sin cần un dedo. */
            int pagina = -1;
            int ultimo = argc;
            if (argc >= 3 && strncmp(argv[argc - 1], "--pag=", 6) == 0) {
                pagina = atoi(argv[argc - 1] + 6);
                ultimo = argc - 1;
            }
            char texto[INBOX_TEXT_MAX];
            const size_t n = join_text(ultimo, argv, 2, texto, sizeof(texto));
            if (n == 0 || n == (size_t)-1) {
                snprintf(texto, sizeof(texto), "mensaje de prueba para ver el aviso");
            }
            ui_aviso_pagina(pagina);
            inbox_post("PRUEBA", texto, strlen(texto));
            printf("Aviso encolado: %s%s\n", texto, pagina >= 0 ? " (pagina forzada)" : "");
            return 0;
        }
        for (size_t i = 0; i < sizeof(pantallas) / sizeof(pantallas[0]); i++) {
            if (strcmp(argv[1], pantallas[i].nombre) == 0) {
                ui_forzar(pantallas[i].estado);
                printf("Pantalla forzada: %s\n", pantallas[i].nombre);
                return 0;
            }
        }
        printf("Uso: ui [idle|config|compose|dictado|borrar]\n");
        return 1;
    }

    char nombre[24];
    char borrador[UI_BORRADOR_MAX + 1];
    const size_t n = ui_borrador_copia(borrador, sizeof(borrador));
    snprintf(nombre, sizeof(nombre), "%s", ui_nombre(ui_estado()));
    printf("Pantalla actual: %s (borrador: %u bytes)\n", nombre, (unsigned)n);
    return 0;
}

/* 'botones [on|off]': loguea cada presion con su GPIO. El joystick de la kiwi
   board esta rotado 45 grados respecto del pinout (ver board.h) y hay dos
   versiones del mapa: la unica forma de resolverlo es apretar y mirar. */
static int cmd_botones(int argc, char **argv)
{
    const bool activar = (argc < 2) || strcmp(argv[1], "off") != 0;
    ui_log_presiones(activar);
    printf("Log de presiones: %s (apreta y mira 'I (ms) ui: presion: NOMBRE (io N)')\n",
           activar ? "ON" : "OFF");
    return 0;
}

/* 'joystick': gira el joystick EN VIVO (0-3 pasos de 90 grados horarios) y
   guarda el giro en NVS. Cada kiwi board viene girada distinto (al grupo le
   pasaron dos mapas que difieren en un cuarto de vuelta), y reflashear para
   probar 0/1/2/3 cuesta ~3 minutos: esto tarda 2 segundos. */
static int cmd_teclas(int argc, char **argv)
{
    /* El joystick de cada placa viene girado distinto y el teclado radial lo sufre
       (el usuario midio 'selecciona el cuadrante de al lado'). No es lo mismo que
       'joystick': eso gira el driver y mueve tambien la navegacion del menu; esto
       gira SOLO el selector de cuadrantes del teclado. */
    if (argc >= 2) {
        const int pasos = atoi(argv[1]);
        if (pasos < 0 || pasos > 3) {
            printf("Uso: teclas [0|1|2|3] (pasos de 90 grados, horario)\n");
            return 1;
        }
        teclado_set_giro(pasos);
    }
    printf("giro del teclado: %d | arriba->cuadrante %d\n", teclado_giro(), teclado_giro());
    printf("con 26 letras: cuadrantes = arriba-izq abcdefg, arriba-der hijklmn, "
           "abajo-der opqrst, abajo-izq uvwxyz\n");
    return 0;
}

static int cmd_joystick(int argc, char **argv)
{
    if (argc >= 2) {
        const int pasos = atoi(argv[1]);
        if (pasos < 0 || pasos > 3) {
            printf("Uso: joystick [0|1|2|3] (pasos de 90 grados, horario)\n");
            return 1;
        }
        buttons_set_giro(pasos);
    }
    printf("giro: %d pasos | UP(io %d) RIGHT(io %d) DOWN(io %d) LEFT(io %d)\n",
           buttons_giro(), buttons_io(BTN_UP), buttons_io(BTN_RIGHT),
           buttons_io(BTN_DOWN), buttons_io(BTN_LEFT));
    printf("si el teclado radial se siente girado, proba 'joystick %d'\n",
           (buttons_giro() + 1) & 3);
    return 0;
}

/* Inyecta pulsaciones sin dedos. Sirve para probar el menu y el pager desde la
   PC: la placa ve la pulsacion por el mismo latch que usa el GPIO, asi que se
   ejercita el mismo codigo de la UI. */
static int cmd_tecla(int argc, char **argv)
{
    if (argc < 2) {
        printf("Uso: tecla [KEY1|KEY2|KEY3|KEY4|CENTER|UP|DOWN|LEFT|RIGHT] [...]\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        const button_id_t b = buttons_buscar(argv[i]);
        if (b >= BTN_COUNT) {
            printf("no conozco '%s' (prob: KEY1 KEY3 CENTER UP)\n", argv[i]);
            return 1;
        }
        buttons_pulsar(b);
        printf("%s ", buttons_name(b));
        /* Separadas en el tiempo: si llegan juntas la UI las ve en el mismo sondeo
           y se queda con una sola (sondea cada 50 ms). */
        vTaskDelay(pdMS_TO_TICKS(180));
    }
    printf("\n");
    return 0;
}

void cli_start(void)
{
    static const esp_console_cmd_t cmds[] = {
        {.command = "send", .help = "Envía un mensaje cifrado a todos los peers", .hint = "<texto>", .func = cmd_send},
        {.command = "sendto", .help = "Envía un mensaje cifrado a un peer concreto", .hint = "<índice|MAC> <texto>", .func = cmd_sendto},
        {.command = "peers", .help = "Lista las sesiones seguras y sus huellas", .func = cmd_peers},
        {.command = "hello", .help = "Re-anuncia por broadcast para descubrir placas", .func = cmd_hello},
        {.command = "whoami", .help = "Muestra mi nick y MAC", .func = cmd_whoami},
        {.command = "nick", .help = "Muestra o cambia tu nick (persistente, no depende de la MAC)", .hint = "[nuevo nick]", .func = cmd_nick},
        {.command = "free", .help = "Muestra RAM libre (heap interno/PSRAM)", .func = cmd_free},
        {.command = "grabar", .help = "Abre el micro; 'grabar' de nuevo lo cierra y manda la toma", .func = cmd_grabar},
        {.command = "texto", .help = "Muestra en el OLED un texto llegada en base64", .hint = "<base64>", .func = cmd_texto},
        {.command = "pantalla", .help = "Vuelca el OLED por consola (lo ve la PC)", .func = cmd_pantalla},
        {.command = "estado", .help = "Linea de estado para las tools de la PC", .func = cmd_estado},
        {.command = "mandar", .help = "Manda cifrado a los pares el borrador del menu", .func = cmd_mandar},
        {.command = "ui", .help = "Muestra o fuerza la pantalla del menu", .hint = "[idle|config|compose|dictado|borrar|aviso <texto>]", .func = cmd_ui},
        {.command = "tecla", .help = "Simula pulsaciones (KEY1..KEY4, CENTER, UP/DOWN/LEFT/RIGHT)", .hint = "<tecla> [...]", .func = cmd_tecla},
        {.command = "botones", .help = "Loguea cada presion con su GPIO (para el joystick)", .hint = "[on|off]", .func = cmd_botones},
        {.command = "teclas", .help = "Gira SOLO el selector de cuadrantes del teclado (0-3)", .hint = "[0|1|2|3]", .func = cmd_teclas},
        {.command = "joystick", .help = "Gira el joystick (0-3 pasos de 90 grados) y lo guarda", .hint = "[0|1|2|3]", .func = cmd_joystick},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
    {
        esp_err_t err = esp_console_cmd_register(&cmds[i]);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "No se pudo registrar '%s': %s", cmds[i].command, esp_err_to_name(err));
    }
    esp_console_register_help_command();

    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "e2ee> ";
    /* 256 alcanzaba para el mensajero, pero 'texto <base64>' de una frase de
       200 bytes necesita ~270 de payload: hay que poder mandarlo entero. */
    cfg.max_cmdline_length = 1024;
    /* Cada PALABRA es un argumento: con 20 y pico no alcanzaba para
       'ui aviso <frase larga>' y el REPL iba descartando argumentos de mas sin
       avisar (se caia el '--pag=N' del final). 48 args = 192 B mas de pila. */
    cfg.max_cmdline_args = 48;
    /* El drenado de una toma corre en la task del REPL (comando 'grabar') y
       transfer.c arma una linea de ~800 bytes en la pila: con el stack por
       defecto del REPL queda muy justo. */
    cfg.task_stack_size = 8192;

    esp_console_repl_t *repl = NULL;
    esp_err_t err = esp_console_new_repl_stdio(&cfg, &repl);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "REPL no disponible (%s); mensajería sigue activa", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Consola lista: escribe 'help'");
}
