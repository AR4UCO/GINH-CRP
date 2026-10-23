/*
 * ESP-NOW Auto-Discovery por proximidad
 * 
 * Mismo firmware para todos los ESP.
 * Se descubren solos por broadcast, sin hardcodear MACs.
 */

#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ESP-NOW-DISCOVER";

// ---------- Tipos de mensaje ----------
typedef enum {
    MSG_DISCOVERY = 1,
    MSG_DISCOVERY_REPLY = 2,
    MSG_DATA = 3,
} msg_type_t;

typedef struct {
    msg_type_t type;
    char nombre[16];
} __attribute__((packed)) espnow_msg_t;

// MAC de broadcast
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---------- Agregar peer si no existe ----------
static void add_peer_if_new(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) return;

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    ESP_LOGI(TAG, "Nuevo peer: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ---------- Callback de recepcion ----------
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < sizeof(espnow_msg_t)) return;

    const espnow_msg_t *msg = (const espnow_msg_t *)data;
    const uint8_t *mac = info->src_addr;

    switch (msg->type) {
        case MSG_DISCOVERY:
            ESP_LOGI(TAG, "Discovery de %02X:%02X:%02X:%02X:%02X:%02X (%s)",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], msg->nombre);

            // Agregar como peer
            add_peer_if_new(mac);

            // Responder para que nos conozca
            espnow_msg_t reply = { .type = MSG_DISCOVERY_REPLY };
            snprintf(reply.nombre, sizeof(reply.nombre), "ESP-%02X%02X", mac[4], mac[5]);
            esp_now_send(mac, (uint8_t *)&reply, sizeof(reply));
            break;

        case MSG_DISCOVERY_REPLY:
            ESP_LOGI(TAG, "Reply de %02X:%02X:%02X:%02X:%02X:%02X (%s)",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], msg->nombre);
            add_peer_if_new(mac);
            break;

        case MSG_DATA:
            ESP_LOGI(TAG, "Datos de %02X:%02X:%02X:%02X:%02X:%02X: %s",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     msg->nombre);
            break;
    }
}

// ---------- Callback de envio ----------
static void on_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "Envio fallido");
    }
}

// ---------- Inicializar WiFi + ESP-NOW ----------
static void init_espnow(void)
{
    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // WiFi
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    // ESP-NOW
    esp_now_init();
    esp_now_register_recv_cb(on_recv);
    esp_now_register_send_cb(on_send);

    // Peer de broadcast
    esp_now_peer_info_t broadcast_peer = {};
    memcpy(broadcast_peer.peer_addr, BROADCAST_MAC, 6);
    esp_now_add_peer(&broadcast_peer);
}

void app_main(void)
{
    init_espnow();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "Mi MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    espnow_msg_t disc = { .type = MSG_DISCOVERY };
    snprintf(disc.nombre, sizeof(disc.nombre), "ESP-%02X%02X", mac[4], mac[5]);

    for (int i = 0; i < 5; i++) {
        ESP_LOGI(TAG, "Enviando discovery %d/5...", i + 1);
        esp_now_send(BROADCAST_MAC, (uint8_t *)&disc, sizeof(disc));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Discovery completo. Listo para comunicar.");

    int contador = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        contador++;
        ESP_LOGI(TAG, "Re-discovery #%d...", contador);
        esp_now_send(BROADCAST_MAC, (uint8_t *)&disc, sizeof(disc));
    }
}
