/*
 * Defuse to Wake - ESP32-S2 configuration. Every pin, priority, stack size, period and
 * threshold lives here. The default bomb values come from the synced copy of
 * game/game_config.h (never retyped).
 *
 * The ESP32-S2 has ONE core. All tasks are created with xTaskCreate (no core pinning),
 * so the priorities below are the whole scheduling story.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>

#include "../game/game_config.h"   /* GAME_DEFAULT_*: default bomb, shared with the MCX */
#include "../shared/protocol.h"

/* ---- Link to the MCXC444: Serial1, 115200 8N1 (proven in Lab 3) ---- */
#define APP_UART_BAUD               115200u
#define APP_UART_RX_PIN             2       /* GPIO2  <- MCX PTE22 (UART2 TX) */
#define APP_UART_TX_PIN             1       /* GPIO1  -> MCX PTE23 (UART2 RX) */
#define APP_DEBUG_BAUD              115200u /* Serial (debug) */

/* ---- I2C bus shared by the OLED and the MPU-6050 ---- */
#define APP_I2C_SDA_PIN             8
#define APP_I2C_SCL_PIN             9
#define APP_I2C_HZ                  400000u
#define APP_OLED_ADDR               0x3C
#define APP_OLED_WIDTH              128
#define APP_OLED_HEIGHT             64
#define APP_MPU_ADDR                0x68
#define APP_I2C_MUTEX_WAIT_MS       30u     /* bounded wait for the I2C bus mutex (a frame takes ~25 ms) */
#define APP_STATE_MUTEX_WAIT_MS     10u     /* bounded wait for the app-state mutex (held for a memcpy) */

/* ---- Demo trigger: the board's BOOT button ---- */
#define APP_BOOT_BUTTON_PIN         0       /* GPIO0, active low */
#define APP_BUTTON_POLL_MS          50u     /* polled inside the link task loop */
#define APP_BOOT_RING_DELAY_S       3u      /* CMD RING_NOW arg */
#define APP_BOMB_CFG_POLL_MS        50u     /* link task: BOMB_CONFIG bookkeeping + triggers; the MCX starts the
                                             * next bomb 3 s after EXPLODED, so this is far inside that window */

/* ---- Task priorities (FreeRTOS: larger = more urgent). One core, no pinning. ---- */
#define APP_PRIO_LINK               4u  /* serial RX must never overflow, ACK/retransmit timing (100 ms) */
#define APP_PRIO_GYRO               3u  /* fixed 50 Hz sampling must not jitter; tamper latency matters */
#define APP_PRIO_DISPLAY            2u  /* 10 fps drawing; a late frame is harmless */
#define APP_PRIO_NETWORK            1u  /* slow, blocks in Wi-Fi calls, latency tolerant: lowest */

/* ---- Task stacks. ESP32 Arduino xTaskCreate takes BYTES. ---- */
#define APP_STACK_LINK              6144u   /* proto_link + BOMB_CONFIG build (2 puzzles, difficulty, history copy) + Serial.printf */
#define APP_STACK_GYRO              4096u
#define APP_STACK_DISPLAY           6144u   /* Adafruit GFX + snprintf */
#define APP_STACK_NETWORK           12288u  /* Wi-Fi, SNTP, TLS handshake + ArduinoJson parsing */

/* ---- Queues ---- */
#define APP_TX_QUEUE_LEN            8u      /* other tasks -> link task */
#define APP_RESULT_QUEUE_LEN        8u      /* link task -> network task: RESULT frames to record */

/* ---- Link task ---- */
#define APP_LINK_LOOP_MS            5u      /* RX drain + proto_link_poll period */
#define APP_LINK_STATS_MS           1000u   /* link task publishes its counters this often */

/* ---- Display ---- */
#define APP_DISPLAY_PERIOD_MS       100u    /* 10 fps */
#define APP_HEADLESS_PRINT_MS       1000u   /* one-line summary period without an OLED */
#define APP_BOOM_FLASH_MS           250u    /* inverted-screen flash half-period */
#define APP_CLUE_PLACEHOLDER        "Clue: (AI clue pending)"
#define APP_CLUE_MAX_CHARS          21      /* 128 px / 6 px per char */

/* ---- Gyro / tamper ---- */
#define APP_GYRO_PERIOD_MS          20u     /* 50 Hz */
#define APP_TAMPER_SEND_MG          600u    /* deviation from 1 g that is sent to the MCX */
#define APP_TAMPER_COOLDOWN_MS      500u    /* local cooldown between TAMPER_EVENTs */
#define APP_GYRO_LOG_MS             5000u   /* peak deviation log period */
#define APP_STANDARD_GRAVITY        9.80665f
#define APP_MG_PER_G                1000.0f
#define APP_TAMPER_MAX_MG           65535u  /* TAMPER_EVENT field is u16 */

/* ---- Network ---- */
#define APP_WIFI_POLL_CONNECTED_MS  2000u   /* WiFiMulti.run() cadence when connected */
#define APP_WIFI_POLL_DOWN_MS       5000u   /* ... and when not connected (each try blocks) */
#define APP_WIFI_CONNECT_TIMEOUT_MS 5000u
#define APP_NET_LOOP_MS             250u
#define APP_NTP_SERVER_1            "pool.ntp.org"
#define APP_NTP_SERVER_2            "time.google.com"
#define APP_TZ_OFFSET_S             (8 * 3600)  /* Asia/Singapore, UTC+8, no DST */
#define APP_TIME_SYNC_PERIOD_MS     600000u /* re-send TIME_SYNC every 10 min */
#define APP_MIN_VALID_UNIX          1700000000u /* anything earlier means SNTP has not set the clock */

/* ---- Settings and flash (Preferences namespace shared by all stored data) ---- */
#define APP_PREFS_NAMESPACE         "defuse"
#define APP_FLASH_MUTEX_WAIT_MS     2000u   /* a flash write can take tens of ms; bounded, never forever */
#define APP_KEY_LINK                "nusmods_link"
#define APP_KEY_LEAD                "lead_min"
#define APP_KEY_TIMETABLE           "tt_cache"
#define APP_KEY_HISTORY             "hist"
#define APP_KEY_MIRROR              "mirror"
#define APP_LINK_MAX_CHARS          512
#define APP_LEAD_MIN_DEFAULT        60      /* minutes of wake-up lead before the first lecture */
#define APP_LEAD_MIN_LOW            15
#define APP_LEAD_MIN_HIGH           180

/* ---- Timetable (NUSMods) ---- */
#define APP_NUSMODS_URL_PREFIX      "https://api.nusmods.com/v2/2026-2027/modules/"
#define APP_NUSMODS_SEMESTER        1
#define APP_HTTP_TIMEOUT_MS         10000u  /* every network call: connect and read, at most 10 s */
#define APP_HTTP_TIMEOUT_S          10u     /* same limit for the TLS client (takes seconds) */
#define APP_TT_NIGHTLY_HOUR         22      /* local hour of the nightly timetable refresh */
#define APP_TT_RETRY_MS             300000u /* retry a failed refresh after 5 min */
#define APP_TT_MODULE_URL_MAX       112
#define APP_TT_CACHE_MAGIC          0x54544331u /* "TTC1": bump when Lesson changes */

/* ---- Morning history + Firebase mirror ---- */
#define APP_HIST_MAGIC              0x48495331u /* "HIS1" */
#define APP_MIRROR_MAGIC            0x4D495231u /* "MIR1" */
#define APP_MIRROR_PERIOD_MS        60000u  /* first retry delay; also the normal upload cadence */
#define APP_MIRROR_BACKOFF_MAX_MS   600000u /* failed uploads back off 60 s -> 10 min */
#define APP_FIREBASE_PATH           "mornings.json?auth="
#define APP_FIREBASE_URL_MAX        320
#define APP_FIREBASE_BODY_MAX       192
#define APP_RING_RECENT_S           (3 * 3600)  /* a fired alarm still names the lesson for 3 h */

/* ---- SoCLaaS LLM + puzzle pipeline (network task, lowest priority, never at alarm time) ----
 * The API key stays in secrets.h (SOCLAAS_API_KEY); everything else is here. SoCLaaS swaps the
 * models behind these IDs without notice, so the chains are the only place to change them. The
 * checker chain starts with the model that did NOT write the puzzle (see checker_order()). */
#define APP_SOCLAAS_BASE_URL        "https://soclaas-api.comp.nus.edu.sg/v1"
#define APP_SOCLAAS_CHAT_PATH       "/chat/completions"
#define APP_LLM_CHAIN_LEN           4
#define APP_LLM_SETTER_CHAIN        { "x-test-2", "x-test-1", "qwen3.6:35b", "default" }
#define APP_LLM_CHECKER_CHAIN       { "x-test-1", "x-test-2", "qwen3.6:35b", "default" }
#define APP_LLM_TIMEOUT_MS          30000u  /* one request: connect and read, at most 30 s */
#define APP_LLM_TIMEOUT_S           30u     /* same limit for the TLS client */
#define APP_LLM_MAX_TOKENS          600     /* reasoning models spend part of it before the JSON */
#define APP_LLM_SETTER_TEMP         0.9f
#define APP_LLM_CHECKER_TEMP        0.0f
#define APP_LLM_REPLY_MAX           2048    /* reply text kept per call */
#define APP_LLM_BODY_MAX            16384   /* larger HTTP bodies are refused */
#define APP_PUZZLE_ATTEMPTS         3       /* setter + checker rounds per puzzle before the bank */
#define APP_PUZZLE_DEAD_CHAINS      2       /* chains that failed outright in a row: stop asking the LLM */
#define APP_PUZZLE_FRESH_S          (24 * 3600) /* a stored set younger than this needs no new run at boot */
#define APP_PUZZLE_NIGHTLY_HOUR     22      /* local hour of the nightly run */
#define APP_PUZZLE_RETRY_MS         1800000u /* a run with no verified puzzle is retried after 30 min... */
#define APP_PUZZLE_RETRIES          2       /* ...at most this many times per trigger */
#define APP_PUZZLE_QUIET_S          300     /* no LLM calls this close to (or just after) the alarm */
#define APP_KEY_PUZZLES             "pz_set"
#define APP_KEY_PUZZLE_USED         "pz_used"
#define APP_CONFIG_PENDING_MS       5000u   /* one BOMB_CONFIG at a time; give up on its ACK after this */

/* ---- Briefing + coach (one LLM call after each nightly puzzle run, same setter chain) ---- */
#define APP_BRIEF_TEMP              0.7f
#define APP_BRIEF_MAX_TOKENS        600
#define APP_BRIEF_MAGIC             0x42524631u /* "BRF1": bump when BriefText changes */
#define APP_KEY_BRIEF               "brief"
#define APP_BRIEF_PROMPT_MAX        640     /* user message: alarm, <= 6 classes, history, difficulty */

/* TLS: 0 = verify servers with the core's built-in CA bundle (default), 1 = encrypted but
 * NOT server-authenticated (setInsecure). Use 1 only to diagnose a certificate problem. */
#define APP_TLS_INSECURE            0

/* ---- Serial test commands ("lead 90", "link <url>", "refresh", "puzzles", "ai", "diff") ---- */
#define APP_CMD_MAX_CHARS           (APP_LINK_MAX_CHARS + 16)
#define APP_SERIAL_RX_BUF           1024u

/* ---- Status print ---- */
#define APP_STATUS_PERIOD_MS        10000u

/* ---- Default bomb as the display assumes it until a BOMB_CONFIG is known ---- */
#define APP_DEFAULT_WIRE_COUNT      GAME_DEFAULT_WIRE_COUNT
#define APP_DEFAULT_TRIP_MIN_CM     GAME_DEFAULT_TRIP_MIN_CM
#define APP_DEFAULT_TRIP_MAX_CM     GAME_DEFAULT_TRIP_MAX_CM
#define APP_DEFAULT_MAX_STRIKES     GAME_DEFAULT_MAX_STRIKES

/* ---- Tripwire gauge range on the OLED ---- */
#define APP_GAUGE_MAX_CM            50

#endif /* APP_CONFIG_H */
