#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"
#include "telegram/telegram_bot.h"
#include "llm/llm_proxy.h"
#include "agent/agent_loop.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "gateway/ws_server.h"
#include "cli/serial_cli.h"
#include "proxy/http_proxy.h"
#include "tools/tool_registry.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "buttons/button_driver.h"
#include "imu/imu_manager.h"
#include "skills/skill_loader.h"
#include "trigger/lick_trigger.h"

static const char *TAG = "mimi";

static char *trim_left(char *s)
{
    while (s && *s && isspace((unsigned char)*s)) s++;
    return s;
}

/* Parse one markdown image line: ![caption](https://...)
 * Returns true when parsed and values are copied into out buffers.
 */
static bool parse_markdown_image_line(const char *line, char *out_url, size_t out_url_sz,
                                      char *out_caption, size_t out_caption_sz)
{
    if (!line || strncmp(line, "![", 2) != 0) return false;
    const char *cap_end = strstr(line + 2, "](");
    if (!cap_end) return false;
    const char *url_start = cap_end + 2;
    const char *url_end = strchr(url_start, ')');
    if (!url_end || url_end <= url_start) return false;

    size_t cap_len = (size_t)(cap_end - (line + 2));
    size_t url_len = (size_t)(url_end - url_start);
    if (url_len == 0 || url_len >= out_url_sz) return false;

    memcpy(out_url, url_start, url_len);
    out_url[url_len] = '\0';
    if (strncmp(out_url, "https://", 8) != 0) return false;

    if (out_caption && out_caption_sz > 0) {
        size_t cpy = cap_len < (out_caption_sz - 1) ? cap_len : (out_caption_sz - 1);
        memcpy(out_caption, line + 2, cpy);
        out_caption[cpy] = '\0';
    }
    return true;
}

/* Telegram dispatcher with image support.
 * If content includes markdown image lines (![]()), images are sent via sendPhoto.
 * Remaining text is sent via sendMessage.
 */
static esp_err_t telegram_send_rich(const char *chat_id, const char *content)
{
    if (!chat_id || !content) return ESP_ERR_INVALID_ARG;

    size_t n = strlen(content);
    char *scratch = malloc(n + 1);
    if (!scratch) return ESP_ERR_NO_MEM;
    memcpy(scratch, content, n + 1);

    char *text_buf = calloc(1, n + 1);
    if (!text_buf) {
        free(scratch);
        return ESP_ERR_NO_MEM;
    }

    bool any_fail = false;
    size_t text_off = 0;
    char *save = NULL;
    for (char *line = strtok_r(scratch, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *trim = trim_left(line);
        char image_url[384] = {0};
        char caption[128] = {0};

        if (parse_markdown_image_line(trim, image_url, sizeof(image_url), caption, sizeof(caption))) {
            esp_err_t e = telegram_send_photo(chat_id, image_url, caption[0] ? caption : NULL);
            if (e != ESP_OK) {
                any_fail = true;
                ESP_LOGW(TAG, "sendPhoto failed, fallback to text URL");
                text_off += snprintf(text_buf + text_off, n + 1 - text_off, "%s\n", image_url);
            }
            continue;
        }

        text_off += snprintf(text_buf + text_off, n + 1 - text_off, "%s\n", line);
    }

    esp_err_t text_err = ESP_OK;
    if (text_buf[0] != '\0') {
        text_err = telegram_send_message(chat_id, text_buf);
    }

    free(text_buf);
    free(scratch);

    if (any_fail || text_err != ESP_OK) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MIMI_SPIFFS_BASE,
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", (int)total, (int)used);

    return ESP_OK;
}

/* Outbound dispatch task: reads from outbound queue and routes to channels */
static void outbound_dispatch_task(void *arg)
{
    ESP_LOGI(TAG, "Outbound dispatch started");

    while (1) {
        mimi_msg_t msg;
        if (message_bus_pop_outbound(&msg, UINT32_MAX) != ESP_OK) continue;

        ESP_LOGI(TAG, "Dispatching response to %s:%s", msg.channel, msg.chat_id);

        if (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0) {
            esp_err_t send_err = telegram_send_rich(msg.chat_id, msg.content);
            if (send_err != ESP_OK) {
                ESP_LOGE(TAG, "Telegram send failed for %s: %s", msg.chat_id, esp_err_to_name(send_err));
            } else {
                ESP_LOGI(TAG, "Telegram send success for %s (%d bytes)", msg.chat_id, (int)strlen(msg.content));
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_WEBSOCKET) == 0) {
            esp_err_t ws_err = ws_server_send(msg.chat_id, msg.content);
            if (ws_err != ESP_OK) {
                ESP_LOGW(TAG, "WS send failed for %s: %s", msg.chat_id, esp_err_to_name(ws_err));
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_SYSTEM) == 0) {
            ESP_LOGI(TAG, "System message [%s]: %.128s", msg.chat_id, msg.content);
        } else {
            ESP_LOGW(TAG, "Unknown channel: %s", msg.channel);
        }

        free(msg.content);
    }
}

void app_main(void)
{
    /* Silence noisy components */
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  MimiClaw - ESP32-S3 AI Agent");
    ESP_LOGI(TAG, "========================================");

    /* Print memory info */
    ESP_LOGI(TAG, "Internal free: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "PSRAM free:    %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Input */
    button_Init();
    imu_manager_init();
    imu_manager_set_shake_callback(NULL);

    /* Phase 1: Core infrastructure */
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(init_spiffs());

    /* Initialize subsystems */
    ESP_ERROR_CHECK(message_bus_init());
    ESP_ERROR_CHECK(lick_trigger_init());
    ESP_ERROR_CHECK(memory_store_init());
    ESP_ERROR_CHECK(skill_loader_init());
    ESP_ERROR_CHECK(session_mgr_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_proxy_init());
    ESP_ERROR_CHECK(telegram_bot_init());
    ESP_ERROR_CHECK(llm_proxy_init());
    ESP_ERROR_CHECK(tool_registry_init());
    ESP_ERROR_CHECK(cron_service_init());
    ESP_ERROR_CHECK(heartbeat_init());
    ESP_ERROR_CHECK(agent_loop_init());

    /* Start Serial CLI first (works without WiFi) */
    ESP_ERROR_CHECK(serial_cli_init());

    /* Start WiFi */
    esp_err_t wifi_err = wifi_manager_start();
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Scanning nearby APs on boot...");
        wifi_manager_scan_and_print();
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        if (wifi_manager_wait_connected(30000) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());

            /* Outbound dispatch task should start first to avoid dropping early replies. */
            ESP_ERROR_CHECK((xTaskCreatePinnedToCore(
                outbound_dispatch_task, "outbound",
                MIMI_OUTBOUND_STACK, NULL,
                MIMI_OUTBOUND_PRIO, NULL, MIMI_OUTBOUND_CORE) == pdPASS)
                ? ESP_OK : ESP_FAIL);

            /* Start network-dependent services */
            ESP_ERROR_CHECK(agent_loop_start());
            ESP_ERROR_CHECK(telegram_bot_start());
            cron_service_start();
            heartbeat_start();
            ESP_ERROR_CHECK(ws_server_start());

            ESP_LOGI(TAG, "All services started!");
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout. Check MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
        }
    } else {
        ESP_LOGW(TAG, "No WiFi credentials. Set MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
    }

    ESP_LOGI(TAG, "MimiClaw ready. Type 'help' for CLI commands.");
}
