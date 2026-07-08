#include "weather_fetch.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "wifi_bsp.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "miniz.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "weather_fetch";

/* Kconfig / NVS config (populated in WeatherFetch_Init). These char arrays
 * are read by data_task inside WeatherFetch_Update (snprintf into the
 * request URL) and overwritten by WeatherFetch_ReloadConfig from the HTTP
 * server task. A torn write mid-buffer would let the reader build a request
 * with a half-updated key/location. The read and the write are short and
 * run at well-defined points, so a critical section around both sides is
 * the cheapest correct guard. */
static char s_api_key[WEATHER_API_KEY_LEN];
static char s_location_id[WEATHER_LOCATION_ID_LEN];
static char s_city_name[WEATHER_CITY_NAME_LEN];
static char s_base_url[64];

static char s_last_error[64];
static bool s_initialized;
/* Manual-refresh flag, set from the HTTP server task and consumed by
 * data_task. Plain volatile is not sufficient on the dual-core ESP32-S3
 * (no ordering/atomicity guarantee); use a critical section so the
 * check-then-clear is atomic and the flag can't be dropped between the
 * read and the write. */
static volatile bool s_refresh_requested;
static portMUX_TYPE s_refresh_spinlock = portMUX_INITIALIZER_UNLOCKED;
/* Reuse the same spinlock to make config-buffer updates atomic vs the
 * data_task reader. portENTER/EXIT_CRITICAL disable interrupts on the
 * current core, which is enough for short memcpy/strcpy guards. */
static portMUX_TYPE s_config_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* Per-Update config snapshot. WeatherFetch_Update snapshots all four
 * config buffers under s_config_spinlock at the start of a fetch, then
 * reads these (not the live statics) for the duration of the multi-
 * second, 3-endpoint fetch. This keeps http_get_json (base_url) and
 * parse_now (city_name) on the same config epoch as the api_key/
 * location_id used in the URL, even if ReloadConfig overwrites the live
 * statics mid-fetch from the HTTP task. s_fetch_in_progress gates the
 * read; only Update writes these. */
static char s_fetch_base_url[64];
static char s_fetch_city_name[WEATHER_CITY_NAME_LEN];
static bool s_fetch_in_progress;

/* ── helpers ── */

static void safe_strcpy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    size_t len = src ? strlen(src) : 0;
    if (len >= dst_size) len = dst_size - 1;
    if (src && len) memcpy(dst, src, len);
    dst[len] = '\0';
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    s_last_error[sizeof(s_last_error) - 1] = '\0';
    va_end(ap);
}

/* Build a QWeather icon code into our compact icon field.
 * Keeps first 3 digits, max 7 chars + NUL. */
static void parse_icon_code(char *out, size_t out_size, const char *raw)
{
    char buf[8] = {0};
    int j = 0;
    if (raw) {
        for (size_t i = 0; raw[i] && j < 7; i++) {
            if (raw[i] >= '0' && raw[i] <= '9') buf[j++] = raw[i];
        }
    }
    buf[j] = '\0';
    safe_strcpy(out, out_size, buf);
}

static int parse_json_int(const cJSON *node)
{
    if (!node) return 0;
    if (cJSON_IsNumber(node)) return (int)cJSON_GetNumberValue(node);
    const char *s = cJSON_GetStringValue(node);
    return s ? (int)strtol(s, NULL, 10) : 0;
}

static int64_t parse_obs_time(const cJSON *root)
{
    const char *obs_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "obsTime"));
    if (!obs_str) return 0;
    /* QWeather v7 "now" returns obsTime as a string, often ISO 8601
     * without seconds and with timezone offset, e.g. "2026-07-01T14:30+08:00".
     * Parse to unix seconds via struct tm + mktime, then correct for TZ. */
    struct tm tm = {0};
    char sign = '+';
    int tz_h = 0, tz_m = 0;
    int n = sscanf(obs_str, "%d-%d-%dT%d:%d%c%d:%d",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &sign, &tz_h, &tz_m);
    if (n < 5) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1) return 0;
    int tz_off = (tz_h * 3600 + tz_m * 60) * (sign == '-' ? -1 : 1);
    return (int64_t)t - tz_off;
}

int WeatherFetch_IconCategory(const char *icon_code)
{
    int code = 0;
    if (icon_code) sscanf(icon_code, "%d", &code);
    if (code == 100) return 0;                /* sun */
    if (code >= 101 && code <= 104) return 1; /* cloud */
    if (code >= 300 && code <= 399) return 2; /* rain */
    if (code >= 400 && code <= 499) return 3; /* snow */
    if (code >= 500 && code <= 509) return 4; /* fog */
    return 1;                                 /* default cloud */
}

/* ── JSON parsing ── */

static bool parse_now(weather_now_t *out, const cJSON *root)
{
    const cJSON *now = cJSON_GetObjectItem(root, "now");
    if (!cJSON_IsObject(now)) { set_error("now missing"); return false; }

    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(now, "text"));
    const char *icon = cJSON_GetStringValue(cJSON_GetObjectItem(now, "icon"));
    safe_strcpy(out->text, sizeof(out->text), text ? text : "");
    if (icon) parse_icon_code(out->icon, sizeof(out->icon), icon);
    else out->icon[0] = '\0';

    out->temp = parse_json_int(cJSON_GetObjectItem(now, "temp"));
    out->feels_like = parse_json_int(cJSON_GetObjectItem(now, "feelsLike"));
    out->humidity = parse_json_int(cJSON_GetObjectItem(now, "humidity"));
    out->wind_scale = parse_json_int(cJSON_GetObjectItem(now, "windScale"));
    out->obs_time = parse_obs_time(now);

    /* city_name comes from the per-Update config snapshot so it stays on
     * the same epoch as the weather data, even if ReloadConfig rewrites
     * s_city_name mid-fetch. */
    safe_strcpy(out->city_name, sizeof(out->city_name),
                s_fetch_in_progress ? s_fetch_city_name : s_city_name);
    return true;
}

static int parse_hourly(weather_hourly_t *out, size_t max_count, const cJSON *root)
{
    const cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
    if (!cJSON_IsArray(hourly)) { set_error("hourly missing"); return 0; }

    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, hourly) {
        if (count >= (int)max_count) break;
        const char *fx = cJSON_GetStringValue(cJSON_GetObjectItem(item, "fxTime"));
        if (!fx) continue;
        int hour = 0;
        sscanf(fx, "%*[^T]T%2d", &hour);

        const char *icon = cJSON_GetStringValue(cJSON_GetObjectItem(item, "icon"));
        out[count].hour = hour;
        if (icon) parse_icon_code(out[count].icon, sizeof(out[count].icon), icon);
        else out[count].icon[0] = '\0';

        out[count].temp = parse_json_int(cJSON_GetObjectItem(item, "temp"));
        count++;
    }
    return count;
}

static int parse_daily(weather_daily_t *out, size_t max_count, const cJSON *root)
{
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (!cJSON_IsArray(daily)) { set_error("daily missing"); return 0; }

    int count = 0;
    cJSON *item;
    /* Skip index 0 (today) — "now" already covers today. */
    int skip_today = 1;
    cJSON_ArrayForEach(item, daily) {
        if (skip_today > 0) { skip_today--; continue; }
        if (count >= (int)max_count) break;

        const char *date = cJSON_GetStringValue(cJSON_GetObjectItem(item, "fxDate"));
        safe_strcpy(out[count].date, sizeof(out[count].date), date ? date : "");

        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(item, "textDay"));
        safe_strcpy(out[count].text_day, sizeof(out[count].text_day), text ? text : "");

        const char *icon = cJSON_GetStringValue(cJSON_GetObjectItem(item, "iconDay"));
        if (icon) parse_icon_code(out[count].icon_day, sizeof(out[count].icon_day), icon);
        else out[count].icon_day[0] = '\0';

        out[count].temp_min = parse_json_int(cJSON_GetObjectItem(item, "tempMin"));
        out[count].temp_max = parse_json_int(cJSON_GetObjectItem(item, "tempMax"));

        count++;
    }
    return count;
}

/* ── HTTP ── */

/* Skip a gzip member header and return a pointer to the raw DEFLATE stream
 * inside it, plus the offset where the trailing CRC32 (4B) + ISIZE (4B)
 * footer begins. Returns the DEFLATE payload length, or -1 if `in` is not
 * a valid gzip stream.
 *
 * gzip layout (RFC 1952):
 *   +0  magic    0x1f 0x8b
 *   +2  CM       compression method (must be 8 = deflate)
 *   +3  FLG      flags (FEXTRA/FNAME/FCOMMENT/FHCRC)
 *   +4  MTIME    4 bytes (ignored)
 *   +8  XFL      1 byte (ignored)
 *   +9  OS       1 byte (ignored)
 *   +10 ...      optional FEXTRA/FNAME/FCOMMENT/FHCRC, sized by FLG
 *   <DEFLATE payload>
 *   <CRC32  4 bytes>
 *   <ISIZE  4 bytes>  (uncompressed size mod 2^32)
 * miniz's tinfl only speaks raw DEFLATE or zlib streams, NOT gzip, so we
 * strip the wrapper here and feed tinfl the raw payload with no header
 * flag. */
static int gzip_strip_header(const char *in, size_t in_len,
                             const char **deflate_out, size_t *deflate_len)
{
    if (in_len < 18) return -1;  /* 10 header + 4 CRC + 4 ISIZE minimum */
    const uint8_t *p = (const uint8_t *)in;
    if (p[0] != 0x1f || p[1] != 0x8b) return -1;
    if (p[2] != 8) return -1;    /* CM must be deflate */

    uint8_t flg = p[3];
    size_t off = 10;

    /* FEXTRA */
    if (flg & 0x04) {
        if (off + 2 > in_len) return -1;
        size_t xlen = p[off] | (p[off + 1] << 8);
        off += 2 + xlen;
        if (off > in_len) return -1;
    }
    /* FNAME — NUL-terminated */
    if (flg & 0x08) {
        while (off < in_len && p[off] != 0) off++;
        if (off >= in_len) return -1;
        off++;  /* skip the NUL */
    }
    /* FCOMMENT — NUL-terminated */
    if (flg & 0x10) {
        while (off < in_len && p[off] != 0) off++;
        if (off >= in_len) return -1;
        off++;
    }
    /* FHCRC — 2-byte header CRC */
    if (flg & 0x02) {
        if (off + 2 > in_len) return -1;
        off += 2;
    }

    /* Trailing footer is CRC32 (4) + ISIZE (4). The DEFLATE payload is
     * everything between the header and the footer. */
    if (in_len - off < 8) return -1;
    size_t trailer = 8;
    *deflate_out = in + off;
    *deflate_len = in_len - off - trailer;
    return 0;
}

/* Decompress a gzip-wrapped response body into a NUL-terminated buffer.
 * QWeather always returns gzip (ignores Accept-Encoding: identity and the
 * gzip=n query param), so the HTTP client yields gzip bytes that cJSON
 * can't parse directly. miniz's tinfl only speaks raw DEFLATE (or zlib),
 * NOT the gzip wrapper, so we strip the gzip header/footer here and feed
 * tinfl the inner raw DEFLATE stream with no header flag.
 * Returns the decompressed length (excl. NUL) or -1 on failure. */
static int gzip_inflate(const char *in, size_t in_len, char *buf, size_t buf_len)
{
    const char *deflate = NULL;
    size_t deflate_len = 0;
    if (gzip_strip_header(in, in_len, &deflate, &deflate_len) != 0) {
        ESP_LOGW(TAG, "gzip: strip header failed (in=%d)", (int)in_len);
        return -1;
    }

    /* Drive the streaming tinfl_decompress directly. The ROM-provided
     * tinfl_decompress_mem_to_mem / _to_heap wrappers rely on miniz's
     * internal allocator which is compiled out (MINIZ_NO_MALLOC) in the
     * esp_rom build, so they return failure/abort. The raw tinfl_decompress
     * takes a caller-allocated decompressor struct + caller buffers and
     * does no malloc — that path works.
     *
     * Decompressor struct is large (~10KB); put it on the heap (not the
     * data_task stack, which is only 12KB) to avoid overflow. */
    static_assert(sizeof(tinfl_decompressor) > 0, "tinfl_decompressor size");
    tinfl_decompressor *decomp = (tinfl_decompressor *)
        heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_8BIT);
    if (!decomp) return -1;

    tinfl_init(decomp);

    size_t in_pos = 0;
    size_t out_pos = 0;
    tinfl_status st;
    /* TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF: we decompress in one shot
     * into a single output buffer (no ring buffer). Required when
     * out_buf_start == out_buf_next (non-wrapping mode). */
    const mz_uint32 flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;

    do {
        size_t in_remaining = deflate_len - in_pos;
        size_t out_remaining = (buf_len - 1) - out_pos;
        st = tinfl_decompress(decomp,
                              (const mz_uint8 *)deflate + in_pos, &in_remaining,
                              (mz_uint8 *)buf,
                              (mz_uint8 *)buf + out_pos, &out_remaining,
                              flags);
        in_pos += in_remaining;
        out_pos += out_remaining;
    } while (st == TINFL_STATUS_HAS_MORE_OUTPUT);

    free(decomp);

    if (st != TINFL_STATUS_DONE) {
        ESP_LOGW(TAG, "gzip: tinfl status=%d out=%d", (int)st, (int)out_pos);
        return -1;
    }
    if (out_pos == 0 || out_pos >= buf_len) {
        return -1;
    }
    buf[out_pos] = '\0';
    return (int)out_pos;
}

static int http_get_json(const char *path, char *buf, size_t buf_len)
{
    /* Worst case: "https://" (8) + base (≤63) + path (≤127) + NUL = 199. */
    char url[200];
    /* Use the per-Update config snapshot for the host if a fetch is in
     * progress, so all 3 endpoints in one Update target the same host
     * even if ReloadConfig rewrites s_base_url mid-fetch. Fall back to
     * the live static when called outside an Update (defensive). */
    const char *host = s_fetch_in_progress ? s_fetch_base_url : s_base_url;
    int n = snprintf(url, sizeof(url), "https://%s%s", host, path);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        set_error("url too long");
        return -1;
    }

    /* Stage the (possibly gzip) response into a heap buffer, since the
     * decompressed size differs from the wire size and we may need to
     * inflate. 16KB comfortably holds any single QWeather endpoint payload. */
    const size_t STAGE_CAP = 16 * 1024;
    char *stage = (char *)heap_caps_malloc(STAGE_CAP, MALLOC_CAP_8BIT);
    if (!stage) { set_error("stage alloc failed"); return -1; }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        /* buffer_size is the chunk the client reads per underlying transport
         * read; not the total body cap. QWeather now payload is ~1.5KB raw,
         * ~600B gzipped. 1024 is plenty and keeps stack usage down. */
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        free(stage);
        set_error("http client init failed");
        return -1;
    }

    /* Manual open + fetch_headers + read loop. QWeather uses chunked
     * transfer-encoding on top of gzip; esp_http_client_read transparently
     * de-chunks, so we just loop until it returns <= 0.
     *
     * esp_http_client_perform() drains the body into the client's internal
     * buffer and discards it (no event handler registered), which is why
     * we saw len=0 earlier. The manual sequence keeps the body bytes
     * available to our own read calls. */
    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        set_error("http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        free(stage);
        return -1;
    }

    /* fetch_headers reads the status line + headers so get_status_code()
     * returns the real value. It returns the content-length for non-chunked
     * responses (or -1 for chunked) — both are valid, do not treat -1/0
     * as an error. Earlier this call returned ESP_ERR_HTTP_FETCH_HEADER
     * because buffer_size was too small for QWeather's header block;
     * 1024 fixed it. */
    int64_t content_len = esp_http_client_fetch_headers(cli);
    (void)content_len;  /* informational only; we read until stream EOF */

    /* Read the body. esp_http_client_read transparently de-chunks
     * chunked transfer-encoding, so we just loop until EOF/timeout. */
    int total = 0;
    while (total < (int)STAGE_CAP - 1) {
        int n = esp_http_client_read(cli, stage + total, STAGE_CAP - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    stage[total] = '\0';

    int status = esp_http_client_get_status_code(cli);
    /* Detect gzip: QWeather always returns gzip on success, but 4xx/5xx
     * errors come back as plain JSON. Trust the magic bytes (0x1f 0x8b)
     * over the Content-Encoding header, which esp_http_client may not
     * expose reliably — the magic bytes are unambiguous. */
    bool gzipped = (total >= 2 &&
                    (uint8_t)stage[0] == 0x1f &&
                    (uint8_t)stage[1] == 0x8b);

    if (status != 200 && !gzipped) {
        /* Surface the real status + a body snippet (plain JSON on 4xx/5xx)
         * so 403/429/etc. are diagnosable instead of "HTTP 0". */
        char snippet[64] = {0};
        if (total > 0) {
            strncpy(snippet, stage, sizeof(snippet) - 1);
            snippet[sizeof(snippet) - 1] = '\0';
            for (char *p = snippet; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        }
        set_error("HTTP %d %s", status, snippet);
        esp_http_client_cleanup(cli);
        free(stage);
        return -1;
    }

    esp_http_client_cleanup(cli);  /* MUST cleanup to release TLS context */

    if (status != 200) {
        /* gzip error body — can't snippet it, just record the status. */
        set_error("HTTP %d", status);
        free(stage);
        return -1;
    }

    int out_len;
    if (gzipped) {
        out_len = gzip_inflate(stage, (size_t)total, buf, buf_len);
        free(stage);
        if (out_len < 0) { set_error("gzip inflate failed"); return -1; }
    } else {
        if ((size_t)total >= buf_len) {
            free(stage);
            set_error("payload too large");
            return -1;
        }
        memcpy(buf, stage, total);
        buf[total] = '\0';
        out_len = total;
        free(stage);
    }
    return out_len;
}

/* Fetch one endpoint and parse with the given parser. */
static bool fetch_and_parse_now(const char *path, weather_now_t *out)
{
    /* 8KB: QWeather now payload decompresses to ~1-2KB, but give headroom
     * so gzip_inflate never truncates. Allocated on the 12KB data_task stack. */
    char buf[8192];
    if (http_get_json(path, buf, sizeof(buf)) < 0) return false;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { set_error("now JSON parse failed"); return false; }

    bool ok = parse_now(out, root);
    cJSON_Delete(root);
    return ok;
}

static bool fetch_and_parse_hourly(const char *path, weather_hourly_t *out, size_t max_count)
{
    char buf[8192];
    if (http_get_json(path, buf, sizeof(buf)) < 0) return false;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { set_error("24h JSON parse failed"); return false; }

    parse_hourly(out, max_count, root);
    cJSON_Delete(root);
    return true;
}

static bool fetch_and_parse_daily(const char *path, weather_daily_t *out, size_t max_count)
{
    char buf[8192];
    if (http_get_json(path, buf, sizeof(buf)) < 0) return false;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { set_error("3d JSON parse failed"); return false; }

    parse_daily(out, max_count, root);
    cJSON_Delete(root);
    return true;
}

/* ── Public API ── */

esp_err_t WeatherFetch_Init(void)
{
    if (s_initialized) return ESP_OK;

    /* Kconfig defaults */
    safe_strcpy(s_api_key,     sizeof(s_api_key),     CONFIG_WEATHER_API_KEY);
    safe_strcpy(s_location_id, sizeof(s_location_id), CONFIG_WEATHER_LOCATION_ID);
    safe_strcpy(s_city_name,   sizeof(s_city_name),   CONFIG_WEATHER_CITY_NAME);
    safe_strcpy(s_base_url,    sizeof(s_base_url),    CONFIG_WEATHER_API_BASE_URL);

    /* NVS overrides (written by Web config API). Non-fatal if missing. */
    nvs_handle_t handle;
    esp_err_t err = nvs_open("weather", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t len = sizeof(s_api_key);
        nvs_get_str(handle, "api_key", s_api_key, &len);
        len = sizeof(s_location_id);
        nvs_get_str(handle, "location_id", s_location_id, &len);
        len = sizeof(s_city_name);
        nvs_get_str(handle, "city_name", s_city_name, &len);
        /* base_url override: QWeather rotated per-developer API hosts in
         * 2026, so operators must be able to change the host from the web
         * UI without reflashing. Falls back to the Kconfig default if the
         * key is absent. */
        len = sizeof(s_base_url);
        nvs_get_str(handle, "base_url", s_base_url, &len);
        nvs_close(handle);
    }
    /* NVS_READONLY open failing with ESP_ERR_NVS_NOT_FOUND just means no
     * override yet — not an init failure. Other errors are logged but we
     * still proceed on Kconfig defaults. */

    s_initialized = true;
    ESP_LOGI(TAG, "init: base=%s city=%s ready=%s",
             s_base_url, s_city_name,
             WeatherFetch_IsReady() ? "yes" : "no");
    return ESP_OK;
}

esp_err_t WeatherFetch_ReloadConfig(void)
{
    /* Re-read NVS overrides into the live static buffers so a web-config
     * change takes effect immediately. Idempotent and safe to call before
     * Init (Init is what sets s_initialized).
     *
     * We stage the NVS reads into locals, then publish all four buffers
     * into the live statics under s_config_spinlock in one shot. This
     * avoids a torn read in WeatherFetch_Update (data_task), which could
     * otherwise build a request URL from a half-overwritten key/location
     * while this function is mid-write from the HTTP server task. */
    char api_key[WEATHER_API_KEY_LEN];
    char location_id[WEATHER_LOCATION_ID_LEN];
    char city_name[WEATHER_CITY_NAME_LEN];
    char base_url[64];
    /* Seed locals from current live values so an absent NVS key leaves the
     * effective value unchanged (nvs_get_str returns non-OK and we keep
     * the prior value) rather than an uninitialized buffer. */
    portENTER_CRITICAL(&s_config_spinlock);
    memcpy(api_key,     s_api_key,     sizeof(api_key));
    memcpy(location_id, s_location_id, sizeof(location_id));
    memcpy(city_name,   s_city_name,   sizeof(city_name));
    memcpy(base_url,    s_base_url,    sizeof(base_url));
    portEXIT_CRITICAL(&s_config_spinlock);

    nvs_handle_t handle;
    esp_err_t err = nvs_open("weather", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len;
    len = sizeof(api_key);     nvs_get_str(handle, "api_key",     api_key,     &len);
    len = sizeof(location_id); nvs_get_str(handle, "location_id", location_id, &len);
    len = sizeof(city_name);   nvs_get_str(handle, "city_name",   city_name,   &len);
    len = sizeof(base_url);    nvs_get_str(handle, "base_url",    base_url,    &len);
    nvs_close(handle);

    /* Publish the staged values atomically vs the data_task reader. */
    portENTER_CRITICAL(&s_config_spinlock);
    memcpy(s_api_key,     api_key,     sizeof(s_api_key));
    memcpy(s_location_id, location_id, sizeof(s_location_id));
    memcpy(s_city_name,   city_name,   sizeof(s_city_name));
    memcpy(s_base_url,    base_url,    sizeof(s_base_url));
    portEXIT_CRITICAL(&s_config_spinlock);

    ESP_LOGI(TAG, "config reloaded: base=%s city=%s ready=%s",
             s_base_url, s_city_name,
             WeatherFetch_IsReady() ? "yes" : "no");
    return ESP_OK;
}

bool WeatherFetch_IsReady(void)
{
    return s_initialized
        && s_api_key[0] != '\0'
        && s_location_id[0] != '\0'
        && s_city_name[0] != '\0';
}

bool WeatherFetch_Update(weather_snapshot_t *snapshot)
{
    if (!snapshot || !WeatherFetch_IsReady()) {
        set_error("not ready");
        return false;
    }
    if (!WifiBsp_IsConnected()) { set_error("WiFi not connected"); return false; }

    /* Heap guard: mbedTLS SSL context needs ~40KB of contiguous internal RAM
     * at ssl_setup time. If internal heap is too low, TLS fails with
     * -0x7F00 (MBEDTLS_ERR_SSL_ALLOC_FAILED). */
    uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (free_internal < 40000) {
        uint32_t free_total = esp_get_free_heap_size();
        ESP_LOGW(TAG, "heap low: internal=%u total=%u — TLS may fail",
                 (unsigned)free_internal, (unsigned)free_total);
        set_error("internal heap too low (%u)", (unsigned)free_internal);
        return false;
    }

    /* Stage the request strings into locals first, then build the URL
     * from a consistent snapshot taken under s_config_spinlock. This
     * prevents a mid-buffer tear if WeatherFetch_ReloadConfig is updating
     * the config from the HTTP task on the other core. We snapshot ALL
     * four config buffers that Update touches: api_key + location_id go
     * into the per-request URL path, base_url is read by http_get_json
     * to build the host, and city_name is copied by parse_now into the
     * snapshot. Snapshotting base_url and city_name here and threading
     * them through (via globals below) keeps the whole multi-second,
     * 3-endpoint fetch using one consistent config epoch. */
    char api_key[WEATHER_API_KEY_LEN];
    char location_id[WEATHER_LOCATION_ID_LEN];
    char base_url[64];
    char city_name[WEATHER_CITY_NAME_LEN];

    portENTER_CRITICAL(&s_config_spinlock);
    memcpy(api_key,     s_api_key,     sizeof(api_key));
    memcpy(location_id, s_location_id, sizeof(location_id));
    memcpy(base_url,    s_base_url,    sizeof(base_url));
    memcpy(city_name,   s_city_name,   sizeof(city_name));
    portEXIT_CRITICAL(&s_config_spinlock);

    /* Publish the snapshot into the per-Update fetch-scoped globals so
     * http_get_json (base_url) and parse_now (city_name) read the same
     * epoch we just snapshotted, not the live statics a concurrent
     * ReloadConfig might be overwriting. Set before any fetch; cleared
     * after. These are only read while s_fetch_in_progress is true and
     * Update is the sole writer — no extra lock needed around them. */
    portENTER_CRITICAL(&s_config_spinlock);
    memcpy(s_fetch_base_url,  base_url,  sizeof(s_fetch_base_url));
    memcpy(s_fetch_city_name, city_name, sizeof(s_fetch_city_name));
    s_fetch_in_progress = true;
    portEXIT_CRITICAL(&s_config_spinlock);

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = WEATHER_CACHE_VERSION;
    bool ok = true;

    char path[128];

    /* 1. now */
    snprintf(path, sizeof(path), "/v7/weather/now?location=%s&key=%s",
             location_id, api_key);
    if (!fetch_and_parse_now(path, &snapshot->now)) {
        ESP_LOGW(TAG, "fetch now failed: %s", s_last_error);
        ok = false; goto fail;
    }

    /* 2. 24h hourly */
    snprintf(path, sizeof(path), "/v7/weather/24h?location=%s&key=%s",
             location_id, api_key);
    if (!fetch_and_parse_hourly(path, snapshot->hourly, WEATHER_HOURLY_COUNT)) {
        ESP_LOGW(TAG, "fetch 24h failed: %s", s_last_error);
        ok = false; goto fail;
    }

    /* 3. 3d daily */
    snprintf(path, sizeof(path), "/v7/weather/3d?location=%s&key=%s",
             location_id, api_key);
    if (!fetch_and_parse_daily(path, snapshot->daily, WEATHER_DAILY_COUNT)) {
        ESP_LOGW(TAG, "fetch 3d failed: %s", s_last_error);
        ok = false; goto fail;
    }

    snapshot->fetch_time = (int64_t)time(NULL);

fail:
    /* Clear the fetch-in-progress flag on every exit path (success or any
     * early return above) so http_get_json/parse_now fall back to the live
     * statics when not mid-Update. */
    portENTER_CRITICAL(&s_config_spinlock);
    s_fetch_in_progress = false;
    portEXIT_CRITICAL(&s_config_spinlock);
    return ok;
}

const char *WeatherFetch_LastError(void)
{
    return s_last_error;
}

void WeatherFetch_RequestRefresh(void)
{
    /* portENTER_CRITICAL gives the dual-core-safe atomicity that a bare
     * volatile write lacks (taskENTER_CRITICAL maps to this on Xtensa MP). */
    portENTER_CRITICAL(&s_refresh_spinlock);
    s_refresh_requested = true;
    portEXIT_CRITICAL(&s_refresh_spinlock);
}

bool WeatherFetch_ConsumeRefreshRequest(void)
{
    bool was_set;
    portENTER_CRITICAL(&s_refresh_spinlock);
    was_set = s_refresh_requested;
    s_refresh_requested = false;
    portEXIT_CRITICAL(&s_refresh_spinlock);
    return was_set;
}
