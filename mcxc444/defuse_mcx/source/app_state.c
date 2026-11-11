/* Defuse to Wake - shared snapshot and control queue. See app_state.h. */
#include "app_state.h"

#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "app_config.h"

static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_ctrl_queue;
static app_state_t s_state;
static volatile uint32_t s_lock_timeouts;

bool app_state_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_mutex = xSemaphoreCreateMutex();
    /* Length 4: these messages are rare (a few at start-up and nightly) and the
     * link already serialises them one at a time (stop-and-wait). */
    s_ctrl_queue = xQueueCreate(APP_CONTROL_QUEUE_LEN, sizeof(app_ctrl_t));
    return s_mutex != NULL && s_ctrl_queue != NULL;
}

/*
 * Why a MUTEX and not a critical section:
 *  - The snapshot is a multi-word struct copied by three tasks (game prio 4 writes;
 *    comm prio 2 and debug prio 1 read). A critical section would mask ALL interrupts
 *    for the copy, including the UART2 RX ISR (priority 64) that must never be delayed.
 *    The mutex only serialises the three tasks.
 *  - FreeRTOS mutexes have PRIORITY INHERITANCE: if the prio-1 debug task holds the
 *    lock and is pre-empted by prio-2 comm, the prio-4 game task waiting for the lock
 *    lends its priority to debug until it releases, so game is not stuck behind comm
 *    (classic priority inversion). A binary semaphore would not do that.
 * The lock is held only for the memcpy, and every take has a bounded timeout.
 */
void app_state_write(const app_state_t *s)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(APP_STATE_MUTEX_WAIT_MS)) == pdTRUE) {
        memcpy(&s_state, s, sizeof(s_state));
        (void)xSemaphoreGive(s_mutex);
    } else {
        taskENTER_CRITICAL();
        s_lock_timeouts++;
        taskEXIT_CRITICAL();
    }
}

bool app_state_read(app_state_t *out)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(APP_STATE_MUTEX_WAIT_MS)) != pdTRUE) {
        taskENTER_CRITICAL();
        s_lock_timeouts++;
        taskEXIT_CRITICAL();
        return false;
    }
    memcpy(out, &s_state, sizeof(*out));
    (void)xSemaphoreGive(s_mutex);
    return true;
}

bool app_ctrl_post(const app_ctrl_t *c)
{
    return xQueueSend(s_ctrl_queue, c, 0) == pdTRUE; /* 0 timeout: never block the comm task */
}

bool app_ctrl_receive(app_ctrl_t *c)
{
    return xQueueReceive(s_ctrl_queue, c, 0) == pdTRUE;
}

uint32_t app_state_lock_timeouts(void)
{
    return s_lock_timeouts;
}
