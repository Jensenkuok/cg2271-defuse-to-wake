/* Defuse to Wake - NUSMods timetable fetch and cache. See timetable.h. */
#include "timetable.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "fault.h"
#include "flash_store.h"
#include "net_tls.h"

namespace {

struct CacheBlob {
    uint32_t magic;
    uint32_t fetched_unix;
    uint32_t count;
    Lesson lessons[SCHED_MAX_LESSONS];
};

SemaphoreHandle_t s_mutex;
Lesson s_lessons[SCHED_MAX_LESSONS];   /* the live list; guarded by s_mutex */
int s_count;
uint32_t s_fetched_unix;
Lesson s_scratch[SCHED_MAX_LESSONS];   /* refresh builds here; only the network task touches it */
CacheBlob s_blob;                      /* load/save buffer (network task / boot only) */

/* Module codes go into a URL: letters and digits only. */
bool module_code_ok(const char *m)
{
    size_t i;

    if (m[0] == '\0') {
        return false;
    }
    for (i = 0; m[i] != '\0'; i++) {
        if (!((m[i] >= '0' && m[i] <= '9') || (m[i] >= 'A' && m[i] <= 'Z') || (m[i] >= 'a' && m[i] <= 'z'))) {
            return false;
        }
    }
    return true;
}

void build_filter(JsonDocument &filter)
{
    JsonObject lesson = filter["semesterData"][0]["timetable"][0].to<JsonObject>();

    filter["semesterData"][0]["semester"] = true;
    lesson["classNo"] = true;
    lesson["startTime"] = true;
    lesson["endTime"] = true;
    lesson["weeks"] = true;
    lesson["day"] = true;
    lesson["lessonType"] = true;
    lesson["venue"] = true;
}

/* weeks[] of ints -> bitmask over teaching weeks 1..13. 0 if it is not an int array. */
uint16_t weeks_mask(JsonVariantConst weeks)
{
    uint16_t mask = 0;
    int w;

    if (!weeks.is<JsonArrayConst>()) {
        return 0;           /* NUSMods can describe weeks as a date range object: not supported */
    }
    for (JsonVariantConst v : weeks.as<JsonArrayConst>()) {
        w = v | 0;
        if (w >= 1 && w <= SCHED_LAST_WEEK) {
            mask = (uint16_t)(mask | (1u << (w - 1)));
        }
    }
    return mask;
}

/* Does this API lesson belong to one of the user's slots for this module? Returns the abbrev or NULL. */
const char *matching_abbrev(const ShareLink &link, const char *module, const char *lesson_type, const char *class_no)
{
    int i;
    const char *name;

    for (i = 0; i < link.count; i++) {
        name = sched_lesson_type_name(link.slots[i].abbrev);
        if (strcmp(link.slots[i].module, module) == 0 && name != NULL && strcmp(name, lesson_type) == 0 &&
            sched_class_no_equal(link.slots[i].class_no, class_no)) {
            return link.slots[i].abbrev;
        }
    }
    return NULL;
}

/* Convert one API lesson; false if it should be skipped (bad fields or no matching slot). */
bool convert_lesson(JsonObjectConst les, const ShareLink &link, const char *module, Lesson *out)
{
    const char *abbrev = matching_abbrev(link, module, les["lessonType"] | "", les["classNo"] | "");
    int day = sched_weekday_from_name(les["day"] | "");
    int start = sched_parse_hhmm(les["startTime"] | "");
    int end = sched_parse_hhmm(les["endTime"] | "");
    uint16_t weeks = weeks_mask(les["weeks"]);
    const char *venue = les["venue"] | "";

    if (abbrev == NULL || day == 0 || start < 0 || end < 0 || weeks == 0u) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->module, sizeof(out->module), "%s", module);
    snprintf(out->type, sizeof(out->type), "%s", abbrev);
    out->weekday = (uint8_t)day;
    out->start_min = (uint16_t)start;
    out->end_min = (uint16_t)end;
    out->weeks = weeks;
    snprintf(out->venue, sizeof(out->venue), "%s", venue);   /* long venues are truncated */
    return true;
}

/* Append the matching semester lessons of a parsed module document. */
int collect_lessons(JsonDocument &doc, const ShareLink &link, const char *module, int *total)
{
    int added = 0;

    for (JsonObjectConst sem : doc["semesterData"].as<JsonArrayConst>()) {
        if ((int)(sem["semester"] | 0) != APP_NUSMODS_SEMESTER) {
            continue;
        }
        for (JsonObjectConst les : sem["timetable"].as<JsonArrayConst>()) {
            if (*total >= SCHED_MAX_LESSONS) {
                Serial.printf("[tt] %s: more than %d lessons, extra ones ignored\n", module, SCHED_MAX_LESSONS);
                return added;
            }
            if (convert_lesson(les, link, module, &s_scratch[*total])) {
                (*total)++;
                added++;
            }
        }
    }
    return added;
}

/* GET one module and add its lessons to s_scratch. False on any HTTP/JSON failure. */
bool fetch_module(const char *module, const ShareLink &link, int *total, int *added)
{
    char url[APP_TT_MODULE_URL_MAX];
    NetworkClientSecure client;
    HTTPClient http;
    JsonDocument filter;
    JsonDocument doc;
    DeserializationError err;
    int code;

    if (fault_net_blocked()) {
        Serial.printf("[tt] %s: simulated outage\n", module);   /* the web page's fault button: fail fast */
        return false;
    }
    snprintf(url, sizeof(url), "%s%s.json", APP_NUSMODS_URL_PREFIX, module);
    net_tls_configure(client);
    http.setConnectTimeout(APP_HTTP_TIMEOUT_MS);
    http.setTimeout(APP_HTTP_TIMEOUT_MS);
    http.useHTTP10(true);              /* no chunked encoding: the body can be parsed straight off the stream */
    if (!http.begin(client, url)) {
        Serial.printf("[tt] %s: could not start the request\n", module);
        return false;
    }
    code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[tt] %s: HTTP %d (%s)\n", module, code, code < 0 ? HTTPClient::errorToString(code).c_str() : "");
        http.end();
        return false;
    }
    build_filter(filter);
    err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) {
        Serial.printf("[tt] %s: JSON error %s\n", module, err.c_str());
        return false;
    }
    *added = collect_lessons(doc, link, module, total);
    return true;
}

bool already_fetched(const ShareLink &link, int idx)
{
    int j;

    for (j = 0; j < idx; j++) {
        if (strcmp(link.slots[j].module, link.slots[idx].module) == 0) {
            return true;
        }
    }
    return false;
}

void save_cache(void)
{
    memset(&s_blob, 0, sizeof(s_blob));
    s_blob.magic = APP_TT_CACHE_MAGIC;
    s_blob.fetched_unix = s_fetched_unix;
    s_blob.count = (uint32_t)s_count;
    memcpy(s_blob.lessons, s_lessons, sizeof(s_lessons));
    if (!flash_write_blob(APP_KEY_TIMETABLE, &s_blob, sizeof(s_blob))) {
        Serial.println("[tt] could not write the cache to flash");
    }
}

}  // namespace

bool timetable_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex != NULL;
}

bool timetable_load_cache(void)
{
    bool ok = flash_read_blob(APP_KEY_TIMETABLE, &s_blob, sizeof(s_blob)) && s_blob.magic == APP_TT_CACHE_MAGIC &&
              s_blob.count <= SCHED_MAX_LESSONS;

    if (!ok) {
        Serial.println("[tt] no timetable cache in flash");
        return false;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(APP_FLASH_MUTEX_WAIT_MS)) != pdTRUE) {
        return false;
    }
    memcpy(s_lessons, s_blob.lessons, sizeof(s_lessons));
    s_count = (int)s_blob.count;
    s_fetched_unix = s_blob.fetched_unix;
    xSemaphoreGive(s_mutex);
    Serial.printf("[tt] cache loaded: %d lessons (fetched at unix %u)\n", s_count, (unsigned)s_fetched_unix);
    return true;
}

bool timetable_refresh(const char *share_link, uint32_t now_unix)
{
    static ShareLink link;       /* ~630 bytes: static keeps it off the network task's stack */
    int total = 0;
    int added = 0;
    int i;

    if (!parse_share_link(share_link, &link) || link.count == 0u) {
        Serial.println("[tt] refresh skipped: the share link has no usable slots");
        return false;
    }
    if (link.skipped > 0u) {
        Serial.printf("[tt] %u share-link entries skipped (unknown or malformed)\n", (unsigned)link.skipped);
    }
    for (i = 0; i < link.count; i++) {
        if (already_fetched(link, i)) {
            continue;
        }
        if (!module_code_ok(link.slots[i].module)) {
            Serial.printf("[tt] refresh failed: bad module code in the link\n");
            return false;
        }
        if (!fetch_module(link.slots[i].module, link, &total, &added)) {
            Serial.println("[tt] refresh failed: keeping the previous timetable");
            return false;
        }
        Serial.printf("[tt] %s: %d lessons\n", link.slots[i].module, added);
    }
    if (total == 0) {
        Serial.println("[tt] refresh failed: no lesson matched the selected slots, keeping the previous timetable");
        return false;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(APP_FLASH_MUTEX_WAIT_MS)) != pdTRUE) {
        return false;
    }
    memset(s_lessons, 0, sizeof(s_lessons));
    memcpy(s_lessons, s_scratch, sizeof(Lesson) * (size_t)total);
    s_count = total;
    s_fetched_unix = now_unix;
    xSemaphoreGive(s_mutex);
    save_cache();
    Serial.printf("[tt] timetable updated: %d lessons\n", total);
    return true;
}

int timetable_copy(Lesson *out, int max)
{
    int n = 0;

    if (out == NULL || max <= 0 || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(APP_FLASH_MUTEX_WAIT_MS)) != pdTRUE) {
        return 0;
    }
    n = (s_count < max) ? s_count : max;
    memcpy(out, s_lessons, sizeof(Lesson) * (size_t)n);
    xSemaphoreGive(s_mutex);
    return n;
}

int timetable_count(void)
{
    return s_count;
}

uint32_t timetable_fetched_unix(void)
{
    return s_fetched_unix;
}
