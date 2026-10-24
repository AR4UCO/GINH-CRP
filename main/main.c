#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"  
#include "esp_mac.h"   

#include "buttons.h"
#include "display.h"

static const char *TAG = "ghost_messenger";

// ----------------------------------------------------
// DECLARACIÓN DEL TECLADO (Viene de teclado.c)
// ----------------------------------------------------
char* readline(void);

// ----------------------------------------------------
// VARIABLES GLOBALES 
// ----------------------------------------------------
typedef enum {
    APP_STATE_IDLE,
    APP_STATE_CONFIG,
    APP_STATE_COMPOSE
} app_state_t;

char my_alias[16] = "HACKER";
char msg_payload[32] = ""; 

// Variable global para guardar la MAC en formato texto
char mi_mac_str[20] = "LEYENDO..."; 

// ----------------------------------------------------
// TAREA EN SEGUNDO PLANO (ESP-NOW)
// ----------------------------------------------------
void esp_now_background_task(void *pvParameters) 
{
    ESP_LOGI("ESP-NOW", "Iniciando red en segundo plano...");
    
    // =========================================================
    // TODO: Tu compañero debe inicializar Wi-Fi y ESP-NOW AQUÍ
    // =========================================================

    // Leer la MAC (Asegúrate de hacerlo DESPUÉS de encender la antena con esp_wifi_start)
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(mi_mac_str, sizeof(mi_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", 
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    vTaskDelete(NULL); // Terminamos la tarea de inicialización
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

    // Iniciar la red en un hilo paralelo para que la UI no se quede en negro
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
                // --- PANTALLA 1: IDLE ---
                case APP_STATE_IDLE:
                    display_text(0, 0, "[ MOD: SNIFFER ]", false);
                    display_text(0, 8, "---------------------", false);
                    display_text(0, 24, " ESCUCHANDO RED...", false);
                    display_text(0, 56, "[SW1]Ver [SW3]Menu", true);
                    break;

                // --- PANTALLA 2: CONFIGURACIÓN ---
                case APP_STATE_CONFIG:
                    display_text(0, 0, "--- CONFIGURACION ---", false);
                    
                    // Mostrar Alias (con espaciado mejorado)
                    snprintf(buffer, sizeof(buffer), " ALIAS: %s", my_alias);
                    display_text(0, 20, buffer, false); 
                    
                    // Mostrar MAC Address
                    display_text(0, 36, " MAC:", false);
                    display_text(0, 44, mi_mac_str, false); 

                    // Footer
                    display_text(0, 56, "[SW1]Edit [SW3]Menu", true);
                    break;

                // --- PANTALLA 3: REDACTAR MENSAJE ---
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
            
            // Solo procesamos cuando el botón se presiona hacia abajo
            if (!ev.pressed) continue; 
            
            force_redraw = true;

            // Navegación principal del menú (SW3 cicla las pantallas)
            if (ev.id == BTN_KEY3) {
                current_state = (current_state + 1) % 3;
                continue;
            }

            // Lógica por pantalla
            switch (current_state) {
                case APP_STATE_IDLE:
                    // Aquí podrías agregar lógica si tocan botones mientras escuchan la red
                    break;
                    
                case APP_STATE_CONFIG:
                    if (ev.id == BTN_KEY1) { 
                        // Abrir teclado virtual
                        char *input = readline();
                        if (input != NULL) {
                            // Copiar de forma segura para no desbordar la memoria
                            strncpy(my_alias, input, sizeof(my_alias) - 1);
                            my_alias[sizeof(my_alias) - 1] = '\0';
                            
                            // IMPORTANTÍSIMO: Evitar memory leaks liberando el malloc
                            free(input);
                        }
                        force_redraw = true; 
                    }
                    break;
                    
                case APP_STATE_COMPOSE:
                    if (ev.id == BTN_KEY1) {
                        // Abrir teclado virtual
                        char *input = readline();
                        if (input != NULL) {
                            // Copiar de forma segura
                            strncpy(msg_payload, input, sizeof(msg_payload) - 1);
                            msg_payload[sizeof(msg_payload) - 1] = '\0';
                            
                            free(input);
                        }
                        force_redraw = true;
                    }
                    // TODO: Aquí podrías agregar un 'if (ev.id == BTN_KEY2)' para hacer el 'esp_now_send'
                    break;
            }
        }
    }
}