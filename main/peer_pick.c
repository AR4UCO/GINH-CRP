#include <stdio.h>
#include <string.h>
#include "peer_pick.h"
#include "e2ee.h"
#include "display.h"
#include "buttons.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PICK_WINDOW 5

bool peer_pick(uint8_t mac_out[6])
{
    int n = e2ee_established_count();

    if (n == 0)
    {
        display_clear();
        display_text(0, 0, "Sin sesion", false);
        display_text(0, 8, "segura", false);
        display_text(0, 40, "Esperando peer...", false);
        display_flush();
        vTaskDelay(pdMS_TO_TICKS(1500));
        return false;
    }

    if (n == 1)
    {
        e2ee_peer_info_t p;
        if (e2ee_get_peer_info(0, &p))
        {
            memcpy(mac_out, p.mac, 6);
            return true;
        }
        return false;
    }

    int sel = 0;
    uint32_t prev = buttons_read();

    for (;;)
    {
        display_clear();
        display_text(0, 0, "Enviar a:", false);

        int first = sel - PICK_WINDOW / 2;
        if (first < 0)
            first = 0;
        if (n > PICK_WINDOW && first > n - PICK_WINDOW)
            first = n - PICK_WINDOW;

        for (int i = first; i < n && i < first + PICK_WINDOW; i++)
        {
            e2ee_peer_info_t p;
            if (!e2ee_get_peer_info(i, &p))
                continue;
            char line[24];
            snprintf(line, sizeof(line), "%c%.20s", i == sel ? '>' : ' ', p.name);
            display_text(0, 10 + (i - first) * 8, line, i == sel);
        }

        display_text(0, 56, "K4=ok K3=salir", false);
        display_flush();

        vTaskDelay(pdMS_TO_TICKS(20));
        uint32_t now = buttons_read();
        uint32_t edge = now & ~prev;
        prev = now;

        if (edge & ((1u << BTN_UP) | (1u << BTN_LEFT)))
        {
            sel = (sel + n - 1) % n;
        }
        else if (edge & ((1u << BTN_DOWN) | (1u << BTN_RIGHT)))
        {
            sel = (sel + 1) % n;
        }
        else if (edge & (1u << BTN_KEY4))
        {
            e2ee_peer_info_t p;
            if (!e2ee_get_peer_info(sel, &p))
                return false;
            memcpy(mac_out, p.mac, 6);
            return true;
        }
        else if (edge & (1u << BTN_KEY3))
        {
            return false;
        }
    }
}
