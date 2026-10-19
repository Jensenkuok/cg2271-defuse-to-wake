/* Defuse to Wake - flash access. See flash_store.h. */
#include "flash_store.h"

#include <Preferences.h>

#include "config.h"

static SemaphoreHandle_t s_mutex;

bool flash_store_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex != NULL;
}

/* Run fn(prefs) with the lock held and the namespace open read-write (a read-only open of a
 * namespace that does not exist yet would log an NVS error on the first boot). */
template <typename Fn>
static bool with_prefs(Fn fn)
{
    Preferences prefs;
    bool ok;

    if (s_mutex == NULL || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(APP_FLASH_MUTEX_WAIT_MS)) != pdTRUE) {
        return false;
    }
    ok = prefs.begin(APP_PREFS_NAMESPACE, false);
    if (ok) {
        ok = fn(prefs);
        prefs.end();
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

bool flash_read_blob(const char *key, void *buf, size_t len)
{
    return with_prefs([&](Preferences &p) {
        return p.getBytesLength(key) == len && p.getBytes(key, buf, len) == len;
    });
}

bool flash_write_blob(const char *key, const void *buf, size_t len)
{
    return with_prefs([&](Preferences &p) { return p.putBytes(key, buf, len) == len; });
}

bool flash_read_str(const char *key, char *buf, size_t size, bool *present)
{
    bool found = false;
    bool ok;

    if (buf == NULL || size == 0u) {
        return false;
    }
    buf[0] = '\0';
    ok = with_prefs([&](Preferences &p) {
        found = p.isKey(key);
        if (found) {
            (void)p.getString(key, buf, size);
        }
        return true;
    });
    buf[size - 1u] = '\0';
    if (present != NULL) {
        *present = ok && found;
    }
    return ok;
}

bool flash_write_str(const char *key, const char *value)
{
    return with_prefs([&](Preferences &p) { return p.putString(key, value) == strlen(value); });
}

int32_t flash_read_int(const char *key, int32_t fallback)
{
    int32_t v = fallback;

    (void)with_prefs([&](Preferences &p) {
        v = p.getInt(key, fallback);
        return true;
    });
    return v;
}

bool flash_write_int(const char *key, int32_t value)
{
    return with_prefs([&](Preferences &p) { return p.putInt(key, value) == sizeof(int32_t); });
}
