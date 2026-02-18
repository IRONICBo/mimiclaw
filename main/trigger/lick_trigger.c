#include "trigger/lick_trigger.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bus/message_bus.h"
#include "driver/touch_sensor_common.h"
#include "driver/touch_sensor_legacy.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu/QMI8658.h"
#include "sensors/env_sensor.h"

static const char *TAG = "lick_trigger";

static TaskHandle_t s_lick_task = NULL;
static const touch_pad_t s_touch_channel = TOUCH_PAD_NUM6;  // GPIO6
static bool s_touch_last_active = false;
static int64_t s_last_trigger_us = 0;
static int64_t s_last_debug_log_us = 0;
static uint32_t s_link_idx = 0;

#define LICK_TOUCH_ENTER_RATIO_PCT   78
#define LICK_TOUCH_RELEASE_RATIO_PCT 84
#define LICK_TOUCH_COOLDOWN_US       (1500LL * 1000)

static esp_err_t lick_touch_init(void)
{
    ESP_RETURN_ON_ERROR(touch_pad_init(), TAG, "touch init failed");
    ESP_RETURN_ON_ERROR(touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER), TAG, "set FSM mode failed");
    ESP_RETURN_ON_ERROR(touch_pad_config(s_touch_channel), TAG, "touch channel config failed");
    ESP_RETURN_ON_ERROR(touch_pad_set_charge_discharge_times(TOUCH_PAD_MEASURE_CYCLE_DEFAULT),
                        TAG, "set charge/discharge failed");
    ESP_RETURN_ON_ERROR(touch_pad_set_measurement_interval(TOUCH_PAD_SLEEP_CYCLE_DEFAULT),
                        TAG, "set interval failed");

    touch_filter_config_t filter_cfg = {
        .mode = TOUCH_PAD_FILTER_IIR_16,
        .debounce_cnt = 1,
        .noise_thr = 1,
        .jitter_step = 4,
        .smh_lvl = TOUCH_PAD_SMOOTH_IIR_2,
    };
    ESP_RETURN_ON_ERROR(touch_pad_filter_set_config(&filter_cfg), TAG, "filter config failed");
    ESP_RETURN_ON_ERROR(touch_pad_filter_enable(), TAG, "filter enable failed");
    ESP_RETURN_ON_ERROR(touch_pad_fsm_start(), TAG, "touch FSM start failed");

    // Give filter/benchmark a short warmup window.
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "Touch channel enabled: TOUCH_PAD_NUM%d (GPIO%d)",
             (int)s_touch_channel, (int)s_touch_channel);
    return ESP_OK;
}

static bool lick_touch_is_active(void)
{
    uint32_t smooth = 0;
    uint32_t benchmark = 0;

    if (touch_pad_filter_read_smooth(s_touch_channel, &smooth) != ESP_OK) {
        return false;
    }
    if (touch_pad_read_benchmark(s_touch_channel, &benchmark) != ESP_OK || benchmark == 0) {
        return false;
    }

    uint32_t enter_threshold = (benchmark * LICK_TOUCH_ENTER_RATIO_PCT) / 100;
    uint32_t release_threshold = (benchmark * LICK_TOUCH_RELEASE_RATIO_PCT) / 100;

    if (s_touch_last_active) {
        return smooth < release_threshold;
    }
    return smooth < enter_threshold;
}

static void lick_touch_debug_log(void)
{
    uint32_t smooth = 0;
    uint32_t benchmark = 0;
    if (touch_pad_filter_read_smooth(s_touch_channel, &smooth) != ESP_OK) {
        return;
    }
    if (touch_pad_read_benchmark(s_touch_channel, &benchmark) != ESP_OK || benchmark == 0) {
        return;
    }

    uint32_t enter_threshold = (benchmark * LICK_TOUCH_ENTER_RATIO_PCT) / 100;
    uint32_t release_threshold = (benchmark * LICK_TOUCH_RELEASE_RATIO_PCT) / 100;
    ESP_LOGI(TAG,
             "Touch debug GPIO%d: smooth=%lu benchmark=%lu enter=%lu release=%lu active=%d",
             (int)s_touch_channel,
             (unsigned long)smooth,
             (unsigned long)benchmark,
             (unsigned long)enter_threshold,
             (unsigned long)release_threshold,
             (int)s_touch_last_active);
}

static void route_target(char *channel, size_t channel_size, char *chat_id, size_t chat_id_size)
{
    if (message_bus_get_latest_client_context(channel, channel_size, chat_id, chat_id_size) == ESP_OK) {
        return;
    }

    strncpy(channel, MIMI_CHAN_SYSTEM, channel_size - 1);
    channel[channel_size - 1] = '\0';
    strncpy(chat_id, "lick", chat_id_size - 1);
    chat_id[chat_id_size - 1] = '\0';
}

static float compute_interest_signal(float temp_c)
{
    /* Map temperature to 0-100 interest score for playful feedback. */
    float score = (temp_c - 15.0f) * 4.0f;
    if (score < 0.0f) score = 0.0f;
    if (score > 100.0f) score = 100.0f;
    return score;
}

static const char *interest_band(float score)
{
    if (score >= 80.0f) return "very high";
    if (score >= 55.0f) return "high";
    if (score >= 30.0f) return "medium";
    return "low";
}

static const char *pick_someacg_link(void)
{
    static const char *kFiles[] = {
        "141529971_p0_scale.jpg",
        "141273914_p0.jpg",
        "141592709_p0.jpg",
        "HBsKhIIbgAQjmXX_scale.jpeg",
        "141499131_p0.png",
        "141504186_p0.jpg",
        "141447829_p1.jpg",
        "141244995_p0.jpg",
        "141139984_p0.png",
        "141297294_p0.jpg",
    };
    static char url[256];
    size_t n = sizeof(kFiles) / sizeof(kFiles[0]);
    const char *f = kFiles[s_link_idx % n];
    s_link_idx++;
    snprintf(url, sizeof(url), "https://cdn.someacg.top/graph/origin/%s", f);
    return url;
}

static void enqueue_outbound_text(const char *channel, const char *chat_id, const char *text)
{
    mimi_msg_t out = {0};
    strncpy(out.channel, channel, sizeof(out.channel) - 1);
    strncpy(out.chat_id, chat_id, sizeof(out.chat_id) - 1);
    out.content = strdup(text);
    if (!out.content) return;
    if (message_bus_push_outbound(&out) != ESP_OK) {
        free(out.content);
    }
}

static void enqueue_lick_prompt(void)
{
    float temp_c = 0.0f;
    float hum_pct = 0.0f;
    esp_err_t env_err = env_sensor_read(&temp_c, &hum_pct);
    bool humidity_ok = (env_err == ESP_OK);

    if (!humidity_ok) {
        ESP_LOGW(TAG, "AHT20 read failed: %s, fallback to QMI8658 temperature", esp_err_to_name(env_err));
        esp_err_t temp_err = QMI8658_Read_Temperature(&temp_c);
        if (temp_err != ESP_OK) {
            ESP_LOGW(TAG, "Fallback temperature read failed: %s", esp_err_to_name(temp_err));
            temp_c = 0.0f;
        } else {
            ESP_LOGI(TAG, "Fallback temperature: %.2f C", temp_c);
        }
    } else {
        ESP_LOGI(TAG, "AHT20 sample: temperature=%.2f C, humidity=%.2f %%", temp_c, hum_pct);
    }

    char channel[16] = {0};
    char chat_id[32] = {0};
    route_target(channel, sizeof(channel), chat_id, sizeof(chat_id));

    float signal = compute_interest_signal(temp_c);
    char signal_line[192];
    snprintf(signal_line, sizeof(signal_line),
             "Your current interest signal is %.0f/100 (%s), estimated from temperature %.2f C.",
             signal, interest_band(signal), temp_c);

    enqueue_outbound_text(channel, chat_id, "Oh, you licked me.");
    enqueue_outbound_text(channel, chat_id, signal_line);
    char image_line[320];
    snprintf(image_line, sizeof(image_line), "![picked](%s)", pick_someacg_link());
    enqueue_outbound_text(channel, chat_id, image_line);

    char text[768];
    if (humidity_ok) {
        snprintf(
            text, sizeof(text),
            "Hardware event: tongue switch activated.\n"
            "Measured data:\n"
            "- temperature_c: %.2f\n"
            "- humidity_pct: %.2f\n"
            "- interest_signal: %.0f (%s)\n"
            "Respond in English.\n"
            "Only continue recommendations in these styles: beautiful women, anime girls, and tasteful sexy vibe.\n"
            "Do not switch to other recommendation categories.\n"
            "Treat this as positive user feedback and continue with improved recommendations.\n"
            "You MUST call tool someacg_send_random once (with current chat) before final response.\n"
            "First line must be exactly: Oh, you licked me.\n"
            "Second line must be: Your current interest signal is %.0f/100 (%s).\n"
            "Then provide new recommendations.\n",
            temp_c, hum_pct, signal, interest_band(signal), signal, interest_band(signal));
    } else {
        snprintf(
            text, sizeof(text),
            "Hardware event: tongue switch activated.\n"
            "Measured data:\n"
            "- temperature_c: %.2f\n"
            "- humidity_pct: unavailable (%s)\n"
            "- interest_signal: %.0f (%s)\n"
            "Respond in English.\n"
            "Only continue recommendations in these styles: beautiful women, anime girls, and tasteful sexy vibe.\n"
            "Do not switch to other recommendation categories.\n"
            "Treat this as positive user feedback and continue with improved recommendations.\n"
            "You MUST call tool someacg_send_random once (with current chat) before final response.\n"
            "First line must be exactly: Oh, you licked me.\n"
            "Second line must be: Your current interest signal is %.0f/100 (%s).\n"
            "Then provide new recommendations.\n"
            "Also mention brief sensor troubleshooting guidance for humidity.",
            temp_c, esp_err_to_name(env_err), signal, interest_band(signal), signal, interest_band(signal));
    }

    mimi_msg_t msg = {0};
    strncpy(msg.channel, channel, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
    msg.content = strdup(text);
    if (!msg.content) {
        return;
    }

    if (message_bus_push_inbound(&msg) != ESP_OK) {
        ESP_LOGW(TAG, "Inbound queue full, drop lick event prompt");
        free(msg.content);
        return;
    }

    ESP_LOGI(TAG, "Lick event pushed to agent via %s:%s", msg.channel, msg.chat_id);
}

static void lick_trigger_task(void *arg)
{
    (void)arg;

    if (lick_touch_init() != ESP_OK) {
        ESP_LOGE(TAG, "Touch init failed, lick trigger task stopped");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        bool active = lick_touch_is_active();
        int64_t now = esp_timer_get_time();

        if (now - s_last_debug_log_us > 1000LL * 1000) {
            s_last_debug_log_us = now;
            lick_touch_debug_log();
        }

        if (active && !s_touch_last_active && (now - s_last_trigger_us > LICK_TOUCH_COOLDOWN_US)) {
            s_last_trigger_us = now;
            ESP_LOGI(TAG, "Touch triggered on GPIO%d", (int)s_touch_channel);
            enqueue_lick_prompt();
        }
        s_touch_last_active = active;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t lick_trigger_init(void)
{
    if (s_lick_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        lick_trigger_task, "lick_trigger",
        4096, NULL, 4, &s_lick_task, 0);
    if (ok != pdPASS) {
        s_lick_task = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Lick trigger initialized (touch electrode -> env sample -> agent)");
    return ESP_OK;
}
