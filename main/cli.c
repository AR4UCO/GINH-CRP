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
#include <stdlib.h>
#include <string.h>
#include "cli.h"
#include "e2ee.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"

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
    cfg.max_cmdline_length = 256;
    cfg.max_cmdline_args = 24;

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
