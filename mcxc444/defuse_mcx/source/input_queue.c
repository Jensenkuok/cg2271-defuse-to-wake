/* Defuse to Wake - the single input event queue. See input_queue.h. */
#include "input_queue.h"

#include "app_config.h"

static QueueHandle_t s_queue;
static volatile uint32_t s_drops; /* written by ISRs and tasks, read by debug */

bool input_queue_init(void)
{
    s_queue = xQueueCreate(APP_INPUT_QUEUE_LEN, sizeof(game_event_t));
    return s_queue != NULL;
}

QueueHandle_t input_queue_handle(void)
{
    return s_queue;
}

void input_post_from_isr(game_event_type_t type, int32_t value, BaseType_t *woken)
{
    game_event_t ev;

    ev.type = type;
    ev.value = value;
    if (xQueueSendFromISR(s_queue, &ev, woken) != pdTRUE) {
        s_drops++; /* ISRs on this M0+ cannot be pre-empted by tasks: safe */
    }
}

void input_post(game_event_type_t type, int32_t value)
{
    game_event_t ev;

    ev.type = type;
    ev.value = value;
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        taskENTER_CRITICAL(); /* an ISR may increment it at the same time */
        s_drops++;
        taskEXIT_CRITICAL();
    }
}

uint32_t input_queue_drops(void)
{
    return s_drops;
}
