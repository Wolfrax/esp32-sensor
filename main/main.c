#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "nvs.h"
#include "mdns.h"
#include "i2c_reader.h"


/* ---------------- CONFIG ---------------- */

#define STORAGE_PATH "/storage"
#define MAX_SAMPLES 128
#define SIMULATE_SENSOR 0
#define LOG_WEB_BUF_SIZE 8192
#define LOG_WEB_TMP_LINE 256

/* ---------------- GLOBALS ---------------- */

static const char *TAG = "webserver";
static wl_handle_t wl_handle_storage;
static char current_ssid[32] = "UNKNOWN";
static char ip_str[16] = "UNKNOWN";
static char hostname[32] = "UNKNOWN";
static httpd_handle_t server = NULL;
static char starttime_str[64] = "UNKNOWN";
static char log_web_buf[LOG_WEB_BUF_SIZE];
static size_t log_web_head = 0;
static bool log_web_wrapped = false;
static SemaphoreHandle_t log_web_mutex = NULL;
static vprintf_like_t log_old_vprintf = NULL;

/* ---------------- SAMPLE STRUCT ---------------- */

typedef struct {
    char ts[32];
    float t, h, p;
} sample_t;

/* ---------------- SSE ---------------- */

#define MAX_SSE_CLIENTS 4
#define SSE_KEEPALIVE_SEC 20

typedef struct {
    bool active;
    int sockfd;
    httpd_req_t *req_async;
    uint32_t last_sent_seq;
} sse_client_t;

typedef struct {
    int client_index;
    bool is_keepalive;
    sample_t sample;
    uint32_t seq;
} sse_work_t;

static sse_client_t sse_clients[MAX_SSE_CLIENTS];
static SemaphoreHandle_t sse_clients_mutex = NULL;

static sample_t latest_sample = {0};
static uint32_t latest_sample_seq = 0;
static SemaphoreHandle_t sample_mutex = NULL;

/* ---------------- WEB LOG BUFFER ---------------- */

static void log_web_append(const char *s, size_t len)
{
    if (len == 0) return;

    // If one message is larger than the whole buffer, keep only the tail
    if (len >= LOG_WEB_BUF_SIZE) {
        s += (len - (LOG_WEB_BUF_SIZE - 1));
        len = LOG_WEB_BUF_SIZE - 1;
    }

    for (size_t i = 0; i < len; i++) {
        log_web_buf[log_web_head] = s[i];
        log_web_head = (log_web_head + 1) % LOG_WEB_BUF_SIZE;

        if (log_web_head == 0) {
            log_web_wrapped = true;
        }
    }
}

static int log_web_vprintf(const char *fmt, va_list ap)
{
    char tmp[LOG_WEB_TMP_LINE];

    // Format once for the web buffer
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap_copy);
    va_end(ap_copy);

    if (n > 0 && log_web_mutex != NULL) {
        size_t len = (n < (int)sizeof(tmp)) ? (size_t)n : (sizeof(tmp) - 1);

        if (xSemaphoreTake(log_web_mutex, 0) == pdTRUE) {
            log_web_append(tmp, len);
            xSemaphoreGive(log_web_mutex);
        }
    }

    // Still send logs to the normal output (monitor/UART)
    if (log_old_vprintf) {
        return log_old_vprintf(fmt, ap);
    }

    return n;
}

static void init_web_log_buffer(void)
{
    log_web_mutex = xSemaphoreCreateMutex();
    assert(log_web_mutex != NULL);

    log_old_vprintf = esp_log_set_vprintf(log_web_vprintf);
}

/* ---------------- SSE ---------------- */

static int sse_find_free_slot_locked(void)
{
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (!sse_clients[i].active) {
            return i;
        }
    }
    return -1;
}

static void sse_remove_client_locked(int idx)
{
    if (idx < 0 || idx >= MAX_SSE_CLIENTS || !sse_clients[idx].active) {
        return;
    }

    httpd_req_t *req_async = sse_clients[idx].req_async;
    int sockfd = sse_clients[idx].sockfd;

    sse_clients[idx].active = false;
    sse_clients[idx].sockfd = -1;
    sse_clients[idx].req_async = NULL;
    sse_clients[idx].last_sent_seq = 0;

    if (server != NULL && sockfd >= 0) {
        httpd_sess_trigger_close(server, sockfd);
    }

    if (req_async != NULL) {
        httpd_req_async_handler_complete(req_async);
    }
}

static void sse_send_work(void *arg)
{
    sse_work_t *work = (sse_work_t *)arg;
    if (work == NULL) {
        return;
    }

    if (xSemaphoreTake(sse_clients_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(work);
        return;
    }

    int idx = work->client_index;
    if (idx < 0 || idx >= MAX_SSE_CLIENTS || !sse_clients[idx].active) {
        xSemaphoreGive(sse_clients_mutex);
        free(work);
        return;
    }

    httpd_req_t *req = sse_clients[idx].req_async;
    esp_err_t err = ESP_FAIL;

    if (req == NULL) {
        sse_remove_client_locked(idx);
        xSemaphoreGive(sse_clients_mutex);
        free(work);
        return;
    }

    if (work->is_keepalive) {
        err = httpd_resp_send_chunk(req, ": keepalive\n\n", HTTPD_RESP_USE_STRLEN);
    } else {
        char buf[192];
        int len = snprintf(buf, sizeof(buf),
                           "event: sample\n"
                           "data: {\"ts\":\"%s\",\"t\":%.2f,\"h\":%.2f,\"p\":%.2f}\n\n",
                           work->sample.ts, work->sample.t, work->sample.h, work->sample.p);

        if (len > 0 && len < (int)sizeof(buf)) {
            err = httpd_resp_send_chunk(req, buf, len);
        } else {
            err = ESP_FAIL;
        }

        if (err == ESP_OK) {
            sse_clients[idx].last_sent_seq = work->seq;
        }
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSE send failed for client %d, removing", idx);
        sse_remove_client_locked(idx);
    }

    xSemaphoreGive(sse_clients_mutex);
    free(work);
}

static void sse_broadcast_task(void *arg)
{
    uint32_t last_global_seq_seen = 0;
    int keepalive_counter = 0;

    while (1) {
        sample_t s = {0};
        uint32_t seq = 0;

        if (xSemaphoreTake(sample_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            seq = latest_sample_seq;
            s = latest_sample;
            xSemaphoreGive(sample_mutex);
        }

        bool have_new_sample = (seq != 0 && seq != last_global_seq_seen);

        keepalive_counter++;
        bool do_keepalive = (keepalive_counter >= SSE_KEEPALIVE_SEC);
        if (do_keepalive) {
            keepalive_counter = 0;
        }

        if (have_new_sample || do_keepalive) {
            if (xSemaphoreTake(sse_clients_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
                    if (!sse_clients[i].active) {
                        continue;
                    }

                    if (have_new_sample && sse_clients[i].last_sent_seq == seq) {
                        continue;
                    }

                    sse_work_t *work = calloc(1, sizeof(sse_work_t));
                    if (work == NULL) {
                        continue;
                    }

                    work->client_index = i;
                    work->is_keepalive = !have_new_sample;
                    work->sample = s;
                    work->seq = seq;

                    esp_err_t err = httpd_queue_work(server, sse_send_work, work);
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "httpd_queue_work failed for client %d", i);
                        free(work);
                    }
                }
                xSemaphoreGive(sse_clients_mutex);
            }
        }

        if (have_new_sample) {
            last_global_seq_seen = seq;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t events_handler(httpd_req_t *req)
{
    if (sse_clients_mutex == NULL || sample_mutex == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SSE not initialized");
        return ESP_FAIL;
    }

    // Check capacity before starting async handling
    if (xSemaphoreTake(sse_clients_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "SSE busy", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    int free_idx = sse_find_free_slot_locked();
    xSemaphoreGive(sse_clients_mutex);

    if (free_idx < 0) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Too many SSE clients", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    httpd_req_t *async_req = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK || async_req == NULL) {
        ESP_LOGE(TAG, "httpd_req_async_handler_begin failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    httpd_resp_set_type(async_req, "text/event-stream");
    httpd_resp_set_hdr(async_req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(async_req, "Connection", "keep-alive");
    httpd_resp_set_hdr(async_req, "Access-Control-Allow-Origin", "*");

    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        httpd_req_async_handler_complete(async_req);
        return ESP_OK;
    }

    if (xSemaphoreTake(sse_clients_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_req_async_handler_complete(async_req);
        return ESP_OK;
    }

    int idx = sse_find_free_slot_locked();
    if (idx < 0) {
        xSemaphoreGive(sse_clients_mutex);
        httpd_req_async_handler_complete(async_req);
        return ESP_OK;
    }

    sse_clients[idx].active = true;
    sse_clients[idx].sockfd = sockfd;
    sse_clients[idx].req_async = async_req;
    sse_clients[idx].last_sent_seq = 0;

    xSemaphoreGive(sse_clients_mutex);

    // Send initial comment so headers/stream are established immediately
    err = httpd_resp_send_chunk(async_req, ": connected\n\n", HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        if (xSemaphoreTake(sse_clients_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            sse_remove_client_locked(idx);
            xSemaphoreGive(sse_clients_mutex);
        }
        return ESP_OK;
    }

    ESP_LOGI(TAG, "SSE client connected, slot=%d sockfd=%d", idx, sockfd);
    return ESP_OK;
}

/* ---------------- WIFI EVENT HANDLER ---------------- */

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;

        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", ip_str);
    }
}

/* ---------------- WIFI ---------------- */

static void wifi_init_base(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Set hostname to "esp32-XXXXXX" where XXXXXX are the last 3 bytes of the MAC address
    // which will be used in DHCP and mDNS responses 
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    snprintf(hostname, sizeof(hostname), "esp32-%02X%02X%02X", mac[3], mac[4], mac[5]);
    
    // Set the hostname for DHCP, visible in the router's client list
    esp_netif_set_hostname(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), hostname);

    // Initialize mDNS with the device's hostname to make it easier to find the device on the network. 
    mdns_init();

    mdns_hostname_set(hostname);
    mdns_instance_name_set("ESP32 Logger"); // This is the name that will show up in mDNS browser apps

    // Advertise an HTTP service on port 80
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

static void wifi_start_sta(const char *ssid, const char *pass)
{
    strncpy(current_ssid, ssid, sizeof(current_ssid) - 1);
    current_ssid[sizeof(current_ssid) - 1] = '\0';

    wifi_config_t wifi_config = { 0 };
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", ssid);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", pass);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);
}

static void wifi_start_ap(void)
{
    strcpy(current_ssid, "ESP32-Setup");

    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32-Setup",
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void save_wifi_config(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    nvs_open("wifi", NVS_READWRITE, &nvs);
    nvs_set_str(nvs, "ssid", ssid);
    nvs_set_str(nvs, "pass", pass);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static int hex2int(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    char *out = dst;
    const char *end = dst + dst_size - 1;

    while (*src && out < end) {
        if (*src == '+') {
            *out++ = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            int hi = hex2int(src[1]);
            int lo = hex2int(src[2]);
            *out++ = (char)((hi << 4) | lo);
            src += 3;
        } else {
            *out++ = *src++;
        }
    }

    *out = '\0';
}

static esp_err_t wifi_setup_handler(httpd_req_t *req)
{
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    char ssid[32] = {0};
    char pass[64] = {0};

    if (httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid)) != ESP_OK ||
        httpd_query_key_value(buf, "pass", pass, sizeof(pass)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid or pass");
        return ESP_FAIL;
    }

    char decoded_ssid[sizeof(ssid)];
    url_decode(decoded_ssid, ssid, sizeof(decoded_ssid));
    strlcpy(ssid, decoded_ssid, sizeof(ssid));

    char decoded_pass[sizeof(pass)];
    url_decode(decoded_pass, pass, sizeof(decoded_pass));
    strlcpy(pass, decoded_pass, sizeof(pass));

    ESP_LOGI(TAG, "Decoded WiFi config: ssid=%s pass=%s", ssid, pass);

    save_wifi_config(ssid, pass);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"WiFi config saved, rebooting\"}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

static esp_err_t wifi_reset_handler(httpd_req_t *req)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS open failed");
        return ESP_FAIL;
    }

    nvs_erase_key(nvs, "ssid");
    nvs_erase_key(nvs, "pass");
    nvs_commit(nvs);
    nvs_close(nvs);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"WiFi config erased, rebooting\"}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

static bool load_wifi_config(char *ssid, size_t ssid_len,
                            char *pass, size_t pass_len)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK)
        return false;

    if (nvs_get_str(nvs, "ssid", ssid, &ssid_len) != ESP_OK ||
        nvs_get_str(nvs, "pass", pass, &pass_len) != ESP_OK) {
        nvs_close(nvs);
        return false;
    }

    nvs_close(nvs);
    return true;
}

/* ---------------- SENSOR ---------------- */

#if SIMULATE_SENSOR
static float get_temperature() { return 20 + (esp_random() % 100) / 10.0; }
static float get_humidity()    { return 40 + (esp_random() % 200) / 10.0; }
static float get_pressure()    { return 1000 + (esp_random() % 100); }
#endif

/* ---------------- FILE ---------------- */

static void append_to_file(const char *filename, sample_t *s)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/data/%s", STORAGE_PATH, filename);

    FILE *f = fopen(path, "a");
    if (!f) return;

    fprintf(f, "%s,%.2f,%.2f,%.2f\n", s->ts, s->t, s->h, s->p);
    fclose(f);
}

static void cleanup_files(void)
{
    time_t now;
    time(&now);

    DIR *dir = opendir(STORAGE_PATH "/data");
    if (!dir) return;

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;

        char path[256];
        if (snprintf(path, sizeof(path), "%s/data/%s", STORAGE_PATH, entry->d_name) >= sizeof(path)) {
            ESP_LOGW(TAG, "Path truncated, skipping file");
            continue;
        }

        struct stat st;
        if (stat(path, &st) == 0) {
            if ((now - st.st_mtime) > 7 * 24 * 3600) {
                unlink(path);
            }
        }
    }
    closedir(dir);
}

/* ---------------- SAMPLING ---------------- */

static void sampling_task(void *arg)
{
    esp_err_t err;

    while (1) {
        sample_t s;

        time_t now;
        time(&now);

        struct tm tinfo;
        gmtime_r(&now, &tinfo);

        strftime(s.ts, sizeof(s.ts), "%Y-%m-%dT%H:%M:%SZ", &tinfo);

#if SIMULATE_SENSOR
        s.t = get_temperature();
        s.h = get_humidity();
        s.p = get_pressure();
#else
        err = i2c_reader_read(&s.t, &s.h, &s.p);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Read failed: %s", esp_err_to_name(err));
        }
#endif

        char filename[32];
        strftime(filename, sizeof(filename), "%Y-%m-%d.csv", &tinfo);

        append_to_file(filename, &s);
        cleanup_files();

        if (xSemaphoreTake(sample_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            latest_sample = s;
            latest_sample_seq++;
            xSemaphoreGive(sample_mutex);
        }      

        vTaskDelay(pdMS_TO_TICKS(10*60000));  // Sample every 10 minutes
    }
}

/* ---------------- FILE SERVING ---------------- */

#define TEMPLATE_BUF_SIZE 256
#define TITLE_MAX_LEN 64
#define DEFAULT_TITLE "ESP32 Sensor"

static void html_escape(char *dst, size_t dst_size, const char *src)
{
    char *out = dst;
    size_t left = dst_size;

    #define APPEND(s) do { \
        int n = snprintf(out, left, "%s", (s)); \
        if (n < 0 || (size_t)n >= left) goto done; \
        out += n; \
        left -= n; \
    } while (0)

    while (*src && left > 1) {
        switch (*src) {
            case '&': APPEND("&amp;"); break;
            case '<': APPEND("&lt;"); break;
            case '>': APPEND("&gt;"); break;
            case '"': APPEND("&quot;"); break;
            case '\'': APPEND("&#39;"); break;
            default:
                *out++ = *src;
                left--;
                break;
        }
        src++;
    }

done:
    *out = '\0';
    #undef APPEND
}

static esp_err_t serve_file_with_title(
    httpd_req_t *req,
    const char *path,
    const char *title
) {
    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    const char *marker = "{{TITLE}}";
    const size_t marker_len = strlen(marker);

    char buf[TEMPLATE_BUF_SIZE];
    char pending[sizeof("{{TITLE}}") - 1];  // marker_len - 1 chars max
    size_t pending_len = 0;

    size_t n;

    httpd_resp_set_type(req, "text/html; charset=utf-8");

    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        char work[TEMPLATE_BUF_SIZE + sizeof("{{TITLE}}")];
        size_t work_len = 0;

        if (pending_len > 0) {
            memcpy(work, pending, pending_len);
            work_len += pending_len;
            pending_len = 0;
        }

        memcpy(work + work_len, buf, n);
        work_len += n;

        size_t pos = 0;

        while (pos < work_len) {
            char *found = NULL;

            for (size_t i = pos; i + marker_len <= work_len; i++) {
                if (memcmp(&work[i], marker, marker_len) == 0) {
                    found = &work[i];
                    break;
                }
            }

            if (found) {
                size_t before_len = found - &work[pos];

                if (before_len > 0) {
                    esp_err_t err = httpd_resp_send_chunk(req, &work[pos], before_len);
                    if (err != ESP_OK) {
                        fclose(f);
                        return err;
                    }
                }

                if (title != NULL && title[0] != '\0') {
                    esp_err_t err = httpd_resp_send_chunk(req, title, HTTPD_RESP_USE_STRLEN);
                    if (err != ESP_OK) {
                        fclose(f);
                        return err;
                    }
                }

                pos = (found - work) + marker_len;               
            } else {
                break;
            }
        }

        /*
         * Keep up to marker_len - 1 trailing chars, in case "{{TITLE}}"
         * is split across two fread() calls.
         */
        size_t remaining = work_len - pos;

        if (remaining >= marker_len) {
            size_t send_len = remaining - (marker_len - 1);

            ESP_ERROR_CHECK_WITHOUT_ABORT(
                httpd_resp_send_chunk(req, &work[pos], send_len)
            );

            pos += send_len;
            remaining = work_len - pos;
        }

        if (remaining > 0) {
            memcpy(pending, &work[pos], remaining);
            pending_len = remaining;
        }
    }

    fclose(f);

    if (pending_len > 0) {
        httpd_resp_send_chunk(req, pending, pending_len);
    }

    return httpd_resp_send_chunk(req, NULL, 0);
}

static void get_request_title(httpd_req_t *req, char *out, size_t out_size)
{
    char query[128];
    char raw_title[TITLE_MAX_LEN] = "";
    char decoded_title[TITLE_MAX_LEN] = "";

    strlcpy(out, DEFAULT_TITLE, out_size);   // default

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "title", raw_title, sizeof(raw_title)) == ESP_OK &&
            raw_title[0] != '\0') {

            url_decode(decoded_title, raw_title, sizeof(decoded_title));
            html_escape(out, out_size, decoded_title);
        }
    }
}

static esp_err_t serve_file(httpd_req_t *req, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char buffer[256];
    size_t len;

    while ((len = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        httpd_resp_send_chunk(req, buffer, len);
    }

    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---------------- ROOT ---------------- */

static esp_err_t root_handler(httpd_req_t *req)
{
    char title[TITLE_MAX_LEN * 6] = "";

    get_request_title(req, title, sizeof(title));

    return serve_file_with_title(req, "/www/index.html", title);
}

/* ---------------- LIST FILES ---------------- */

static esp_err_t list_files_handler(httpd_req_t *req)
{
    DIR *dir = opendir(STORAGE_PATH "/data");
    if (!dir) return ESP_FAIL;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"files\":[");

    struct dirent *entry;
    bool first = true;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;

        if (!first) httpd_resp_sendstr_chunk(req, ",");
        first = false;

        httpd_resp_sendstr_chunk(req, "\"");
        httpd_resp_sendstr_chunk(req, entry->d_name);
        httpd_resp_sendstr_chunk(req, "\"");
    }

    closedir(dir);
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);

    return ESP_OK;
}

/* ---------------- DATA ---------------- */

static esp_err_t data_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{ \"data\": [");

    bool first = true;

    DIR *dir = opendir(STORAGE_PATH "/data");
    if (!dir) {
        httpd_resp_sendstr_chunk(req, "] }");
        httpd_resp_sendstr_chunk(req, NULL);
        ESP_LOGE(TAG, "Failed to open data directory: %s", strerror(errno));
        return ESP_FAIL;
    }

    struct dirent *entry;
    char path[256];

    while ((entry = readdir(dir)) != NULL) {

        // Only process .csv files
        if (!strstr(entry->d_name, ".csv")) continue;

        int written = snprintf(path, sizeof(path),
                               "%s/data/%s",
                               STORAGE_PATH,
                               entry->d_name);

        if (written < 0 || written >= sizeof(path)) continue;

        FILE *f = fopen(path, "r");
        if (!f) continue;

        char line[128];

        while (fgets(line, sizeof(line), f)) {

            char ts[32];
            float t, h, p;

            // Parse CSV: timestamp,temp,hum,press
            if (sscanf(line, "%31[^,],%f,%f,%f", ts, &t, &h, &p) == 4) {

                char json[160];

                int len = snprintf(json, sizeof(json),
                    "%s{\"ts\":\"%s\",\"t\":%.2f,\"h\":%.2f,\"p\":%.2f}",
                    first ? "" : ",",
                    ts, t, h, p);

                if (len > 0 && len < sizeof(json)) {
                    httpd_resp_sendstr_chunk(req, json);
                    first = false;
                }
            }
        }

        fclose(f);
    }

    closedir(dir);

    httpd_resp_sendstr_chunk(req, "] }");
    httpd_resp_sendstr_chunk(req, NULL);

    return ESP_OK;
}

/* ---------------- GET FILE ---------------- */

static esp_err_t get_file_handler(httpd_req_t *req)
{
    char query[128];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No query");
        return ESP_FAIL;
    }

    char filename[64];

    if (httpd_query_key_value(query, "filename", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing param");
        return ESP_FAIL;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/data/%s", STORAGE_PATH, filename);

    return serve_file(req, path);
}

/* ---------------- GET NODE ---------------- */

static esp_err_t get_node_handler(httpd_req_t *req)
{
    // Return starttime, mac, ip_str, node name, and current SSID as JSON
    // Example response: { "node": { "starttime": "<starttime>", "mac": "<mac_address>", "ip": "<ip_address>", "name": "<node_name>", "ssid": "<ssid>" } }

    httpd_resp_set_type(req, "application/json");

    httpd_resp_sendstr_chunk(req, "{ \"node\": { \"starttime\": \"");
    httpd_resp_sendstr_chunk(req, starttime_str);
    
    httpd_resp_sendstr_chunk(req, "\", \"mac\": \"");
    uint8_t mac[6];
    char mac_str[32];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_str, sizeof(mac_str),"%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    httpd_resp_sendstr_chunk(req, mac_str);

    httpd_resp_sendstr_chunk(req, "\", \"ip\": \"");
    httpd_resp_sendstr_chunk(req, ip_str);
    httpd_resp_sendstr_chunk(req, "\", \"name\": \"");
    httpd_resp_sendstr_chunk(req, hostname);
    httpd_resp_sendstr_chunk(req, "\", \"ssid\": \"");
    httpd_resp_sendstr_chunk(req, current_ssid);
    httpd_resp_sendstr_chunk(req, "\" } }");

    httpd_resp_sendstr_chunk(req, NULL);

    return ESP_OK;
}

/* ---------------- LOGS ---------------- */

static esp_err_t logs_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (log_web_mutex == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Log buffer not initialized");
        return ESP_FAIL;
    }

    if (xSemaphoreTake(log_web_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Log buffer busy");
        return ESP_FAIL;
    }

    if (!log_web_wrapped) {
        esp_err_t err = httpd_resp_send(req, log_web_buf, log_web_head);
        xSemaphoreGive(log_web_mutex);
        return err;
    }

    esp_err_t err = httpd_resp_send_chunk(req,
                                          &log_web_buf[log_web_head],
                                          LOG_WEB_BUF_SIZE - log_web_head);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, &log_web_buf[0], log_web_head);
    }

    xSemaphoreGive(log_web_mutex);

    if (err != ESP_OK) {
        return err;
    }

    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t debug_handler(httpd_req_t *req)
{
    char title[TITLE_MAX_LEN * 6] = "";

    get_request_title(req, title, sizeof(title));

    return serve_file_with_title(req, "/www/debug.html", title);
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return serve_file(req, "/www/favicon.png");
}

/* ---------------- SERVER ---------------- */

static void start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 11;  // We have 11 handlers
    config.max_open_sockets = 10;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;

    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t list = { .uri = "/list_files", .method = HTTP_GET, .handler = list_files_handler };
    httpd_uri_t data = { .uri = "/data", .method = HTTP_GET, .handler = data_handler };
    httpd_uri_t getf = { .uri = "/get_file", .method = HTTP_GET, .handler = get_file_handler };
    httpd_uri_t node = { .uri = "/get_node", .method = HTTP_GET, .handler = get_node_handler };
    httpd_uri_t events = { .uri = "/events", .method = HTTP_GET, .handler = events_handler };
    httpd_uri_t wifi_setup = { .uri = "/wifi_setup", .method = HTTP_POST, .handler = wifi_setup_handler };
    httpd_uri_t wifi_reset = { .uri = "/wifi_reset", .method = HTTP_POST, .handler = wifi_reset_handler };
    httpd_uri_t logs = { .uri = "/logs", .method = HTTP_GET, .handler = logs_handler };
    httpd_uri_t debug = { .uri = "/debug.html", .method = HTTP_GET, .handler = debug_handler };
    httpd_uri_t favicon = { .uri = "/favicon.png", .method = HTTP_GET, .handler = favicon_handler };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &list));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &data));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &getf));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &node));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &events));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_setup));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_reset));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &logs));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &debug));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &favicon));
}

/* ---------------- TIME ---------------- */

static void obtain_time(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Set timezone (Sweden)
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();

    time_t now = 0;
    struct tm timeinfo = { 0 };

    while (timeinfo.tm_year < (2020 - 1900)) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    // Set global time_str to the current time in human-readable format for logging purposes
    strftime(starttime_str, sizeof(starttime_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
}

/* ---------------- FATFS ---------------- */

static void init_fatfs(void)
{
    const esp_vfs_fat_mount_config_t mount_config_www = {
        .format_if_mount_failed = false,
        .max_files = 5
    };

    const esp_vfs_fat_mount_config_t mount_config_storage = {
        .format_if_mount_failed = true,
        .max_files = 10
    };

    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_ro(
        "/www",
        "www",
        &mount_config_www
    ));

    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl(
        "/storage",
        "storage",
        &mount_config_storage,
        &wl_handle_storage
    ));

    mkdir("/storage/data", 0775);
}

/* ---------------- MAIN ---------------- */

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    init_web_log_buffer();

    wifi_init_base();

    char ssid[32] = {0};
    char pass[64] = {0};

    if (load_wifi_config(ssid, sizeof(ssid), pass, sizeof(pass))) {
        wifi_start_sta(ssid, pass);
        obtain_time();
        ESP_LOGI(TAG, "System time obtained: %s", starttime_str);
    } else {
        wifi_start_ap();
    }

    init_fatfs();

    sample_mutex = xSemaphoreCreateMutex();
    assert(sample_mutex != NULL);

    sse_clients_mutex = xSemaphoreCreateMutex();
    assert(sse_clients_mutex != NULL);

    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        sse_clients[i].active = false;
        sse_clients[i].sockfd = -1;
        sse_clients[i].req_async = NULL;
        sse_clients[i].last_sent_seq = 0;
    }

    start_server();

    esp_err_t err = i2c_reader_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Sensor init failed: %s", esp_err_to_name(err));
        return;
    }

    xTaskCreate(sampling_task, "sampling", 4096, NULL, 5, NULL);
    xTaskCreate(sse_broadcast_task, "sse_broadcast", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "%s ready", hostname);
}