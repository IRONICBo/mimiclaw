#include "trigger/lick_trigger.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bus/message_bus.h"
#include "buttons/button_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu/QMI8658.h"
#include "sensors/env_sensor.h"

static const char *TAG = "lick_trigger";

static TaskHandle_t s_lick_task = NULL;

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

static void enqueue_lick_prompt(void)
{
    float temp_c = 0.0f;
    float hum_pct = 0.0f;
    esp_err_t env_err = env_sensor_read(&temp_c, &hum_pct);
    bool humidity_ok = (env_err == ESP_OK);

    if (!humidity_ok) {
        esp_err_t temp_err = QMI8658_Read_Temperature(&temp_c);
        if (temp_err != ESP_OK) {
            ESP_LOGW(TAG, "Fallback temperature read failed: %s", esp_err_to_name(temp_err));
            temp_c = 0.0f;
        }
    }

    char channel[16] = {0};
    char chat_id[32] = {0};
    route_target(channel, sizeof(channel), chat_id, sizeof(chat_id));

    char text[512];
    if (humidity_ok) {
        snprintf(
            text, sizeof(text),
            "Hardware event: tongue switch activated.\n"
            "Measured data:\n"
            "- temperature_c: %.2f\n"
            "- humidity_pct: %.2f\n"
            "Please summarize this state and recommend the next step in concise bullets.",
            temp_c, hum_pct);
    } else {
        snprintf(
            text, sizeof(text),
            "Hardware event: tongue switch activated.\n"
            "Measured data:\n"
            "- temperature_c: %.2f\n"
            "- humidity_pct: unavailable (%s)\n"
            "Please summarize the state and recommend the next step, including sensor troubleshooting guidance.",
            temp_c, esp_err_to_name(env_err));
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
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        enqueue_lick_prompt();
    }
}

static void on_button_event(PressEvent event)
{
    if (event == SINGLE_CLICK && s_lick_task) {
        xTaskNotifyGive(s_lick_task);
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

    button_set_event_callback(on_button_event);
    ESP_LOGI(TAG, "Lick trigger initialized (SINGLE_CLICK -> env sample -> agent)");
    return ESP_OK;
}
