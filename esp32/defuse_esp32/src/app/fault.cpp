/* Defuse to Wake - fault injection state. See fault.h. */
#include "fault.h"

#include <Arduino.h>

#include "config.h"
#include "web_logic.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_corrupt_armed;     /* guarded by s_mux */
static Outage s_outage;          /* guarded by s_mux; the expiry rule is the host-tested outage_* in web_logic */

void fault_init(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_corrupt_armed = false;
    s_outage.active = false;
    s_outage.until_ms = 0u;
    taskEXIT_CRITICAL(&s_mux);
}

void fault_corrupt_arm(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_corrupt_armed = true;
    taskEXIT_CRITICAL(&s_mux);
    Serial.println("[fault] corrupt-next-packet armed");
}

bool fault_corrupt_armed(void)
{
    bool armed;

    taskENTER_CRITICAL(&s_mux);
    armed = s_corrupt_armed;
    taskEXIT_CRITICAL(&s_mux);
    return armed;
}

bool fault_corrupt_take(void)
{
    bool was_armed;

    taskENTER_CRITICAL(&s_mux);
    was_armed = s_corrupt_armed;
    s_corrupt_armed = false;
    taskEXIT_CRITICAL(&s_mux);
    return was_armed;
}

void fault_outage_start(void)
{
    const uint32_t now = millis();

    taskENTER_CRITICAL(&s_mux);
    outage_start(&s_outage, now, APP_FAULT_OUTAGE_S * 1000u);
    taskEXIT_CRITICAL(&s_mux);
    Serial.printf("[fault] simulated internet outage for %u s (Wi-Fi stays up)\n", (unsigned)APP_FAULT_OUTAGE_S);
}

bool fault_outage_active(void)
{
    const uint32_t now = millis();
    bool active;

    taskENTER_CRITICAL(&s_mux);
    active = outage_active(s_outage, now);
    taskEXIT_CRITICAL(&s_mux);
    return active;
}

uint32_t fault_outage_left_s(void)
{
    const uint32_t now = millis();
    uint32_t left;

    taskENTER_CRITICAL(&s_mux);
    left = outage_left_s(s_outage, now);
    taskEXIT_CRITICAL(&s_mux);
    return left;
}

bool fault_net_blocked(void)
{
    return fault_outage_active();
}
