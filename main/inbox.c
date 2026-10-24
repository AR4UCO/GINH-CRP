#include <stdio.h>
#include <string.h>
#include "inbox.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static bool s_pending;
static char s_sender[INBOX_SENDER_MAX];
static char s_text[INBOX_TEXT_MAX];

/* La fuente del display es ASCII 5x7: reemplazar todo lo demás. */
static char clean(char c)
{
    return (c >= 0x20 && c <= 0x7E) ? c : '?';
}

bool inbox_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    return s_lock != NULL;
}

bool inbox_post(const char *sender, const char *text, size_t len)
{
    if (!s_lock || !text)
        return false;
    if (len > INBOX_TEXT_MAX - 1)
        len = INBOX_TEXT_MAX - 1;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    size_t i = 0;
    if (sender)
    {
        for (; i < INBOX_SENDER_MAX - 1 && sender[i]; i++)
            s_sender[i] = clean(sender[i]);
    }
    s_sender[i] = '\0';

    for (i = 0; i < len; i++)
        s_text[i] = clean(text[i]);
    s_text[len] = '\0';

    s_pending = true;
    xSemaphoreGive(s_lock);
    return true;
}

bool inbox_pending(void)
{
    if (!s_lock)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool pending = s_pending;
    xSemaphoreGive(s_lock);
    return pending;
}

bool inbox_take(char *sender, size_t sender_len, char *text, size_t text_len)
{
    if (!s_lock || !sender || !text || sender_len == 0 || text_len == 0)
        return false;

    bool got = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pending)
    {
        snprintf(sender, sender_len, "%s", s_sender);
        snprintf(text, text_len, "%s", s_text);
        s_pending = false;
        got = true;
    }
    xSemaphoreGive(s_lock);
    return got;
}
