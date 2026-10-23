/*
 * ESP-NOW E2EE — Auto-descubrimiento + cifrado extremo a extremo
 *
 * Mismo firmware para todos los ESP32-S3.
 *  1. Al arrancar, cada placa genera un par X25519 efímero y lo anuncia
 *     por broadcast (HELLO).
 *  2. Cada par de placas deriva su propia clave AES-256-GCM (ECDH + KDF),
 *     sin claves grabadas en el firmware (TOFU: primera clave vista gana).
 *  3. Los datos viajan cifrados y autenticados; la huella del par se
 *     imprime por serie para verificación manual.
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
#include "freertos/queue.h"
#include "e2ee.h"
#include "cli.h"

static const char *TAG = "ESP-NOW-E2EE";

// MAC de broadcast
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---------- Cola de recepción: el callback de WiFi solo encola ----------
typedef struct {
    uint8_t mac[6];
    int     len;
    uint8_t data[ESP_NOW_MAX_DATA_LEN];
} rx_event_t;

static QueueHandle_t s_rx_queue;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len <= 0 || len > ESP_NOW_MAX_DATA_LEN) return;
    if (s_rx_queue == NULL) return;

    rx_event_t evt;
    memcpy(evt.mac, info->src_addr, 6);
    evt.len = len;
    memcpy(evt.data, data, len);

    if (xQueueSend(s_rx_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Cola RX llena, paquete descartado");
    }
}

static void on_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "Envio fallido");
    }
}

// Tarea trabajadora: aquí va toda la criptografía (fuera del callback de WiFi)
static void rx_worker(void *arg)
{
    rx_event_t evt;
    while (xQueueReceive(s_rx_queue, &evt, portMAX_DELAY) == pdTRUE) {
        e2ee_handle_packet(evt.mac, evt.data, evt.len);
    }
}

// ---------- Datos de aplicación ya descifrados ----------
static void on_secure_data(const uint8_t mac[6], const uint8_t *payload, size_t len)
{
    char name[32];
    if (!e2ee_lookup_name(mac, name, sizeof(name)))
        snprintf(name, sizeof(name), MACSTR, MAC2STR(mac));

    ESP_LOGI(TAG, "MSG de %s: %.*s", name, (int)len, payload);
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
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);  // todos en el mismo canal

    // ESP-NOW
    esp_now_init();
    esp_now_register_recv_cb(on_recv);
    esp_now_register_send_cb(on_send);

    // Peer de broadcast (sin cifrar: lo cifra la capa de aplicación)
    esp_now_peer_info_t broadcast_peer = {};
    memcpy(broadcast_peer.peer_addr, BROADCAST_MAC, 6);
    broadcast_peer.channel = 1;
    esp_now_add_peer(&broadcast_peer);
}

void app_main(void)
{
    // Cola ANTES de arrancar ESP-NOW (para no perder paquetes).
    // La tarea se crea después de e2ee_init: los paquetes que lleguen
    // mientras tanto simplemente esperan en la cola.
    s_rx_queue = xQueueCreate(8, sizeof(rx_event_t));

    init_espnow();

    ESP_ERROR_CHECK(e2ee_init());
    e2ee_set_rx_callback(on_secure_data);

    xTaskCreate(rx_worker, "e2ee_rx", 4096, NULL, 5, NULL);

    // Consola interactiva: send / sendto / peers / nick / hello / whoami
    cli_start();

    // Anuncio inicial: 5 HELLO por broadcast (1 por segundo)
    for (int i = 0; i < 5; i++) {
        ESP_LOGI(TAG, "Enviando HELLO %d/5...", i + 1);
        e2ee_send_hello(BROADCAST_MAC);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Listo. 'send <texto>' para enviar cifrado, 'help' para comandos.");

    // Re-anuncio periódico: si ya hay sesión, los peers lo ignoran en silencio.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        e2ee_send_hello(BROADCAST_MAC);
    }
}
