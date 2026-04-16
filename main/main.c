#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_mac.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

/* ---------------- CONFIG ---------------- */

#define STORAGE_PATH "/storage"
#define MAX_SAMPLES 128

/* ---------------- GLOBALS ---------------- */

static const char *TAG = "webserver";
static wl_handle_t wl_handle;
static char current_ssid[32] = "UNKNOWN";
static char ip_str[16] = "UNKNOWN";
static char hostname[32] = "UNKNOWN";
static httpd_handle_t server = NULL;
static char starttime_str[64] = "UNKNOWN";

// SSE (Server Side Events)
typedef struct {
    char event[32];
    char data[128];
} sse_msg_t;

static QueueHandle_t sse_queue = NULL;
static int sse_fd = -1;

/* ---------------- SAMPLE STRUCT ---------------- */

typedef struct {
    char ts[32];
    float t, h, p;
} sample_t;

__attribute__((unused)) static sample_t samples[MAX_SAMPLES];
__attribute__((unused)) static int sample_count = 0;

/* ---------------- SSE ---------------- */

static esp_err_t events_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    ESP_LOGI(TAG, "SSE client connected");

    sample_t s;

    while (1) {
        if (xQueueReceive(sse_queue, &s, pdMS_TO_TICKS(15000)) == pdPASS) {
            char buf[192];
            int len = snprintf(buf, sizeof(buf),
                               "event: sample\n"
                               "data: {\"ts\":\"%s\",\"t\":%.2f,\"h\":%.2f,\"p\":%.2f}\n\n",
                               s.ts, s.t, s.h, s.p);

            if (len < 0 || len >= sizeof(buf)) {
                ESP_LOGE(TAG, "SSE message truncated");
                continue;
            }

            esp_err_t err = httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "SSE send failed, client disconnected");
                break;
            }
        } else {
            esp_err_t err = httpd_resp_send_chunk(req, ": keepalive\n\n", HTTPD_RESP_USE_STRLEN);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "SSE keepalive failed, client disconnected");
                break;
            }
        }
    }

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

__attribute__((unused)) static esp_err_t OLD_events_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");

    sse_fd = httpd_req_to_sockfd(req);
    ESP_LOGI(TAG, "SSE connected fd=%d", sse_fd);

    while (sse_fd >= 0) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (httpd_resp_send_chunk(req, ": keepalive\n\n", HTTPD_RESP_USE_STRLEN) != ESP_OK) {
            sse_fd = -1;
            ESP_LOGE(TAG, "SSE connection lost, stopping keepalive");
            break;
        }
    }

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

__attribute__((unused)) static esp_err_t sse_send_event(httpd_handle_t server, int fd, const char *event, const char *data)
{
    if (fd < 0) {
        return ESP_FAIL;
    }

    char buf[256];
    int len = snprintf(buf, sizeof(buf), "event: %s\ndata: %s\n\n", event, data);
    ESP_LOGI(TAG, "Sending SSE: %s", buf);
    if (len < 0 || len >= sizeof(buf)) {
        ESP_LOGE(TAG, "SSE message truncated, not sending");
        return ESP_FAIL;
    }

    if (httpd_socket_send(server, fd, buf, len, 0) < 0) {
        ESP_LOGE(TAG, "Failed to send SSE message");
        return ESP_FAIL;
    }

    return ESP_OK;
}

__attribute__((unused)) static void sse_task(void *arg)
{
    sse_msg_t msg;

    while (1) {
        if (xQueueReceive(sse_queue, &msg, portMAX_DELAY)) {
            if (sse_fd >= 0) {
                if (sse_send_event(server, sse_fd, msg.event, msg.data) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send SSE");
                    sse_fd = -1;
                }
            }
        }
    }
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
    strncpy(current_ssid, ssid, sizeof(current_ssid));

    wifi_config_t wifi_config = { 0 };
    strcpy((char *)wifi_config.sta.ssid, ssid);
    strcpy((char *)wifi_config.sta.password, pass);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
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

/* ---------------- NVS ---------------- */

__attribute__((unused)) static void save_wifi_config(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    nvs_open("wifi", NVS_READWRITE, &nvs);
    nvs_set_str(nvs, "ssid", ssid);
    nvs_set_str(nvs, "pass", pass);
    nvs_commit(nvs);
    nvs_close(nvs);
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

static float get_temperature() { return 20 + (esp_random() % 100) / 10.0; }
static float get_humidity()    { return 40 + (esp_random() % 200) / 10.0; }
static float get_pressure()    { return 1000 + (esp_random() % 100); }

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
    while (1) {
        sample_t s;

        time_t now;
        time(&now);

        struct tm tinfo;
        gmtime_r(&now, &tinfo);

        strftime(s.ts, sizeof(s.ts), "%Y-%m-%dT%H:%M:%SZ", &tinfo);

        s.t = get_temperature();
        s.h = get_humidity();
        s.p = get_pressure();

        char filename[32];
        strftime(filename, sizeof(filename), "%Y-%m-%d.csv", &tinfo);

        append_to_file(filename, &s);
        cleanup_files();

        if (sse_queue != NULL) {
            if (xQueueSend(sse_queue, &s, 0) != pdPASS) {
                ESP_LOGW(TAG, "SSE queue full, dropping live sample");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

/* ---------------- FILE SERVING ---------------- */

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
    return serve_file(req, STORAGE_PATH "/www/index.html");
}

/* ---------------- LIST FILES ---------------- */

static esp_err_t list_files_handler(httpd_req_t *req)
{
    ESP_LOGI("webserver", "LIST FILES handler called");

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
    /* --- IGNORE ---
    // Notify clients about the new IP via SSE
    sse_msg_t msg = {0};
    strlcpy(msg.event, "ip", sizeof(msg.event));
    strlcpy(msg.data, ip_str, sizeof(msg.data));
    xQueueSend(sse_queue, &msg, 0);
    */

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
    
    ESP_LOGI("webserver", "GET NODE handler called");

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

/* ---------------- SERVER ---------------- */

static void start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_start(&server, &config);

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t list = { .uri = "/list_files", .method = HTTP_GET, .handler = list_files_handler };
    httpd_uri_t data = { .uri = "/data", .method = HTTP_GET, .handler = data_handler };
    httpd_uri_t getf = { .uri = "/get_file", .method = HTTP_GET, .handler = get_file_handler };
    httpd_uri_t node = { .uri = "/get_node", .method = HTTP_GET, .handler = get_node_handler };
    httpd_uri_t events = { .uri = "/events", .method = HTTP_GET, .handler = events_handler };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &list);
    httpd_register_uri_handler(server, &data);
    httpd_register_uri_handler(server, &getf);
    httpd_register_uri_handler(server, &node);
    httpd_register_uri_handler(server, &events);
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
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5  // max nr of open files at the same time
    };

    esp_vfs_fat_spiflash_mount_rw_wl(
        STORAGE_PATH,
        "storage",
        &mount_config,
        &wl_handle
    );

    mkdir(STORAGE_PATH "/data", 0775);
    mkdir(STORAGE_PATH "/www", 0775);
}


/* ---------------- MAIN ---------------- */

void app_main(void)
{
    nvs_flash_init();

    wifi_init_base();

    char ssid[32] = {0};
    char pass[64] = {0};

    if (load_wifi_config(ssid, sizeof(ssid), pass, sizeof(pass))) {
        wifi_start_sta(ssid, pass);
    } else {
        wifi_start_ap();
    }

    obtain_time();
    init_fatfs();

    start_server();

    sse_queue = xQueueCreate(8, sizeof(sample_t));
    assert(sse_queue != NULL);

    xTaskCreate(sampling_task, "sampling", 4096, NULL, 5, NULL);

    //sse_queue = xQueueCreate(8, sizeof(sse_msg_t));
    //xTaskCreate(sse_task, "sse_task", 4096, NULL, 5, NULL);
    
    //xTaskCreate(sampling_task, "sampling", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "%s ready", hostname);
}