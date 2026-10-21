/*
 * Defuse to Wake - where the large data buffers live, and the memory numbers we print.
 *
 * Every TLS connection takes ~32 KB+ of INTERNAL RAM (the core builds mbedTLS with
 * CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC and 16 KB records), so the big working buffers (lesson
 * lists, LLM reply text, web JSON, puzzle sets) are allocated ONCE at init from PSRAM and
 * only fall back to internal RAM when the board has no PSRAM. Rules:
 *   - data buffers only: task stacks stay internal (a stack in PSRAM crashes while the flash
 *     cache is off, e.g. during an NVS write);
 *   - buffers that are handed to flash_store / Preferences stay internal static arrays, so a
 *     flash write never reads from PSRAM;
 *   - allocation happens at init only (CLAUDE.md: no dynamic allocation after start-up).
 */
#ifndef MEM_BIG_H
#define MEM_BIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Zeroed block of `bytes`: PSRAM when present, else internal. NULL (after a "[boot] FATAL" line
 * naming `what`) when the allocation fails. Never freed. */
void *mem_big_alloc(size_t bytes, const char *what);

template <typename T>
static inline T *mem_big_new(size_t count, const char *what)
{
    return static_cast<T *>(mem_big_alloc(sizeof(T) * count, what));
}

bool mem_psram_present(void);
size_t mem_internal_free(void);
size_t mem_internal_largest(void);

/* One "[boot] ..." block: PSRAM size/free, internal free and largest block, and the warning
 * when there is no PSRAM. `when` names the moment ("start", "after init"). */
void mem_boot_report(const char *when);

/* One "[mem] ..." status line (plus the PSRAM warning when it is missing). */
void mem_status_print(void);

#endif /* MEM_BIG_H */
