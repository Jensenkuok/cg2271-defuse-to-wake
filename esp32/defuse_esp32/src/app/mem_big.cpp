/* Defuse to Wake - large-buffer allocation and memory report. See mem_big.h. */
#include "mem_big.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

static const char k_no_psram_warning[] = "[boot] WARNING: PSRAM not enabled (Tools > PSRAM > Enabled)";

bool mem_psram_present(void)
{
    return ESP.getPsramSize() > 0u;
}

size_t mem_internal_free(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

size_t mem_internal_largest(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

void *mem_big_alloc(size_t bytes, const char *what)
{
    const uint32_t caps = (mem_psram_present() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT;
    void *p = heap_caps_calloc(1u, bytes, caps);

    if (p == NULL) {
        Serial.printf("[boot] FATAL: cannot allocate %u bytes for %s (%s, int_free=%u int_largest=%u)\n",
                      (unsigned)bytes, what, mem_psram_present() ? "PSRAM" : "internal", (unsigned)mem_internal_free(),
                      (unsigned)mem_internal_largest());
    }
    return p;
}

void mem_boot_report(const char *when)
{
    Serial.printf("[boot] %s: PSRAM size=%u free=%u  internal free=%u largest=%u\n", when,
                  (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram(), (unsigned)mem_internal_free(),
                  (unsigned)mem_internal_largest());
    if (!mem_psram_present()) {
        Serial.println(k_no_psram_warning);
    }
}

void mem_status_print(void)
{
    Serial.printf("[mem] int_free=%u int_largest=%u int_min=%u psram_free=%u\n", (unsigned)mem_internal_free(),
                  (unsigned)mem_internal_largest(), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)ESP.getFreePsram());
    if (!mem_psram_present()) {
        Serial.println(k_no_psram_warning);
    }
}
