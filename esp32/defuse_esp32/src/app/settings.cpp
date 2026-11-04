/* Defuse to Wake - settings in flash. See settings.h. */
#include "settings.h"

#include <Arduino.h>
#include <string.h>

#include "../../secrets.h"
#include "config.h"
#include "difficulty.h"
#include "flash_store.h"

static volatile uint32_t s_version;   /* 32-bit reads/writes are atomic; only bumped under s_change */
static volatile uint32_t s_diff_version;
static volatile uint8_t s_diff_mode = DIFF_MODE_DEFAULT;   /* RAM copy of the flash value; 8-bit access is atomic */
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

void settings_load_difficulty(void)
{
    const int stored = (int)flash_read_int(APP_KEY_DIFFICULTY, DIFF_MODE_DEFAULT);

    s_diff_mode = (uint8_t)(difficulty_mode_valid(stored) ? stored : (int)DIFF_MODE_DEFAULT);   /* a broken value = Auto */
}

int settings_get_difficulty_mode(void)
{
    return s_diff_mode;
}

int settings_set_difficulty_mode(int mode)
{
    if (!difficulty_mode_valid(mode)) {
        return s_diff_mode;
    }
    if (mode == (int)s_diff_mode) {
        return mode;                                  /* unchanged: no flash wear, no refill */
    }
    if (!flash_write_int(APP_KEY_DIFFICULTY, mode)) {
        return s_diff_mode;
    }
    s_diff_mode = (uint8_t)mode;
    taskENTER_CRITICAL(&s_change);
    s_diff_version = s_diff_version + 1u;
    taskEXIT_CRITICAL(&s_change);
    return mode;
}

uint32_t settings_difficulty_version(void)
{
    return s_diff_version;
}
