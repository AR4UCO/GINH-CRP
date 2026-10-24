#include "driver/gpio.h"
#include "nvs.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "board.h"
#include "buttons.h"

static const char *TAG = "buttons";

// Generalmente uno tiene un filtro pasabanda bajo para
// filtrar la oscilacion de un boton, como no tenemos eso
// por software esperamos un ratito. Esto se puede mejorar.
#define DEBOUNCE_MS     20
#define EVENT_QUEUE_LEN 16

// Esto esta mal, ver los comentarios en board.h
// btw chequeen: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gpio.html
static struct {
    gpio_num_t gpio;
    const char *name;
} s_buttons[BTN_COUNT] = {
    [BTN_UP] = {BOARD_KEY_UP_IO, "UP"},
    [BTN_DOWN] = {BOARD_KEY_DOWN_IO, "DOWN"},
    [BTN_LEFT] = {BOARD_KEY_LEFT_IO, "LEFT"},
    [BTN_RIGHT] = {BOARD_KEY_RIGHT_IO, "RIGHT"},
    [BTN_CENTER] = {BOARD_KEY_CENTER_IO, "CENTER"},
    [BTN_KEY1] = {BOARD_KEY_1_IO, "KEY1"},
    [BTN_KEY2] = {BOARD_KEY_2_IO, "KEY2"},
    [BTN_KEY3] = {BOARD_KEY_3_IO, "KEY3"},
    [BTN_KEY4] = {BOARD_KEY_4_IO, "KEY4"},
};

static QueueHandle_t s_isr_queue;
static QueueHandle_t s_event_queue;

static volatile uint32_t s_state;

/* Cada pulsacion queda latcheada este tiempo aunque se suelte antes. La UI
   muestrea cada 50 ms y el teclado cada 20: con el estado 'mantenido' puro, un
   toque rapido (30-40 ms) cae entre dos lecturas y la placa parece no responder.
   Con el latch el flanco de subida SIEMPRE se ve, y el tiempo de 'mantenido'
   sigue midiendo bien para la pulsacion larga (umbral 700 ms >> 120 ms). */
#define LATCH_MS 120
static volatile TickType_t s_latch_hasta[BTN_COUNT];

/* ---------------- giro del joystick (declarado temprano: lo usa buttons_task) --
   El joystick de la board viene fisicamente girado y al grupo le pasaron dos
   mapas que difieren en un cuarto de vuelta (graficos/teclado: UP=9; crypto:
   UP=10). En vez de pelear el pinout a manotazos, el giro se aplica sobre el
   IDENTIFICADOR (los GPIO quedan registrados como nacen) y se puede cambiar en
   vivo con 'joystick <0-3>' del REPL; se guarda en NVS porque cada placa es un
   mundo. Se comprueba apretando y mirando el log del comando 'botones'. */
#ifndef CONFIG_HACK_JOYSTICK_PASOS
#define CONFIG_HACK_JOYSTICK_PASOS 0
#endif

#define NVS_NS_BOTONES "kiwi_botones"
#define NVS_KEY_GIRO   "giro"

/* Los cuatro del joystick, en el orden horario en que los nombra board.h. */
static const button_id_t GIRO_ORDEN[4] = { BTN_UP, BTN_RIGHT, BTN_DOWN, BTN_LEFT };

static volatile int s_giro = CONFIG_HACK_JOYSTICK_PASOS;

/* del GPIO que disparo (indice natural) al boton logico que ve la UI */
static button_id_t giro_a_logico(button_id_t fisico, int pasos)
{
    for (int d = 0; d < 4; d++) {
        if (GIRO_ORDEN[d] == fisico) {
            return GIRO_ORDEN[(d - (pasos & 3) + 4) & 3];
        }
    }
    return fisico;
}

/* inverso: para reportar que GPIO hay detras de cada boton logico */
static button_id_t giro_a_fisico(button_id_t logico, int pasos)
{
    for (int d = 0; d < 4; d++) {
        if (GIRO_ORDEN[d] == logico) {
            return GIRO_ORDEN[(d + (pasos & 3)) & 3];
        }
    }
    return logico;
}

static void cargar_giro_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BOTONES, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t v = CONFIG_HACK_JOYSTICK_PASOS;
    const esp_err_t err = nvs_get_i32(h, NVS_KEY_GIRO, &v);
    nvs_close(h);
    if (err == ESP_OK && v >= 0 && v <= 3) {
        s_giro = (int)v;
        ESP_LOGI(TAG, "giro del joystick cargado de NVS: %d pasos", s_giro);
    }
}

static inline bool level_is_pressed(button_id_t id)
{
    return gpio_get_level(s_buttons[id].gpio) == 0;
}

/*
    Creo que la mayoria ya tuvo Orga II pero un isr es un interrupt
    service routine aka el cachito de codigo que llamas cuando te cae
    una interrupcion, por cuestiones obvias queremos lidear con la int
    lo mas rapido posible y hay funciones especificas para encolar cuando
    estas dentro de una isr.
    https://www.freertos.org/Documentation/02-Kernel/04-API-references/06-Queues/04-xQueueSendFromISR
*/
static void IRAM_ATTR button_isr(void *arg)
{
    button_id_t id = (button_id_t)(uintptr_t)arg;
    BaseType_t woken = pdFALSE;

    gpio_intr_disable(s_buttons[id].gpio);
    xQueueSendFromISR(s_isr_queue, &id, &woken);

    // Woken dice: Escribir a la queue desperto una task de mayor prioridad
    //              a la task interrumpida
    // Esta nada mas para evitar un priority inversion.
    // Si vieron SO lo entienden de una
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

/*
    No esta del todo mal pensar en las tasks como "procesos light"
    Piensen que son unidades de trabajo concurrentes con la particularidad
    de que ningun proceso tiene memoria propia...
*/
static void buttons_task(void *arg)
{
    button_id_t id;

    while (1) {
        xQueueReceive(s_isr_queue, &id, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

        gpio_intr_enable(s_buttons[id].gpio);

        const button_id_t logico = giro_a_logico(id, s_giro);
        const uint32_t bit = 1u << logico;
        const bool pressed = level_is_pressed(id); /* el GPIO que disparo, sin girar */
        if (pressed == !!(s_state & bit)) {
            continue;
        }
        s_state = pressed ? (s_state | bit) : (s_state & ~bit);
        if (pressed) {
            s_latch_hasta[logico] = xTaskGetTickCount() + pdMS_TO_TICKS(LATCH_MS);
        }

        const button_event_t ev = {.id = logico, .pressed = pressed};
        xQueueSend(s_event_queue, &ev, 0);
    }
}

/*
    OKAY LETS GO!!!!!
    Tenemos una cola de la interrupcion de donde lee la task
    Una cola de eventos de donde lee el resto que quiere leer botones
    Nuextra task de botones (vean la docu para entender las flags son medio un embole)
    https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos_idf.html#_CPPv411xTaskCreate14TaskFunction_tPCKcK22configSTACK_DEPTH_TYPEPCv11UBaseType_tPC12TaskHandle_t
*/
/* 'joystick <0-3>' del REPL: cambia el giro en vivo y lo persiste. */
void buttons_set_giro(int pasos)
{
    pasos &= 3;
    s_giro = pasos;

    /* Tiramos el estado de las cuatro direcciones para que un boton que estaba
       levantando a mitad del cambio no quede 'apretado' para siempre. */
    uint32_t bits_joystick = 0;
    for (int d = 0; d < 4; d++) {
        bits_joystick |= 1u << GIRO_ORDEN[d];
        s_latch_hasta[GIRO_ORDEN[d]] = 0;
    }
    s_state &= ~bits_joystick;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_BOTONES, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_i32(h, NVS_KEY_GIRO, pasos) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }

    ESP_LOGI(TAG, "giro del joystick: %d pasos -> %s(%d) %s(%d) %s(%d) %s(%d)",
             pasos, buttons_name(BTN_UP), buttons_io(BTN_UP), buttons_name(BTN_RIGHT),
             buttons_io(BTN_RIGHT), buttons_name(BTN_DOWN), buttons_io(BTN_DOWN),
             buttons_name(BTN_LEFT), buttons_io(BTN_LEFT));
}

int buttons_giro(void)
{
    return s_giro;
}

esp_err_t buttons_init(void)
{
    cargar_giro_nvs();

    s_isr_queue = xQueueCreate(BTN_COUNT, sizeof(button_id_t));
    s_event_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(button_event_t));
    ESP_RETURN_ON_FALSE(s_isr_queue && s_event_queue, ESP_ERR_NO_MEM, TAG, "no mem for queues");

    ESP_RETURN_ON_FALSE(xTaskCreate(buttons_task, "buttons", 3072, NULL, 10, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task create failed");

    esp_err_t err = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "isr service install failed");

    for (int i = 0; i < BTN_COUNT; i++) {
        const gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_buttons[i].gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "config of %s failed", s_buttons[i].name);
        // Por cada boton agregamos el isr de los botones como el handler.
        ESP_RETURN_ON_ERROR(gpio_isr_handler_add(s_buttons[i].gpio, button_isr, (void *)(uintptr_t)i), TAG,
                            "isr add of %s failed", s_buttons[i].name);
    }

    for (int i = 0; i < BTN_COUNT; i++) {
        if (level_is_pressed(i)) {
            s_state |= 1u << giro_a_logico((button_id_t)i, s_giro);
        }
    }
    ESP_LOGI(TAG, "joystick: %s(%d) %s(%d) %s(%d) %s(%d) (giro %d pasos)",
             buttons_name(BTN_UP), buttons_io(BTN_UP), buttons_name(BTN_RIGHT),
             buttons_io(BTN_RIGHT), buttons_name(BTN_DOWN), buttons_io(BTN_DOWN),
             buttons_name(BTN_LEFT), buttons_io(BTN_LEFT), s_giro);
    return ESP_OK;
}

QueueHandle_t buttons_event_queue(void)
{
    return s_event_queue;
}

uint32_t buttons_read(void)
{
    uint32_t v = s_state;
    const TickType_t ahora = xTaskGetTickCount();
    for (int i = 0; i < BTN_COUNT; i++) {
        const TickType_t hasta = s_latch_hasta[i];
        if (hasta != 0 && (int32_t)(ahora - hasta) < 0) {
            v |= 1u << i;
        }
    }
    return v;
}

/* Simula una pulsacion que no sale de un GPIO (comando 'tecla' del REPL).
   Reusa el latch, que es justo lo que lee la UI, asi que ninguna pantalla tiene
   que tener un codigo especial para poder probarse desde la PC. */
bool buttons_pulsar(button_id_t id)
{
    if (id >= BTN_COUNT) {
        return false;
    }
    s_latch_hasta[id] = xTaskGetTickCount() + pdMS_TO_TICKS(LATCH_MS);
    return true;
}

/* Busca un boton por el nombre que usa el propio log ('KEY1', 'UP', 'CENTER'...). */
button_id_t buttons_buscar(const char *nombre)
{
    if (!nombre) {
        return BTN_COUNT;
    }
    for (int i = 0; i < BTN_COUNT; i++) {
        if (strcasecmp(nombre, buttons_name((button_id_t)i)) == 0) {
            return (button_id_t)i;
        }
    }
    return BTN_COUNT;
}

const char *buttons_name(button_id_t id)
{
    return id < BTN_COUNT ? s_buttons[id].name : "?";
}

int buttons_io(button_id_t id)
{
    if (id >= BTN_COUNT) {
        return -1;
    }
    return (int)s_buttons[giro_a_fisico(id, s_giro)].gpio;
}
