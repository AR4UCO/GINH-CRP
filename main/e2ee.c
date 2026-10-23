
#include <stdio.h>
#include <string.h>
#include "e2ee.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "E2EE";

#define E2E_VERSION 0x01
#define PKT_HELLO 0x01
#define PKT_DATA 0x02

#define X25519_PUB_LEN 32
#define BOOT_NONCE_LEN 8
#define NAME_LEN 8
#define KEY_LEN 32
#define SALT_LEN 4
#define NONCE_LEN 12
#define TAG_LEN 16
#define SEQ_LEN 8
#define MAX_PEERS 8

#define HELLO_PKT_LEN (2 + X25519_PUB_LEN + BOOT_NONCE_LEN + NAME_LEN) // 50
#define KDF_INFO_LEN (14 + 12 + 16)                                    // etiqueta + 2 MACs + 2 boot_nonces

typedef struct
{
    bool used;
    uint8_t mac[6];
    psa_key_id_t key; // clave AES-256 de la sesión (0 = vacía)
    uint8_t salt[SALT_LEN];
    uint64_t tx_seq;                  // próximo seq de envío
    uint64_t rx_seq;                  // último seq aceptado del peer
    uint8_t peer_pub[X25519_PUB_LEN]; // pub con la que se derivó key
    uint8_t peer_nonce[BOOT_NONCE_LEN];
    uint8_t fp[8]; // huella corta del par
    char name[NAME_LEN + 1];
} session_t;

static session_t s_peers[MAX_PEERS];
static SemaphoreHandle_t s_lock;
static psa_key_id_t s_x25519; // par efímero de este arranque
static uint8_t s_our_pub[X25519_PUB_LEN];
static uint8_t s_our_nonce[BOOT_NONCE_LEN];
static uint8_t s_our_mac[6];
static char s_our_name[NAME_LEN + 1];
static e2ee_rx_cb_t s_rx_cb;

// MAC2STR y MACSTR vienen de esp_mac.h

// ---------- utilidades ----------

static inline void put_be64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--)
    {
        p[i] = v & 0xFF;
        v >>= 8;
    }
}

static inline uint64_t get_be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static bool is_broadcast(const uint8_t mac[6])
{
    return memcmp(mac, "\xFF\xFF\xFF\xFF\xFF\xFF", 6) == 0;
}

static void add_espnow_peer(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac))
        return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false; // el cifrado es de capa de aplicación (este módulo)
    esp_now_add_peer(&peer);
}

// ---------- tabla de sesiones (llamar con s_lock tomado) ----------

static session_t *session_find(const uint8_t mac[6])
{
    for (int i = 0; i < MAX_PEERS; i++)
    {
        if (s_peers[i].used && memcmp(s_peers[i].mac, mac, 6) == 0)
            return &s_peers[i];
    }
    return NULL;
}

static session_t *session_alloc(void)
{
    for (int i = 0; i < MAX_PEERS; i++)
    {
        if (!s_peers[i].used)
        {
            memset(&s_peers[i], 0, sizeof(session_t));
            s_peers[i].used = true;
            return &s_peers[i];
        }
    }
    // Tabla llena: expulsar el primer slot (uso de laboratorio; avisar)
    ESP_LOGW(TAG, "Tabla de peers llena, expulsando slot 0");
    if (s_peers[0].key != 0)
        psa_destroy_key(s_peers[0].key);
    memset(&s_peers[0], 0, sizeof(session_t));
    s_peers[0].used = true;
    return &s_peers[0];
}

// ---------- criptografía ----------

/* ConcatKDF con SHA-256 (SP 800-56A):
 *   bloque_i = SHA256( i_be32 || shared || info )  -> 32 bytes por bloque */
static esp_err_t kdf_derive(const uint8_t shared[KEY_LEN],
                            const uint8_t mac_peer[6], const uint8_t nonce_peer[BOOT_NONCE_LEN],
                            uint8_t key_out[KEY_LEN], uint8_t salt_out[SALT_LEN])
{
    // info ordenado por MAC para que ambos lados construyan lo mismo
    uint8_t info[KDF_INFO_LEN];
    memcpy(info, "espnow-e2ee-v1", 14);
    if (memcmp(s_our_mac, mac_peer, 6) < 0)
    {
        memcpy(info + 14, s_our_mac, 6);
        memcpy(info + 20, mac_peer, 6);
        memcpy(info + 26, s_our_nonce, 8);
        memcpy(info + 34, nonce_peer, 8);
    }
    else
    {
        memcpy(info + 14, mac_peer, 6);
        memcpy(info + 20, s_our_mac, 6);
        memcpy(info + 26, nonce_peer, 8);
        memcpy(info + 34, s_our_nonce, 8);
    }

    uint8_t buf[4 + KEY_LEN + KDF_INFO_LEN];
    memcpy(buf + 4, shared, KEY_LEN);
    memcpy(buf + 36, info, KDF_INFO_LEN);

    uint8_t out[KEY_LEN];
    size_t olen;
    buf[0] = 0;
    buf[1] = 0;
    buf[2] = 0;
    buf[3] = 1;
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, buf, sizeof(buf), out, sizeof(out), &olen);
    if (st != PSA_SUCCESS)
        goto fail;
    memcpy(key_out, out, KEY_LEN);

    buf[3] = 2;
    st = psa_hash_compute(PSA_ALG_SHA_256, buf, sizeof(buf), out, sizeof(out), &olen);
    if (st != PSA_SUCCESS)
        goto fail;
    memcpy(salt_out, out, SALT_LEN);

    memset(buf, 0, sizeof(buf));
    memset(out, 0, sizeof(out));
    return ESP_OK;
fail:
    memset(buf, 0, sizeof(buf));
    memset(out, 0, sizeof(out));
    return ESP_FAIL;
}

/* Deriva e instala la clave de sesión con un peer (llamar con s_lock tomado). */
static esp_err_t session_establish(session_t *s, const uint8_t peer_pub[X25519_PUB_LEN],
                                   const uint8_t peer_nonce[BOOT_NONCE_LEN])
{
    uint8_t shared[KEY_LEN];
    size_t olen;
    psa_status_t st = psa_raw_key_agreement(PSA_ALG_ECDH, s_x25519,
                                            peer_pub, X25519_PUB_LEN,
                                            shared, sizeof(shared), &olen);
    if (st != PSA_SUCCESS || olen != KEY_LEN)
    {
        ESP_LOGW(TAG, "ECDH falló (%d)", (int)st);
        return ESP_FAIL;
    }

    // Rechazo de puntos de orden bajo (claves públicas maliciosas/degeneradas)
    uint8_t acc = 0;
    for (int i = 0; i < KEY_LEN; i++)
        acc |= shared[i];
    if (acc == 0)
    {
        ESP_LOGW(TAG, "ECDH: secreto débil rechazado");
        memset(shared, 0, sizeof(shared));
        return ESP_FAIL;
    }

    uint8_t key[KEY_LEN];
    esp_err_t err = kdf_derive(shared, s->mac, peer_nonce, key, s->salt);
    memset(shared, 0, sizeof(shared));
    if (err != ESP_OK)
        return err;

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_key_id_t new_key = 0;
    st = psa_import_key(&attr, key, KEY_LEN, &new_key);
    memset(key, 0, sizeof(key));
    if (st != PSA_SUCCESS)
    {
        ESP_LOGW(TAG, "psa_import_key falló (%d)", (int)st);
        return ESP_FAIL;
    }

    if (s->key != 0)
        psa_destroy_key(s->key); // re-negociación: sustituir
    s->key = new_key;
    s->tx_seq = 0;
    s->rx_seq = 0;
    return ESP_OK;
}

/* Huella corta idéntica en ambas placas (comparar por consola/serial). */
static void compute_fingerprint(const uint8_t mac_peer[6], const uint8_t peer_pub[X25519_PUB_LEN],
                                uint8_t out[8])
{
    uint8_t buf[6 + 6 + 32 + 32];
    if (memcmp(s_our_mac, mac_peer, 6) < 0)
    {
        memcpy(buf, s_our_mac, 6);
        memcpy(buf + 6, mac_peer, 6);
        memcpy(buf + 12, s_our_pub, 32);
        memcpy(buf + 44, peer_pub, 32);
    }
    else
    {
        memcpy(buf, mac_peer, 6);
        memcpy(buf + 6, s_our_mac, 6);
        memcpy(buf + 12, peer_pub, 32);
        memcpy(buf + 44, s_our_pub, 32);
    }
    uint8_t hash[32];
    size_t olen;
    psa_hash_compute(PSA_ALG_SHA_256, buf, sizeof(buf), hash, sizeof(hash), &olen);
    memcpy(out, hash, 8);
}

static void log_fingerprint(const uint8_t mac_peer[6], const uint8_t fp[8])
{
    ESP_LOGI(TAG, "Huella del par " MACSTR ": %02X%02X%02X%02X-%02X%02X%02X%02X",
             MAC2STR(mac_peer),
             fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], fp[6], fp[7]);
}

// ---------- API pública ----------

esp_err_t e2ee_init(void)
{
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "psa_crypto_init falló (%d)", (int)st);
        return ESP_FAIL;
    }

    esp_read_mac(s_our_mac, ESP_MAC_WIFI_STA);
    snprintf(s_our_name, sizeof(s_our_name), "ESP-%02X%02X", s_our_mac[4], s_our_mac[5]);
    esp_fill_random(s_our_nonce, sizeof(s_our_nonce));

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return ESP_ERR_NO_MEM;

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attr, 255);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
    st = psa_generate_key(&attr, &s_x25519);
    if (st != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "X25519 keygen falló (%d)", (int)st);
        return ESP_FAIL;
    }
    size_t olen;
    st = psa_export_public_key(s_x25519, s_our_pub, sizeof(s_our_pub), &olen);
    if (st != PSA_SUCCESS || olen != X25519_PUB_LEN)
        return ESP_FAIL;

    ESP_LOGI(TAG, "E2EE listo. Soy %s (" MACSTR ")", s_our_name, MAC2STR(s_our_mac));
    return ESP_OK;
}

void e2ee_set_rx_callback(e2ee_rx_cb_t cb)
{
    s_rx_cb = cb;
}

esp_err_t e2ee_send_hello(const uint8_t mac[6])
{
    uint8_t pkt[HELLO_PKT_LEN];
    pkt[0] = E2E_VERSION;
    pkt[1] = PKT_HELLO;
    memcpy(pkt + 2, s_our_pub, X25519_PUB_LEN);
    memcpy(pkt + 34, s_our_nonce, BOOT_NONCE_LEN);
    memcpy(pkt + 42, s_our_name, NAME_LEN); // cabe: "ESP-XXXX\0" = 8

    if (!is_broadcast(mac))
        add_espnow_peer(mac);
    return esp_now_send(mac, pkt, sizeof(pkt));
}

esp_err_t e2ee_send(const uint8_t mac[6], const uint8_t *data, size_t len)
{
    if (len > E2EE_MAX_PAYLOAD)
        return ESP_ERR_INVALID_SIZE;

    // Los DATOS siempre van unicast. Solo HELLO puede ir a broadcast.
    // (Evita que una MAC mal pasada difunda un mensaje a todas las placas)
    if (is_broadcast(mac))
    {
        ESP_LOGW(TAG, "e2ee_send rechaza destino broadcast");
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    session_t *s = session_find(mac);
    if (!s || s->key == 0)
    {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND; // sin sesión: la app puede enviar HELLO primero
    }

    s->tx_seq++;
    uint64_t seq = s->tx_seq;

    // nonce = salt || seq_be, con bit de dirección en el salt:
    // ambas direcciones comparten clave y ambas empiezan su seq en 1,
    // así que se separan los espacios de nonce según el orden de las MACs
    // (en GCM, repetir (clave, nonce) con distinto texto sería catastrófico).
    uint8_t nonce[NONCE_LEN];
    memcpy(nonce, s->salt, SALT_LEN);
    if (memcmp(s_our_mac, s->mac, 6) > 0)
        nonce[SALT_LEN - 1] ^= 0x80;
    put_be64(nonce + SALT_LEN, seq);

    uint8_t ptext[SEQ_LEN + E2EE_MAX_PAYLOAD];
    put_be64(ptext, seq);
    memcpy(ptext + SEQ_LEN, data, len);

    uint8_t pkt[2 + NONCE_LEN + SEQ_LEN + E2EE_MAX_PAYLOAD + TAG_LEN]; // 250 máx
    pkt[0] = E2E_VERSION;
    pkt[1] = PKT_DATA;
    memcpy(pkt + 2, nonce, NONCE_LEN);

    uint8_t aad[8];
    memcpy(aad, s_our_mac, 6);
    aad[6] = E2E_VERSION;
    aad[7] = PKT_DATA;

    size_t olen = 0;
    psa_status_t st = psa_aead_encrypt(s->key, PSA_ALG_GCM,
                                       nonce, sizeof(nonce),
                                       aad, sizeof(aad),
                                       ptext, SEQ_LEN + len,
                                       pkt + 14, sizeof(pkt) - 14, &olen);
    xSemaphoreGive(s_lock);
    memset(ptext, 0, sizeof(ptext));

    if (st != PSA_SUCCESS)
    {
        ESP_LOGW(TAG, "aead_encrypt falló (%d)", (int)st);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "TX unicast " MACSTR " seq=%llu (%u bytes)",
             MAC2STR(mac), (unsigned long long)seq, (unsigned)len);
    return esp_now_send(mac, pkt, 14 + olen);
}

int e2ee_established_count(void)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_PEERS; i++)
    {
        if (s_peers[i].used && s_peers[i].key != 0)
            n++;
    }
    xSemaphoreGive(s_lock);
    return n;
}

bool e2ee_get_peer_info(int idx, e2ee_peer_info_t *out)
{
    if (!out)
        return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0, n = 0; i < MAX_PEERS; i++)
    {
        session_t *s = &s_peers[i];
        if (!s->used || s->key == 0)
            continue;
        if (n++ != idx)
            continue;
        memset(out, 0, sizeof(*out));
        memcpy(out->mac, s->mac, 6);
        snprintf(out->name, sizeof(out->name), "%s", s->name);
        memcpy(out->fingerprint, s->fp, sizeof(out->fingerprint));
        out->tx_seq = s->tx_seq;
        out->rx_seq = s->rx_seq;
        ok = true;
        break;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void e2ee_get_self(uint8_t mac[6], char *name, size_t name_len)
{
    if (mac)
        memcpy(mac, s_our_mac, 6);
    if (name && name_len > 0)
        snprintf(name, name_len, "%s", s_our_name);
}

bool e2ee_lookup_name(const uint8_t mac[6], char *name, size_t name_len)
{
    if (!name || name_len == 0)
        return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    session_t *s = session_find(mac);
    if (s && s->key != 0)
    {
        snprintf(name, name_len, "%s", s->name);
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

// ---------- recepción ----------

static void handle_hello(const uint8_t mac[6], const uint8_t *body, int len)
{
    if (len < X25519_PUB_LEN + BOOT_NONCE_LEN + NAME_LEN)
        return;
    const uint8_t *peer_pub = body;
    const uint8_t *peer_nonce = body + X25519_PUB_LEN;
    const char *peer_name = (const char *)(body + X25519_PUB_LEN + BOOT_NONCE_LEN);

    if (memcmp(mac, s_our_mac, 6) == 0)
        return; // propio broadcast
    if (memcmp(peer_pub, s_our_pub, X25519_PUB_LEN) == 0)
        return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    session_t *s = session_find(mac);

    // Ya negociado con esta misma pub+nonce: ignorar.
    // (Evita ping-pong infinito de HELLOs y resets de contadores)
    if (s && s->key != 0 &&
        memcmp(s->peer_pub, peer_pub, X25519_PUB_LEN) == 0 &&
        memcmp(s->peer_nonce, peer_nonce, BOOT_NONCE_LEN) == 0)
    {
        xSemaphoreGive(s_lock);
        return;
    }

    bool nuevo = (s == NULL);
    if (nuevo)
        s = session_alloc();
    memcpy(s->mac, mac, 6);
    memcpy(s->name, peer_name, NAME_LEN);
    s->name[NAME_LEN] = '\0';

    uint8_t fp[8] = {0};
    esp_err_t err = session_establish(s, peer_pub, peer_nonce);
    if (err == ESP_OK)
    {
        memcpy(s->peer_pub, peer_pub, X25519_PUB_LEN);
        memcpy(s->peer_nonce, peer_nonce, BOOT_NONCE_LEN);
        compute_fingerprint(mac, peer_pub, fp);
        memcpy(s->fp, fp, sizeof(fp));
    }
    xSemaphoreGive(s_lock);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "No se pudo establecer sesión con " MACSTR, MAC2STR(mac));
        return;
    }

    add_espnow_peer(mac);
    ESP_LOGI(TAG, "Sesión segura %s con %s (" MACSTR ")",
             nuevo ? "establecida" : "renovada", s->name, MAC2STR(mac));
    log_fingerprint(mac, fp);

    // Responder para que el peer también derive la clave de inmediato
    e2ee_send_hello(mac);
}

static void handle_data(const uint8_t mac[6], const uint8_t *body, int len)
{
    // mínimo: nonce + seq + tag
    if (len < NONCE_LEN + SEQ_LEN + TAG_LEN)
        return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    session_t *s = session_find(mac);
    if (!s || s->key == 0)
    {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "DATA de peer desconocido " MACSTR ", re-negociando", MAC2STR(mac));
        e2ee_send_hello(mac);
        return;
    }

    uint8_t aad[8];
    memcpy(aad, mac, 6);
    aad[6] = E2E_VERSION;
    aad[7] = PKT_DATA;

    uint8_t ptext[SEQ_LEN + E2EE_MAX_PAYLOAD];
    size_t olen = 0;
    psa_status_t st = psa_aead_decrypt(s->key, PSA_ALG_GCM,
                                       body, NONCE_LEN,
                                       aad, sizeof(aad),
                                       body + NONCE_LEN, len - NONCE_LEN,
                                       ptext, sizeof(ptext), &olen);
    if (st != PSA_SUCCESS)
    {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "Descifrado/autenticación falló de " MACSTR " (%d), re-negociando",
                 MAC2STR(mac), (int)st);
        // El peer pudo reiniciarse (nueva clave efímera): auto-recuperación
        e2ee_send_hello(mac);
        return;
    }

    uint64_t seq = (olen >= SEQ_LEN) ? get_be64(ptext) : 0;
    if (olen < SEQ_LEN || seq <= s->rx_seq)
    {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "Replay/seq viejo de " MACSTR " (seq=%llu)", MAC2STR(mac),
                 (unsigned long long)seq);
        memset(ptext, 0, sizeof(ptext));
        return;
    }
    s->rx_seq = seq;
    e2ee_rx_cb_t cb = s_rx_cb;
    xSemaphoreGive(s_lock);

    if (cb)
        cb(mac, ptext + SEQ_LEN, olen - SEQ_LEN);
    memset(ptext, 0, sizeof(ptext));
}

void e2ee_handle_packet(const uint8_t mac[6], const uint8_t *data, int len)
{
    if (len < 2 || data[0] != E2E_VERSION)
        return;
    switch (data[1])
    {
    case PKT_HELLO:
        handle_hello(mac, data + 2, len - 2);
        break;
    case PKT_DATA:
        handle_data(mac, data + 2, len - 2);
        break;
    default:
        break;
    }
}
