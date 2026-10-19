/* Defuse to Wake - settings in flash. See settings.h. */
#include "settings.h"

#include <Arduino.h>
#include <string.h>

#include "../../secrets.h"
#include "config.h"
#include "flash_store.h"

static volatile uint32_t s_version;   /* 32-bit reads/writes are atomic; only bumped under s_change */
static portMUX_TYPE s_change = portMUX_INITIALIZER_UNLOCKED;

static void bump_version(void)
{
    taskENTER_CRITICAL(&s_change);
    s_version = s_version + 1u;
    taskEXIT_CRITICAL(&s_change);
}

static int clamp_lead(int minutes)
{
    if (minutes < APP_LEAD_MIN_LOW) {
        return APP_LEAD_MIN_LOW;
    }
    return (minutes > APP_LEAD_MIN_HIGH) ? APP_LEAD_MIN_HIGH : minutes;
}

void settings_get_link(char *out, size_t size)
{
    bool present = false;

    if (out == NULL || size == 0u) {
        return;
    }
    if (!flash_read_str(APP_KEY_LINK, out, size, &present) || !present) {
        snprintf(out, size, "%s", NUSMODS_SHARE_LINK);   /* first-boot default */
    }
}

bool settings_set_link(const char *url)
{
    char current[APP_LINK_MAX_CHARS + 1];            /* on the caller's stack: callers may be different tasks */
    size_t len = (url != NULL) ? strlen(url) : 0u;

    if (len == 0u || len > APP_LINK_MAX_CHARS) {
        return false;
    }
    settings_get_link(current, sizeof(current));
    if (strcmp(current, url) == 0) {
        return true;                                  /* unchanged: no flash wear, no refresh */
    }
    if (!flash_write_str(APP_KEY_LINK, url)) {
        return false;
    }
    bump_version();
    return true;
}

int settings_get_lead_min(void)
{
    return clamp_lead((int)flash_read_int(APP_KEY_LEAD, APP_LEAD_MIN_DEFAULT));
}

int settings_set_lead_min(int minutes)
{
    int value = clamp_lead(minutes);

    if (value == settings_get_lead_min()) {
        return value;
    }
    if (!flash_write_int(APP_KEY_LEAD, value)) {
        return settings_get_lead_min();
    }
    bump_version();
    return value;
}

uint32_t settings_version(void)
{
    return s_version;
}
