/*
 * E2EE sobre ESP-NOW
 * ------------------
 * Intercambio de claves efímeras X25519 (ECDH) en el arranque + AES-256-GCM
 * por par de placas. Protección contra replay con contador monotónico.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Payload máximo de aplicación por mensaje:
 * 250 (ESP-NOW) - 2 (cabecera) - 12 (nonce) - 8 (seq) - 16 (tag GCM) = 212 */
#define E2EE_MAX_PAYLOAD 212

/* Callback de aplicación: datos ya descifrados y autenticados. */
typedef void (*e2ee_rx_cb_t)(const uint8_t src_mac[6], const uint8_t *payload, size_t len);

/* Inicializa PSA crypto y genera el par X25519 efímero de este arranque.
 * Llamar DESPUÉS de esp_wifi_start(). */
esp_err_t e2ee_init(void);

void e2ee_set_rx_callback(e2ee_rx_cb_t cb);

/* Envía HELLO (nuestra clave pública efímera) a un peer o a broadcast. */
esp_err_t e2ee_send_hello(const uint8_t mac[6]);

/* Cifra y envía datos de aplicación a un peer con sesión establecida. */
esp_err_t e2ee_send(const uint8_t mac[6], const uint8_t *data, size_t len);

/* Procesa un paquete ESP-NOW recibido.
 * Llamar desde la tarea trabajadora, NO desde el callback de WiFi. */
void e2ee_handle_packet(const uint8_t src_mac[6], const uint8_t *data, int len);

/* Número de sesiones seguras establecidas. */
int e2ee_established_count(void);

/* MAC del peer establecido nº idx (para round-robin). false si no existe. */
bool e2ee_get_peer_mac(int idx, uint8_t mac[6]);
