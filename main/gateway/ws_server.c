#include "ws_server.h"
#include "mimi_config.h"
#include "bus/message_bus.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "cJSON.h"

static const char *TAG = "ws";

static httpd_handle_t s_server = NULL;

/* Simple client tracking */
typedef struct {
    int fd;
    char chat_id[32];
    bool active;
} ws_client_t;

static ws_client_t s_clients[MIMI_WS_MAX_CLIENTS];
static int s_extension_fd = -1;

typedef struct {
    char request_id[64];
    bool ok;
    char payload[3072];
} ws_cmd_result_t;

static QueueHandle_t s_browser_result_q = NULL;

static ws_client_t *find_client_by_fd(int fd)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *find_client_by_chat_id(const char *chat_id)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && strcmp(s_clients[i].chat_id, chat_id) == 0) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *add_client(int fd)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            s_clients[i].fd = fd;
            snprintf(s_clients[i].chat_id, sizeof(s_clients[i].chat_id), "ws_%d", fd);
            s_clients[i].active = true;
            ESP_LOGI(TAG, "Client connected: %s (fd=%d)", s_clients[i].chat_id, fd);
            return &s_clients[i];
        }
    }
    ESP_LOGW(TAG, "Max clients reached, rejecting fd=%d", fd);
    return NULL;
}

static void remove_client(int fd)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            ESP_LOGI(TAG, "Client disconnected: %s", s_clients[i].chat_id);
            s_clients[i].active = false;
            if (s_extension_fd == fd) {
                s_extension_fd = -1;
            }
            return;
        }
    }
}

static esp_err_t ws_send_json_fd(int fd, const char *json_str)
{
    if (!s_server || fd < 0 || !json_str) return ESP_ERR_INVALID_ARG;
    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_str,
        .len = strlen(json_str),
    };
    return httpd_ws_send_frame_async(s_server, fd, &ws_pkt);
}

static void queue_browser_result(cJSON *root)
{
    if (!s_browser_result_q || !root) return;
    cJSON *rid = cJSON_GetObjectItem(root, "request_id");
    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (!rid || !cJSON_IsString(rid) || !ok || !cJSON_IsBool(ok)) return;

    ws_cmd_result_t item = {0};
    snprintf(item.request_id, sizeof(item.request_id), "%s", rid->valuestring);
    item.ok = cJSON_IsTrue(ok);

    if (item.ok) {
        cJSON *result = cJSON_GetObjectItem(root, "result");
        if (result) {
            char *txt = cJSON_PrintUnformatted(result);
            if (txt) {
                snprintf(item.payload, sizeof(item.payload), "%s", txt);
                free(txt);
            }
        } else {
            snprintf(item.payload, sizeof(item.payload), "{}");
        }
    } else {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        if (err && cJSON_IsString(err)) {
            snprintf(item.payload, sizeof(item.payload), "%s", err->valuestring);
        } else {
            snprintf(item.payload, sizeof(item.payload), "unknown_error");
        }
    }

    xQueueSend(s_browser_result_q, &item, 0);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* WebSocket handshake — register client */
        int fd = httpd_req_to_sockfd(req);
        add_client(fd);
        return ESP_OK;
    }

    /* Receive WebSocket frame */
    httpd_ws_frame_t ws_pkt = {0};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    /* Get frame length */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.len == 0) return ESP_OK;

    ws_pkt.payload = calloc(1, ws_pkt.len + 1);
    if (!ws_pkt.payload) return ESP_ERR_NO_MEM;

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(ws_pkt.payload);
        return ret;
    }

    int fd = httpd_req_to_sockfd(req);
    ws_client_t *client = find_client_by_fd(fd);

    /* Parse JSON message */
    cJSON *root = cJSON_Parse((char *)ws_pkt.payload);
    free(ws_pkt.payload);

    if (!root) {
        ESP_LOGW(TAG, "Invalid JSON from fd=%d", fd);
        return ESP_OK;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *content = cJSON_GetObjectItem(root, "content");

    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "register") == 0) {
        cJSON *role = cJSON_GetObjectItem(root, "role");
        if (role && cJSON_IsString(role) && strcmp(role->valuestring, "extension") == 0) {
            s_extension_fd = fd;
            if (client) {
                snprintf(client->chat_id, sizeof(client->chat_id), "browser_extension");
            }
            cJSON *ack = cJSON_CreateObject();
            cJSON_AddStringToObject(ack, "type", "register_ack");
            cJSON_AddNumberToObject(ack, "ts", (double)(esp_timer_get_time() / 1000));
            char *ack_str = cJSON_PrintUnformatted(ack);
            cJSON_Delete(ack);
            if (ack_str) {
                ws_send_json_fd(fd, ack_str);
                free(ack_str);
            }
            ESP_LOGI(TAG, "Extension registered (fd=%d)", fd);
        }
        cJSON_Delete(root);
        return ESP_OK;
    }

    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "ping") == 0) {
        cJSON *pong = cJSON_CreateObject();
        cJSON_AddStringToObject(pong, "type", "pong");
        cJSON *ts = cJSON_GetObjectItem(root, "ts");
        if (ts && cJSON_IsNumber(ts)) {
            cJSON_AddNumberToObject(pong, "ts", ts->valuedouble);
        } else {
            cJSON_AddNumberToObject(pong, "ts", (double)(esp_timer_get_time() / 1000));
        }
        char *pong_str = cJSON_PrintUnformatted(pong);
        cJSON_Delete(pong);
        if (pong_str) {
            ws_send_json_fd(fd, pong_str);
            free(pong_str);
        }
        cJSON_Delete(root);
        return ESP_OK;
    }

    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "command_result") == 0) {
        queue_browser_result(root);
        cJSON_Delete(root);
        return ESP_OK;
    }

    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "message") == 0
        && content && cJSON_IsString(content)) {

        /* Determine chat_id */
        const char *chat_id = client ? client->chat_id : "ws_unknown";
        cJSON *cid = cJSON_GetObjectItem(root, "chat_id");
        if (cid && cJSON_IsString(cid)) {
            chat_id = cid->valuestring;
            /* Update client's chat_id if provided */
            if (client) {
                strncpy(client->chat_id, chat_id, sizeof(client->chat_id) - 1);
            }
        }

        ESP_LOGI(TAG, "WS message from %s: %.40s...", chat_id, content->valuestring);

        /* Push to inbound bus */
        mimi_msg_t msg = {0};
        strncpy(msg.channel, MIMI_CHAN_WEBSOCKET, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
        msg.content = strdup(content->valuestring);
        if (msg.content) {
            message_bus_push_inbound(&msg);
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ws_server_start(void)
{
    memset(s_clients, 0, sizeof(s_clients));
    s_extension_fd = -1;
    if (!s_browser_result_q) {
        s_browser_result_q = xQueueCreate(8, sizeof(ws_cmd_result_t));
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = MIMI_WS_PORT;
    config.ctrl_port = MIMI_WS_PORT + 1;
    config.max_open_sockets = MIMI_WS_MAX_CLIENTS;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register WebSocket URI */
    httpd_uri_t ws_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    ESP_LOGI(TAG, "WebSocket server started on port %d", MIMI_WS_PORT);
    return ESP_OK;
}

esp_err_t ws_server_send(const char *chat_id, const char *text)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;

    ws_client_t *client = find_client_by_chat_id(chat_id);
    if (!client) {
        ESP_LOGW(TAG, "No WS client with chat_id=%s", chat_id);
        return ESP_ERR_NOT_FOUND;
    }

    /* Build response JSON */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", text);
    cJSON_AddStringToObject(resp, "chat_id", chat_id);

    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (!json_str) return ESP_ERR_NO_MEM;

    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_str,
        .len = strlen(json_str),
    };

    esp_err_t ret = httpd_ws_send_frame_async(s_server, client->fd, &ws_pkt);
    free(json_str);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send to %s: %s", chat_id, esp_err_to_name(ret));
        remove_client(client->fd);
    }

    return ret;
}

esp_err_t ws_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "WebSocket server stopped");
    }
    return ESP_OK;
}

esp_err_t ws_server_browser_rpc(const char *type, const char *extra_json,
                                char *out_payload, size_t out_size,
                                bool *out_ok, uint32_t timeout_ms)
{
    if (!type || !out_payload || out_size == 0 || !out_ok) return ESP_ERR_INVALID_ARG;
    if (!s_server || s_extension_fd < 0) {
        snprintf(out_payload, out_size, "browser_extension_not_connected");
        *out_ok = false;
        return ESP_ERR_INVALID_STATE;
    }

    char request_id[64];
    snprintf(request_id, sizeof(request_id), "req_%08x%08x", (unsigned)esp_random(), (unsigned)esp_random());

    /* Drop stale results from previous RPC rounds. */
    ws_cmd_result_t stale;
    while (s_browser_result_q && xQueueReceive(s_browser_result_q, &stale, 0) == pdTRUE) {}

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "request_id", request_id);

    if (extra_json && extra_json[0]) {
        cJSON *extra = cJSON_Parse(extra_json);
        if (extra && cJSON_IsObject(extra)) {
            cJSON *child = extra->child;
            while (child) {
                cJSON_AddItemToObject(root, child->string, cJSON_Duplicate(child, 1));
                child = child->next;
            }
        }
        cJSON_Delete(extra);
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        snprintf(out_payload, out_size, "build_payload_failed");
        *out_ok = false;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ws_send_json_fd(s_extension_fd, payload);
    free(payload);
    if (err != ESP_OK) {
        snprintf(out_payload, out_size, "send_failed:%s", esp_err_to_name(err));
        *out_ok = false;
        return err;
    }

    int64_t start_ms = esp_timer_get_time() / 1000;
    ws_cmd_result_t item;
    while ((esp_timer_get_time() / 1000 - start_ms) < (int64_t)timeout_ms) {
        if (xQueueReceive(s_browser_result_q, &item, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (strcmp(item.request_id, request_id) == 0) {
                snprintf(out_payload, out_size, "%s", item.payload);
                *out_ok = item.ok;
                return ESP_OK;
            }
        }
    }

    snprintf(out_payload, out_size, "timeout_waiting_command_result");
    *out_ok = false;
    return ESP_ERR_TIMEOUT;
}
