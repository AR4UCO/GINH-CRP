#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "buttons.h"
#include "display.h"

static const char *TAG = "ghost_messenger";

// ----------------------------------------------------
// VARIABLES GLOBALES (Para el teclado virtual)
// ----------------------------------------------------
typedef enum {
    APP_STATE_IDLE,
    APP_STATE_CONFIG,
    APP_STATE_COMPOSE
} app_state_t;

char my_alias[16] = "HACKER";
char msg_payload[32] = ""; 
int esp_now_channel = 1;

// ----------------------------------------------------
// TAREA EN SEGUNDO PLANO (ESP-NOW)
// ----------------------------------------------------
void esp_now_background_task(void *pvParameters) 
{
    ESP_LOGI("ESP-NOW", "Iniciando red en segundo plano...");
    
    // --- ATENCIÓN ---
    // Dile a tu compañero que pegue toda la lógica de inicialización de
    // Wi-Fi y el "discovery" de ESP-NOW AQUÍ ADENTRO. 
    // De esta forma, mientras la placa busca redes, la pantalla sigue viva.

    vTaskDelete(NULL); // Terminar hilo de setup
}

// ----------------------------------------------------
// CONTROLADOR PRINCIPAL
// ----------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando nodo ESP-NOW Ghost in the Hal");

    // Inicialización de hardware
    bool display_ok = display_init() == ESP_OK;
    ESP_ERROR_CHECK(buttons_init());

    // Iniciar la red en un núcleo/hilo paralelo para no colgar la UI
    xTaskCreate(esp_now_background_task, "esp_now_task", 4096, NULL, 5, NULL);

    QueueHandle_t events = buttons_event_queue();
    app_state_t current_state = APP_STATE_IDLE;
    bool force_redraw = true;

    while (1) {
        // --- 1. RENDERIZADO DE UX ---
        if (force_redraw && display_ok) {
            display_clear();
            char buffer[40]; 

            switch (current_state) {
                case APP_STATE_IDLE:
                    display_text(0, 0, "[ MOD: SNIFFER ]", false);
                    display_text(0, 8, "---------------------", false);
                    display_text(0, 24, " ESCUCHANDO RED...", false);
                    display_text(0, 56, "[SW1]Ver [SW3]Menu", true);
                    break;

                case APP_STATE_CONFIG:
                    display_text(0, 0, "--- CONFIGURACION ---", false);
                    snprintf(buffer, sizeof(buffer), " ALIAS: %s", my_alias);
                    display_text(0, 24, buffer, false); 
                    snprintf(buffer, sizeof(buffer), " CANAL: %03d", esp_now_channel);
                    display_text(0, 40, buffer, false);
                    display_text(0, 56, "[SW1]Edit [SW3]Menu", true);
                    break;

                case APP_STATE_COMPOSE:
                    display_text(0, 0, "--- NUEVO MENSAJE ---", false);
                    snprintf(buffer, sizeof(buffer), "> %s", msg_payload);
                    display_text(0, 24, buffer, false);
                    display_text(0, 48, "---------------------", false);
                    display_text(0, 56, "[SW1]Kyb [SW3]Menu", true);
                    break;
            }
            display_flush();
            force_redraw = false;
        }

        // --- 2. PROCESAMIENTO DE EVENTOS ---
        button_event_t ev;
        if (xQueueReceive(events, &ev, portMAX_DELAY) == pdTRUE) {
            
            if (!ev.pressed) continue; 
            force_redraw = true;

            if (ev.id == BTN_KEY3) {
                current_state = (current_state + 1) % 3;
                continue;
            }

            switch (current_state) {
                case APP_STATE_IDLE:
                    break;
                case APP_STATE_CONFIG:
                    if (ev.id == BTN_KEY1) {
                        // Abrir teclado virtual (alias)
                    }
                    break;
                case APP_STATE_COMPOSE:
                    if (ev.id == BTN_KEY1) {
                        // Abrir teclado virtual (mensaje)
                    }
                    break;
            }
        }
    }
}