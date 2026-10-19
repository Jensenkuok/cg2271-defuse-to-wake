/*
 * Defuse to Wake - the only door to flash (Preferences / NVS, namespace APP_PREFS_NAMESPACE).
 * Every settings, timetable, history and mirror-queue access goes through here so one mutex
 * serialises them (Preferences is not thread-safe; the network task writes while the future
 * web-page task reads settings). Waits are bounded: a stuck holder makes the call fail, it
 * never blocks a task forever. Never call from the link/gyro/display tasks: a flash write can
 * take tens of milliseconds.
 */
#ifndef FLASH_STORE_H
#define FLASH_STORE_H

#include <Arduino.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool flash_store_init(void);

/* Blob of exactly len bytes. read returns true only if a blob of that exact size exists. */
bool flash_read_blob(const char *key, void *buf, size_t len);
bool flash_write_blob(const char *key, const void *buf, size_t len);

/* String into buf (always NUL-terminated). *present tells whether the key existed. */
bool flash_read_str(const char *key, char *buf, size_t size, bool *present);
bool flash_write_str(const char *key, const char *value);

int32_t flash_read_int(const char *key, int32_t fallback);
bool flash_write_int(const char *key, int32_t value);

#endif /* FLASH_STORE_H */
