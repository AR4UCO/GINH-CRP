#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"  
#include "esp_now.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_event.h"

#include "buttons.h"
#include "display.h"

#include "e2ee.h"
#include "inbox.h"
#include "peer_pick.h"
#include "cli.h"

static const char *TAG = "ghost_ui";

// DECLARACIÓN DEL TECLADO
char* readline(void);

// VARIABLES GLOBALES (UI)
typedef enum {
    APP_STATE_IDLE,
    APP_STATE_CONFIG,
    APP_STATE_COMPOSE
} app_state_t;

char my_alias[E2EE_NICK_MAX + 1] = "HACKER";
char msg_payload[E2EE_MAX_PAYLOAD + 1] = ""; 
char mi_mac_str[20] = "LEYENDO..."; 

// Variables para mostrar el último mensaje recibido
static char last_sender[INBOX_SENDER_MAX] = "";
static char last_msg[INBOX_TEXT_MAX] = "";
static bool show_received_msg = false;

// EVENTOS Y CALLBACKS DE RED (Backend)
static QueueHandle_t s_rx_queue;
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct {
    uint8_t mac[6];
    int     len;
    uint8_t data[ESP_NOW_MAX_DATA_LEN];
} rx_event_t;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len <= 0 || len > ESP_NOW_MAX_DATA_LEN || !s_rx_queue) return;
    rx_event_t evt;
    memcpy(evt.mac, info->src_addr, 6);
    evt.len = len;
    memcpy(evt.data, data, len);
    xQueueSend(s_rx_queue, &evt, 0);
}

static void on_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {}

static void rx_worker(void *arg) {
    rx_event_t evt;
    while (xQueueReceive(s_rx_queue, &evt, portMAX_DELAY) == pdTRUE) {
        e2ee_handle_packet(evt.mac, evt.data, evt.len);
    }
}

static void on_secure_data(const uint8_t mac[6], const uint8_t *payload, size_t len) {
    char name[32];
    if (!e2ee_lookup_name(mac, name, sizeof(name)))
        snprintf(name, sizeof(name), MACSTR, MAC2STR(mac));
    
    // Guardar en el buzón seguro
    inbox_post(name, (const char *)payload, len);
}

// ----------------------------------------------------
// TAREA EN SEGUNDO PLANO (ESP-NOW + E2EE)
// ----------------------------------------------------
void esp_now_background_task(void *pvParameters) 
{
    ESP_LOGI("ESP-NOW", "Iniciando red y criptografía...");

    // 1. Inicializar Memoria (NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 2. Inicializar Wi-Fi
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    // 3. Inicializar ESP-NOW
    esp_now_init();
    esp_now_register_recv_cb(on_recv);
    esp_now_register_send_cb(on_send);

    esp_now_peer_info_t broadcast_peer = {};
    memcpy(broadcast_peer.peer_addr, BROADCAST_MAC, 6);
    broadcast_peer.channel = 1;
    esp_now_add_peer(&broadcast_peer);

    // 4. Inicializar E2EE (Criptografía) y Buzón
    inbox_init();
    ESP_ERROR_CHECK(e2ee_init());
    e2ee_set_rx_callback(on_secure_data);

    // Arrancar el hilo que procesa los descifrados
    s_rx_queue = xQueueCreate(8, sizeof(rx_event_t));
    xTaskCreate(rx_worker, "e2ee_rx", 4096, NULL, 5, NULL);
    
    // Arrancar consola por Serial (por si la conectan a la PC)
    cli_start();

    // 5. Cargar datos locales a la UI
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(mi_mac_str, sizeof(mi_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", 
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    
    // Carga el nick real desde la NVS (sobrescribe "HACKER" si ya se configuró antes)
    e2ee_get_self(NULL, my_alias, sizeof(my_alias));

    // 6. Bucle de Discovery (HELLO Broadcast)
    while(1) {
        e2ee_send_hello(BROADCAST_MAC);
        vTaskDelay(pdMS_TO_TICKS(10000)); // Enviamos latido cada 10 segundos
    }
}

// ----------------------------------------------------
// CONTROLADOR PRINCIPAL (UI Loop)
// ----------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando sistema OS");

    bool display_ok = display_init() == ESP_OK;
    ESP_ERROR_CHECK(buttons_init());

    xTaskCreate(esp_now_background_task, "net_task", 6144, NULL, 5, NULL);

    QueueHandle_t events = buttons_event_queue();
    app_state_t current_state = APP_STATE_IDLE;
    bool force_redraw = true;

    while (1) {
        
        // --- POLLING DEL BUZÓN E2EE ---
        // Revisamos periódicamente si el hilo de red nos dejó un mensaje
        if (inbox_pending()) {
            inbox_take(last_sender, sizeof(last_sender), last_msg, sizeof(last_msg));
            show_received_msg = true;
            force_redraw = true; // Forzar repintado para mostrar la alerta
        }

        // --- 1. RENDERIZADO DE UX ---
        if (force_redraw && display_ok) {
            display_clear();
            char buffer[40]; 

            switch (current_state) {
                // --- PANTALLA 1: IDLE / MENSAJES ---
                case APP_STATE_IDLE:
                    if (show_received_msg) {
                        // Alerta Cyberpunk
                        display_text(0, 0, "[!] INTERCEPTADO:", true); // Titulo invertido
                        display_text(0, 10, last_sender, false);
                        
                        // Dividimos el texto en 3 lineas para que entre bien
                        char l1[22]={0}, l2[22]={0}, l3[22]={0};
                        strncpy(l1, last_msg, 21);
                        if(strlen(last_msg) > 21) strncpy(l2, last_msg + 21, 21);
                        if(strlen(last_msg) > 42) strncpy(l3, last_msg + 42, 21);
                        
                        display_text(0, 24, l1, false);
                        display_text(0, 32, l2, false);
                        display_text(0, 40, l3, false);
                        
                        display_text(0, 56, "[SW1]Borrar [SW3]Menu", true);
                    } else {
                        display_text(0, 0, "[ MOD: SNIFFER ]", false);
                        display_text(0, 8, "---------------------", false);
                        display_text(0, 24, " ESCUCHANDO RED...", false);
                        
                        int conectados = e2ee_established_count();
                        snprintf(buffer, sizeof(buffer), " SESIONES: %d", conectados);
                        display_text(0, 40, buffer, false);

                        display_text(0, 56, "[SW1]---    [SW3]Menu", true);
                    }
                    break;

                // --- PANTALLA 2: CONFIGURACIÓN ---
                case APP_STATE_CONFIG:
                    display_text(0, 0, "--- CONFIGURACION ---", false);
                    snprintf(buffer, sizeof(buffer), " ALIAS: %s", my_alias);
                    display_text(0, 20, buffer, false); 
                    display_text(0, 36, " MAC:", false);
                    display_text(0, 44, mi_mac_str, false); 
                    display_text(0, 56, "[SW1]Edit [SW3]Menu", true);
                    break;

                // --- PANTALLA 3: REDACTAR MENSAJE ---
                case APP_STATE_COMPOSE:
                    display_text(0, 0, "--- BROADCAST ---", false);
                    
                    // Mostrar preview del mensaje escrito
                    char l_prev[22]={0};
                    strncpy(l_prev, msg_payload, 21);
                    snprintf(buffer, sizeof(buffer), "> %s", l_prev);
                    display_text(0, 24, buffer, false);
                    
                    display_text(0, 48, "---------------------", false);
                    display_text(0, 56, "[1]Kyb [2]Tx [3]Menu", true); // SW2 es Tx (Transmitir)
                    break;
            }
            display_flush();
            force_redraw = false;
        }

        // --- 2. PROCESAMIENTO DE EVENTOS ---
        button_event_t ev;
        // Se desbloquea cada 500ms para permitir revisar el inbox_pending arriba
        if (xQueueReceive(events, &ev, pdMS_TO_TICKS(500)) == pdTRUE) {
            
            if (!ev.pressed) continue; 
            force_redraw = true;

            if (ev.id == BTN_KEY3) {
                current_state = (current_state + 1) % 3;
                continue;
            }

            switch (current_state) {
                case APP_STATE_IDLE:
                    if (ev.id == BTN_KEY1 && show_received_msg) {
                        // Borra el mensaje y vuelve al sniffer
                        show_received_msg = false; 
                    }
                    break;
                    
                case APP_STATE_CONFIG:
                    if (ev.id == BTN_KEY1) { 
                        char *input = readline();
                        if (input != NULL) {
                            // Actualizar alias propio y notificar al backend para la NVS
                            e2ee_set_nick(input); 
                            e2ee_get_self(NULL, my_alias, sizeof(my_alias));
                            free(input);
                        }
                        force_redraw = true; 
                    }
                    break;
                    
                case APP_STATE_COMPOSE:
                    if (ev.id == BTN_KEY1) {
                        char *input = readline();
                        if (input != NULL) {
                            strncpy(msg_payload, input, sizeof(msg_payload) - 1);
                            msg_payload[sizeof(msg_payload) - 1] = '\0';
                            free(input);
                        }
                        force_redraw = true;
                    }
                    // BOTÓN TX: Enviar mensaje cifrado!
                    if (ev.id == BTN_KEY2 && strlen(msg_payload) > 0) {
                        uint8_t dest[6];
                        // Abre el selector de la placa destino (hecho por tu compañero)
                        if (peer_pick(dest)) { 
                            e2ee_send(dest, (const uint8_t *)msg_payload, strlen(msg_payload));
                            // Limpiar payload despues de enviar
                            msg_payload[0] = '\0';
                            current_state = APP_STATE_IDLE; // Vuelve a la pantalla principal
                        }
                        force_redraw = true;
                    }
                    break;
            }
        }
    }
}