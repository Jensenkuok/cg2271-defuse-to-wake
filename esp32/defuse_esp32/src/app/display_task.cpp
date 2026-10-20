/* Defuse to Wake - OLED display task. See display_task.h. */
#include "display_task.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "i2c_bus.h"

#define SEC_PER_MIN        60u
#define SEC_PER_HOUR       3600u
#define SEC_PER_DAY        86400u
#define CHAR_W             6      /* default font: 5 px glyph + 1 px gap, at text size 1 */
#define CHAR_H             8
#define CLUE_Y             56
/* ACTIVE layout, y in pixels (64 px = 8 text rows of 8 px). Top bar: timer + strikes, y 0..15. */
#define DIAL_BOX_Y         17     /* digit boxes y 17..34 (size-2 digits) */
#define DIAL_BOX_W         24
#define DIAL_BOX_H         18
#define DIAL_BOX_PITCH     32
#define DIAL_X0            4
#define DIAL_DIGITS        4
#define DIAL_MARK_Y        36     /* bar under the digit being entered */
#define DIAL_TEXT_Y        40     /* puzzle text: 3 rows (y 40, 48, 56) */
#define DIAL_TEXT_ROWS     3
#define WIRES_X0           4
#define WIRES_SPAN         120
#define WIRE_TOP           17     /* wires y 17..34 */
#define WIRE_H             18
#define WIRE_STUB          7
#define WIRE_NUM_Y         36     /* wire numbers 1..N, y 36..43 */
#define WIRES_TEXT_Y       48     /* puzzle text: 2 rows (y 48, 56) */
#define WIRES_TEXT_ROWS    2
#define DEFUSED_TEXT_Y     16     /* DEFUSED page text: 6 rows (y 16 .. 63) under the size-2 title */
#define DEFUSED_TEXT_ROWS  6
#define TEXT_ROWS_MAX      6
#define PAGE_DOT           5
#define PAGE_DOT_PITCH     7
#define PAGE_DOT_X0        1      /* left of the centred size-2 "DEFUSED" title (x 22..105) */
#define PAGE_DOT_Y         5
#define UNKNOWN_PUZZLE     "Puzzle: see dial"
#define GAUGE_X0           4
#define GAUGE_W            120
#define GAUGE_Y            26
#define GAUGE_H            10
#define HOLD_Y             44
#define HOLD_H             8

static TaskHandle_t s_task;
static Adafruit_SSD1306 s_display(APP_OLED_WIDTH, APP_OLED_HEIGHT, &Wire, -1);

static const char *const k_screen_names[] = { "IDLE", "ARMED", "BOMB_ACTIVE", "DEFUSED", "EXPLODED" };
static const char *const k_module_names[] = { "-", "DIAL", "WIRES", "TRIPWIRE" };

/* ================= model (pure) ================= */
static void fmt_mmss(char *dst, size_t n, uint32_t seconds)
{
    snprintf(dst, n, "%02u:%02u", (unsigned)((seconds / SEC_PER_MIN) % 100u), (unsigned)(seconds % SEC_PER_MIN));
}

/* The puzzle of the ACTIVE pair for the module on screen, plus its hint. The pair is the config the
 * MCX ACKed, i.e. what the running (or next) bomb asks for. Until one is known the MCX is assumed
 * to run its default bomb (game_config.h), whose clue comes from the template bank; the literal
 * UNKNOWN_PUZZLE text is the last resort if even that cannot be rendered. */
static void fill_puzzle(const AppState &st, ScreenModel *out)
{
    const bool known = st.active.valid;
    uint8_t code[PROTO_DIAL_DIGITS] = { GAME_DEFAULT_DIAL_CODE_0, GAME_DEFAULT_DIAL_CODE_1, GAME_DEFAULT_DIAL_CODE_2,
                                        GAME_DEFAULT_DIAL_CODE_3 };
    int wires = (int)GAME_DEFAULT_WIRE_COUNT;
    int wire_k = (int)GAME_DEFAULT_CORRECT_WIRE + 1;     /* game_config.h is zero-based */
    const char *stored = "";

    if (known) {
        memcpy(code, st.active.dial_code, sizeof(code));
        wires = st.active.wire_count;
        wire_k = st.active.wire_k;
    }
    if (out->module_type == PROTO_MODULE_DIAL) {
        stored = known ? st.active.dial_text : "";
        if (stored[0] == '\0') {
            (void)dial_template(code, 0u, out->puzzle_text, sizeof(out->puzzle_text));
        }
        (void)dial_hint(code, st.game.strikes, out->hint, sizeof(out->hint));
    } else if (out->module_type == PROTO_MODULE_WIRES) {
        stored = known ? st.active.wire_text : "";
        if (stored[0] == '\0') {
            (void)wire_template(wire_k, wires, 0u, out->puzzle_text, sizeof(out->puzzle_text));
        }
        out->wire_count = (uint8_t)wires;               /* the drawing follows the MCX's wire count */
        out->hint_wire = (int8_t)wire_hint_index(wire_k, wires, st.game.strikes, out->wire_cut_mask);
        (void)wire_hint(wire_k, wires, st.game.strikes, out->wire_cut_mask, out->hint, sizeof(out->hint));
    } else {
        return;
    }
    if (stored[0] != '\0') {
        snprintf(out->puzzle_text, sizeof(out->puzzle_text), "%s", stored);
    }
    if (out->puzzle_text[0] == '\0') {
        snprintf(out->puzzle_text, sizeof(out->puzzle_text), "%s", UNKNOWN_PUZZLE);
    }
}

/* DEFUSED pages: the result line, the briefing, the coach message. The stored text counts only if
 * it is for today; otherwise today's template (timetable + history) is used. */
static void fill_defused(const AppState &st, uint32_t unix_now, uint32_t defused_ms, ScreenModel *out)
{
    const BriefText *bt = (unix_now != 0u) ? brief_pick(st.brief, st.brief_today, local_day(unix_now)) : NULL;

    out->defused_page = (uint8_t)defused_page(defused_ms);
    if (st.result_valid && st.last_result.outcome == PROTO_OUTCOME_DEFUSED) {
        (void)result_line(st.last_result.duration_s, st.last_result.strikes, st.last_result.snoozes, out->result_text,
                          sizeof(out->result_text));
    } else {
        snprintf(out->result_text, sizeof(out->result_text), "Defused!");
    }
    snprintf(out->brief_text, sizeof(out->brief_text), "%s", bt != NULL ? bt->briefing : APP_BRIEF_GENERIC);
    snprintf(out->coach_text, sizeof(out->coach_text), "%s", bt != NULL ? bt->coach : APP_COACH_GENERIC);
}

void build_model(const AppState &st, uint32_t now_ms, uint32_t unix_now, uint32_t defused_ms, ScreenModel *out)
{
    uint32_t lo;
    uint32_t hi;
    uint32_t local;

    memset(out, 0, sizeof(*out));
    out->hint_wire = -1;
    out->screen = SCR_IDLE;
    if (st.game_valid) {
        switch (st.game.state) {
        case PROTO_STATE_ARMED:       out->screen = SCR_ARMED; break;
        case PROTO_STATE_BOMB_ACTIVE: out->screen = SCR_ACTIVE; break;
        case PROTO_STATE_DEFUSED:     out->screen = SCR_DEFUSED; break;
        case PROTO_STATE_EXPLODED:    out->screen = SCR_EXPLODED; break;
        default:                      out->screen = SCR_IDLE; break;
        }
    }
    out->link_down = !st.link_up;
    out->wifi_up = st.wifi_up;
    snprintf(out->ip_text, sizeof(out->ip_text), "%s", st.wifi_up ? st.ip : "");
    out->time_valid = st.time_valid;
    out->colon_on = ((now_ms / 500u) % 2u) == 0u;
    out->invert = (out->screen == SCR_EXPLODED) && (((now_ms / APP_BOOM_FLASH_MS) % 2u) == 0u);

    if (st.time_valid && unix_now != 0u) {
        local = unix_now + (uint32_t)APP_TZ_OFFSET_S; /* UTC+8 applied by hand: no TZ env needed */
        local %= SEC_PER_DAY;
        snprintf(out->clock_text, sizeof(out->clock_text), "%02u:%02u", (unsigned)(local / SEC_PER_HOUR),
                 (unsigned)((local % SEC_PER_HOUR) / SEC_PER_MIN));
    } else {
        snprintf(out->clock_text, sizeof(out->clock_text), "--:--");
    }

    if (st.next_valid && st.alarm_unix != 0u) {
        /* The wake-up time (alarm), not the class start: that is what the sleeper cares about. */
        static const char *const k_weekday[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        time_t at = (time_t)(st.alarm_unix + (uint32_t)APP_TZ_OFFSET_S);
        struct tm tm_local;
        char full[SCHED_MODULE_LEN + SCHED_VENUE_LEN + 16] = { 0 };

        gmtime_r(&at, &tm_local);   /* UTC+8 added by hand, as for the clock above */
        snprintf(full, sizeof(full), "%s %02d:%02d %s %s", k_weekday[tm_local.tm_wday % 7], tm_local.tm_hour,
                 tm_local.tm_min, st.next_module, st.next_venue);
        memcpy(out->next_text, full, sizeof(out->next_text) - 1u);   /* longer text is cut to the 21-char screen width */
    }

    out->ring_in_s = (st.alarm_unix != 0u && st.time_valid && st.alarm_unix > unix_now)
                         ? (int32_t)(st.alarm_unix - unix_now) : -1;

    fmt_mmss(out->timer_text, sizeof(out->timer_text), st.game.time_left_s);
    out->strikes = st.game.strikes;
    out->max_strikes = st.max_strikes;
    out->module_type = st.game.module_type;

    /* module_value, exactly as documented in docs/game.md:
     * DIAL = value | digit_index << 8; WIRES = cursor | cut_mask << 8;
     * TRIPWIRE = min(distance_cm, 255) | hold_pct << 8. */
    lo = (uint32_t)st.game.module_value & 0xFFu;
    hi = ((uint32_t)st.game.module_value >> 8) & 0xFFu;
    out->dial_value = (uint8_t)lo;
    out->dial_digit = (uint8_t)hi;
    out->wire_count = st.wire_count;
    out->wire_cursor = (uint8_t)lo;
    out->wire_cut_mask = (uint8_t)hi;
    out->trip_cm = (uint8_t)lo;
    out->trip_hold_pct = (uint8_t)hi;
    out->trip_min_cm = st.trip_min_cm;
    out->trip_max_cm = st.trip_max_cm;

    if (st.result_valid && st.last_result.outcome == PROTO_OUTCOME_DEFUSED) {
        fmt_mmss(out->taken_text, sizeof(out->taken_text), st.last_result.duration_s);
    } else {
        snprintf(out->taken_text, sizeof(out->taken_text), "--:--");
    }
    snprintf(out->clue, sizeof(out->clue), "%.*s", APP_CLUE_MAX_CHARS, APP_CLUE_PLACEHOLDER);

    if (out->screen == SCR_ACTIVE) {
        fill_puzzle(st, out);
    } else if (out->screen == SCR_DEFUSED) {
        fill_defused(st, unix_now, defused_ms, out);
    }
}

/* ================= text rows (shared by the OLED and the headless print) ================= */
struct TextBlock {
    int y0;                                  /* pixel row of the first text row */
    int rows;                                /* 0 = this screen has no text block */
    char line[TEXT_ROWS_MAX][TEXT_COLS + 1];
};

static_assert(APP_CLUE_MAX_CHARS == TEXT_COLS, "the OLED text width is 21 columns");

/* The size-1 text of a screen: puzzle text wrapped into the rows left under the module graphic (the
 * last row scrolls at MARQUEE_CPS if it does not fit), with the hint replacing the last row once
 * there is a strike; on DEFUSED the rotating page, wrapped the same way. */
static void layout_text(const ScreenModel &m, uint32_t now_ms, TextBlock *tb)
{
    const uint32_t offset = marquee_offset(now_ms, MARQUEE_CPS);
    const char *text = "";
    int text_rows = 0;

    memset(tb, 0, sizeof(*tb));
    if (m.screen == SCR_ACTIVE && m.module_type == PROTO_MODULE_DIAL) {
        tb->y0 = DIAL_TEXT_Y;
        tb->rows = DIAL_TEXT_ROWS;
        text = m.puzzle_text;
    } else if (m.screen == SCR_ACTIVE && m.module_type == PROTO_MODULE_WIRES) {
        tb->y0 = WIRES_TEXT_Y;
        tb->rows = WIRES_TEXT_ROWS;
        text = m.puzzle_text;
    } else if (m.screen == SCR_DEFUSED) {
        tb->y0 = DEFUSED_TEXT_Y;
        tb->rows = DEFUSED_TEXT_ROWS;
        text = (m.defused_page == 0u) ? m.result_text : ((m.defused_page == 1u) ? m.brief_text : m.coach_text);
    } else {
        return;
    }
    text_rows = tb->rows;
    if (m.screen == SCR_ACTIVE && m.hint[0] != '\0') {
        text_rows--;                         /* the hint takes the last row */
        snprintf(tb->line[tb->rows - 1], sizeof(tb->line[0]), "%.*s", TEXT_COLS, m.hint);
    }
    (void)fit_text(text, TEXT_COLS, text_rows, offset, tb->line[0], sizeof(tb->line[0]));
}

/* ================= headless summary ================= */
static void print_summary(const ScreenModel &m, const TextBlock &tb)
{
    int i;

    Serial.printf("[oled-off] %s link=%s wifi=%s", k_screen_names[m.screen], m.link_down ? "DOWN" : "up",
                  m.wifi_up ? "up" : "down");
    switch (m.screen) {
    case SCR_IDLE:
        Serial.printf(" clock=%s next=[%s] ip=%s", m.clock_text, m.next_text, m.ip_text[0] != '\0' ? m.ip_text : "-");
        break;
    case SCR_ARMED:
        Serial.printf(" ring_in=%ds", (int)m.ring_in_s);
        break;
    case SCR_ACTIVE:
        Serial.printf(" t=%s strikes=%u/%u %s", m.timer_text, m.strikes, m.max_strikes,
                      m.module_type < 4 ? k_module_names[m.module_type] : "?");
        if (m.module_type == PROTO_MODULE_DIAL) {
            Serial.printf(" digit=%u val=%u", m.dial_digit, m.dial_value);
        } else if (m.module_type == PROTO_MODULE_WIRES) {
            Serial.printf(" wires=%u cursor=%u cut=0x%02x", m.wire_count, m.wire_cursor, m.wire_cut_mask);
        } else if (m.module_type == PROTO_MODULE_TRIPWIRE) {
            Serial.printf(" dist=%ucm band=%u..%u hold=%u%%", m.trip_cm, m.trip_min_cm, m.trip_max_cm,
                          m.trip_hold_pct);
        }
        break;
    case SCR_DEFUSED:
        Serial.printf(" taken=%s page=%u", m.taken_text, m.defused_page);
        break;
    default:
        break;
    }
    if (tb.rows > 0) {                       /* the same rows the OLED would draw, joined with " / " */
        Serial.print(" text=[");
        for (i = 0; i < tb.rows; i++) {
            if (tb.line[i][0] != '\0') {
                Serial.printf("%s%s", i > 0 ? " / " : "", tb.line[i]);
            }
        }
        Serial.print("]");
    }
    Serial.println();
}

/* ================= OLED drawing ================= */
static void text_centered(const char *s, int y, int size)
{
    int w = (int)strlen(s) * CHAR_W * size;

    s_display.setTextSize(size);
    s_display.setCursor((APP_OLED_WIDTH - w) / 2, y);
    s_display.print(s);
}

static void draw_wifi_icon(int x, int y, bool up)
{
    int i;
    int h;

    for (i = 0; i < 4; i++) {
        h = 2 + 2 * i;
        if (up) {
            s_display.fillRect(x + i * 4, y + 8 - h, 3, h, SSD1306_WHITE);
        } else {
            s_display.drawRect(x + i * 4, y + 8 - h, 3, h, SSD1306_WHITE);
        }
    }
}

/* Two linked rings; crossed out with a slash when the link is down. */
static void draw_link_icon(int x, int y, bool up)
{
    s_display.drawRoundRect(x, y + 1, 8, 6, 2, SSD1306_WHITE);
    s_display.drawRoundRect(x + 5, y + 1, 8, 6, 2, SSD1306_WHITE);
    if (!up) {
        s_display.drawLine(x, y + 8, x + 13, y - 1, SSD1306_WHITE);
    }
}

static void draw_dial(const ScreenModel &m)
{
    char c[2] = { 0, 0 };
    int i;
    int x;

    for (i = 0; i < DIAL_DIGITS; i++) {
        x = DIAL_X0 + i * DIAL_BOX_PITCH;
        s_display.drawRect(x, DIAL_BOX_Y, DIAL_BOX_W, DIAL_BOX_H, SSD1306_WHITE);
        if (i < m.dial_digit) {
            c[0] = '*';                          /* locked */
        } else if (i == m.dial_digit) {
            c[0] = (char)('0' + (m.dial_value % 10)); /* being entered */
        } else {
            c[0] = '-';                          /* not reached yet */
        }
        s_display.setTextSize(2);                /* 12 x 16 px glyph in an 24 x 18 px box */
        s_display.setCursor(x + (DIAL_BOX_W - 2 * CHAR_W) / 2, DIAL_BOX_Y + 1);
        s_display.print(c);
        if (i == m.dial_digit) {
            s_display.fillRect(x + 4, DIAL_MARK_Y, DIAL_BOX_W - 8, 2, SSD1306_WHITE); /* caret bar */
        }
    }
}

static void draw_wires(const ScreenModel &m)
{
    int n = (m.wire_count < 1) ? 1 : ((m.wire_count > 8) ? 8 : m.wire_count);
    int span = WIRES_SPAN / n;
    int i;
    int x;
    char num[2] = { 0, 0 };

    for (i = 0; i < n; i++) {
        x = WIRES_X0 + span * i + span / 2;
        if ((m.wire_cut_mask >> i) & 1u) {
            /* cut: two stubs with a gap and frayed ends */
            s_display.drawFastVLine(x, WIRE_TOP, WIRE_STUB, SSD1306_WHITE);
            s_display.drawFastVLine(x + 1, WIRE_TOP, WIRE_STUB, SSD1306_WHITE);
            s_display.drawFastVLine(x, WIRE_TOP + WIRE_H - WIRE_STUB, WIRE_STUB, SSD1306_WHITE);
            s_display.drawFastVLine(x + 1, WIRE_TOP + WIRE_H - WIRE_STUB, WIRE_STUB, SSD1306_WHITE);
            s_display.drawLine(x, WIRE_TOP + WIRE_STUB, x - 3, WIRE_TOP + WIRE_STUB + 3, SSD1306_WHITE);
            s_display.drawLine(x + 1, WIRE_TOP + WIRE_H - WIRE_STUB, x + 4, WIRE_TOP + WIRE_H - WIRE_STUB - 3,
                               SSD1306_WHITE);
        } else {
            s_display.drawFastVLine(x, WIRE_TOP, WIRE_H, SSD1306_WHITE);
            s_display.drawFastVLine(x + 1, WIRE_TOP, WIRE_H, SSD1306_WHITE);
        }
        /* The wire number under each wire; the cursor is that number in inverse video; the wire the
         * hint rules out (i == hint_wire) shows an X instead of its number. */
        num[0] = (i == m.hint_wire) ? 'X' : (char)('1' + i);
        if (i == m.wire_cursor) {
            s_display.fillRect(x - 3, WIRE_NUM_Y - 1, CHAR_W + 2, CHAR_H + 1, SSD1306_WHITE);
            s_display.setTextColor(SSD1306_BLACK);
        }
        s_display.setTextSize(1);
        s_display.setCursor(x - CHAR_W / 2 + 1, WIRE_NUM_Y);
        s_display.print(num);
        s_display.setTextColor(SSD1306_WHITE);
    }
}

static int gauge_px(int cm)
{
    if (cm < 0) { cm = 0; }
    if (cm > APP_GAUGE_MAX_CM) { cm = APP_GAUGE_MAX_CM; }
    return GAUGE_X0 + (cm * GAUGE_W) / APP_GAUGE_MAX_CM;
}

static void draw_tripwire(const ScreenModel &m)
{
    char buf[12];
    int bx0 = gauge_px(m.trip_min_cm);
    int bx1 = gauge_px(m.trip_max_cm);
    int mx = gauge_px(m.trip_cm);
    int fill = ((GAUGE_W - 2) * m.trip_hold_pct) / 100;

    s_display.drawRect(GAUGE_X0, GAUGE_Y, GAUGE_W + 1, GAUGE_H, SSD1306_WHITE);          /* 0..50 cm bar */
    s_display.drawRect(bx0, GAUGE_Y - 4, (bx1 - bx0) + 1, GAUGE_H + 8, SSD1306_WHITE);   /* band bracket */
    s_display.fillRect(mx - 1, GAUGE_Y + 1, 3, GAUGE_H - 2, SSD1306_WHITE);             /* current distance */
    if (m.trip_cm > APP_GAUGE_MAX_CM) {
        s_display.setTextSize(1);
        s_display.setCursor(GAUGE_X0 + GAUGE_W - CHAR_W, GAUGE_Y - 12);
        s_display.print(">");
    }
    s_display.setTextSize(1);
    s_display.setCursor(GAUGE_X0, GAUGE_Y + GAUGE_H + 4);
    s_display.print("0");
    snprintf(buf, sizeof(buf), "%ucm", m.trip_cm);
    s_display.setCursor((APP_OLED_WIDTH - (int)strlen(buf) * CHAR_W) / 2, GAUGE_Y + GAUGE_H + 4);
    s_display.print(buf);
    s_display.setCursor(GAUGE_X0 + GAUGE_W - 2 * CHAR_W, GAUGE_Y + GAUGE_H + 4);
    s_display.print("50");

    s_display.drawRect(GAUGE_X0, HOLD_Y + 4, GAUGE_W + 1, HOLD_H, SSD1306_WHITE);        /* hold progress */
    s_display.fillRect(GAUGE_X0 + 1, HOLD_Y + 5, fill, HOLD_H - 2, SSD1306_WHITE);
}

static void draw_text_block(const TextBlock &tb)
{
    int i;

    s_display.setTextSize(1);
    for (i = 0; i < tb.rows; i++) {
        s_display.setCursor(0, tb.y0 + i * CHAR_H);
        s_display.print(tb.line[i]);
    }
}

static void draw_active(const ScreenModel &m, const TextBlock &tb)
{
    int i;
    int x;

    s_display.setTextSize(2);
    s_display.setCursor(0, 0);
    s_display.print(m.timer_text);
    for (i = 0; i < m.max_strikes && i < 5; i++) {              /* strike marks, right aligned */
        x = APP_OLED_WIDTH - (m.max_strikes - i) * 12;
        if (i < m.strikes) {
            s_display.fillRect(x, 3, 9, 9, SSD1306_WHITE);
        } else {
            s_display.drawRect(x, 3, 9, 9, SSD1306_WHITE);
        }
    }
    if (m.link_down) {
        draw_link_icon(66, 4, false);
    }
    switch (m.module_type) {
    case PROTO_MODULE_DIAL:     draw_dial(m); break;
    case PROTO_MODULE_WIRES:    draw_wires(m); break;
    case PROTO_MODULE_TRIPWIRE: draw_tripwire(m); break;
    default: break;
    }
    if (m.module_type == PROTO_MODULE_TRIPWIRE) {
        s_display.setTextSize(1);                /* tripwire screen unchanged */
        s_display.setCursor(0, CLUE_Y);
        s_display.print(m.clue);
    } else {
        draw_text_block(tb);                     /* dial / wires: puzzle text + hint */
    }
}

static void draw_oled(const ScreenModel &m, const TextBlock &tb)
{
    char buf[24];

    s_display.clearDisplay();
    s_display.setTextColor(SSD1306_WHITE);
    switch (m.screen) {
    case SCR_IDLE:
        snprintf(buf, sizeof(buf), "%s", m.clock_text);
        if (!m.colon_on && m.time_valid) {
            buf[2] = ' ';
        }
        text_centered(buf, 22, 4);
        if (m.next_text[0] != '\0') {
            s_display.setTextSize(1);
            s_display.setCursor(0, CLUE_Y);
            s_display.print(m.next_text);
        }
        if (m.ip_text[0] != '\0') {
            /* The web page's address, small, in the top-left corner: the bottom row is the next-alarm
             * line (up to the full 21 columns) and the clock fills the rows between. */
            s_display.setTextSize(1);
            s_display.setCursor(0, 0);
            s_display.print(m.ip_text);
        }
        draw_wifi_icon(96, 0, m.wifi_up);
        draw_link_icon(114, 0, !m.link_down);
        break;
    case SCR_ARMED:
        text_centered("ARMED", 6, 3);
        if (m.ring_in_s >= 0) {
            text_centered("RING IN", 36, 1);
            fmt_mmss(buf, sizeof(buf), (uint32_t)m.ring_in_s);
            text_centered(buf, 46, 2);
        } else {
            text_centered("waiting...", 40, 1);
        }
        if (m.link_down) {
            draw_link_icon(114, 0, false);
        }
        break;
    case SCR_ACTIVE:
        draw_active(m, tb);
        break;
    case SCR_DEFUSED:
        /* 30 s hold: three pages, one per DEFUSED_PAGE_MS (result line, briefing, coach) */
        text_centered("DEFUSED", 0, 2);
        for (int p = 0; p < DEFUSED_PAGES; p++) {
            if (p == m.defused_page) {
                s_display.fillRect(PAGE_DOT_X0 + p * PAGE_DOT_PITCH, PAGE_DOT_Y, PAGE_DOT, PAGE_DOT, SSD1306_WHITE);
            } else {
                s_display.drawRect(PAGE_DOT_X0 + p * PAGE_DOT_PITCH, PAGE_DOT_Y, PAGE_DOT, PAGE_DOT, SSD1306_WHITE);
            }
        }
        draw_text_block(tb);
        if (m.link_down) {
            draw_link_icon(114, 0, false);
        }
        break;
    default: /* SCR_EXPLODED */
        text_centered("BOOM", 20, 4);
        break;
    }
}

/* ================= task ================= */
static void display_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    bool oled_ok = false;
    bool inverted = false;
    bool was_defused = false;
    uint32_t defused_since = 0;
    uint32_t last_print = 0;
    static AppState st;              /* static: the task is created once, and ~1.5 KB of copies would eat its 6 KB stack */
    static ScreenModel m;
    static TextBlock tb;
    uint32_t now;
    bool defused;
    time_t t;

    (void)arg;
    if (i2c_bus_probe(APP_OLED_ADDR) && i2c_bus_lock(APP_I2C_MUTEX_WAIT_MS)) {
        /* periphBegin = false: Wire was already started by i2c_bus_init() */
        oled_ok = s_display.begin(SSD1306_SWITCHCAPVCC, APP_OLED_ADDR, true, false);
        i2c_bus_unlock();
    }
    if (oled_ok) {
        Serial.println("[display] OLED ready");
    } else {
        Serial.println("[display] OLED not found: headless mode (state summary on Serial, 1 Hz)");
    }

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_DISPLAY_PERIOD_MS));
        if (!app_state_read(&st)) {
            continue;
        }
        now = millis();
        t = time(nullptr);
        defused = st.game_valid && st.game.state == PROTO_STATE_DEFUSED;
        if (defused && !was_defused) {
            defused_since = now;             /* page rotation restarts at the first DEFUSED frame */
        }
        was_defused = defused;
        build_model(st, now, st.time_valid ? (uint32_t)t : 0u, defused ? (uint32_t)(now - defused_since) : 0u, &m);
        layout_text(m, now, &tb);

        if (!oled_ok) {
            if ((uint32_t)(now - last_print) >= APP_HEADLESS_PRINT_MS) {
                last_print = now;
                print_summary(m, tb);
            }
            continue;
        }
        draw_oled(m, tb);                   /* RAM only: no bus traffic yet */
        if (i2c_bus_lock(APP_I2C_MUTEX_WAIT_MS)) {
            if (m.invert != inverted) {
                inverted = m.invert;
                s_display.invertDisplay(inverted);
            }
            s_display.display();            /* ~25 ms at 400 kHz; the gyro waits (bounded) */
            i2c_bus_unlock();
        }
    }
}

bool display_task_start(void)
{
    return xTaskCreate(display_task, "display", APP_STACK_DISPLAY, NULL, APP_PRIO_DISPLAY, &s_task) == pdPASS;
}

TaskHandle_t display_task_handle(void)
{
    return s_task;
}
