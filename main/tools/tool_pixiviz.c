#include "tools/tool_pixiviz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus/message_bus.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "proxy/http_proxy.h"
#include "telegram/telegram_bot.h"

#include "cJSON.h"

#define SOMEACG_HOST "www.someacg.top"
#define SOMEACG_LIST_PATH_FMT "/api/list?quality=false&page=%d&size=0"
#define SOMEACG_UA "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36"
#define SOMEACG_CDN_FMT "https://cdn.someacg.top/graph/origin/%s"

#define LIST_BUF_SIZE (96 * 1024)
#define LIST_LIMIT 20

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t resp_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (!rb) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (rb->len + evt->data_len + 1 > rb->cap) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(rb->buf + rb->len, evt->data, evt->data_len);
        rb->len += evt->data_len;
        rb->buf[rb->len] = '\0';
    }
    return ESP_OK;
}

static bool looks_like_html_challenge(const char *s)
{
    if (!s) return false;
    return strstr(s, "<!DOCTYPE html") != NULL ||
           strstr(s, "<html") != NULL ||
           strstr(s, "Security Checkpoint") != NULL ||
           strstr(s, "Just a moment") != NULL;
}

static esp_err_t someacg_list_request_direct(int page, char *out, size_t out_size)
{
    char path[128];
    snprintf(path, sizeof(path), SOMEACG_LIST_PATH_FMT, page);

    char url[256];
    snprintf(url, sizeof(url), "https://%s%s", SOMEACG_HOST, path);

    resp_buf_t rb = {
        .buf = out,
        .len = 0,
        .cap = out_size,
    };
    out[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = resp_event_handler,
        .user_data = &rb,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Referer", "https://www.someacg.top/");
    esp_http_client_set_header(client, "User-Agent", SOMEACG_UA);
    esp_http_client_set_header(client, "sec-ch-ua-platform", "\"macOS\"");
    esp_http_client_set_header(client, "sec-ch-ua", "\"Not:A-Brand\";v=\"99\", \"Google Chrome\";v=\"145\", \"Chromium\";v=\"145\"");
    esp_http_client_set_header(client, "sec-ch-ua-mobile", "?0");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (status != 200) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t someacg_list_request_proxy(int page, char *out, size_t out_size)
{
    char path[128];
    snprintf(path, sizeof(path), SOMEACG_LIST_PATH_FMT, page);

    proxy_conn_t *conn = proxy_conn_open(SOMEACG_HOST, 443, 15000);
    if (!conn) return ESP_FAIL;

    char req[1024];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Referer: https://www.someacg.top/\r\n"
        "User-Agent: %s\r\n"
        "sec-ch-ua-platform: \"macOS\"\r\n"
        "sec-ch-ua: \"Not:A-Brand\";v=\"99\", \"Google Chrome\";v=\"145\", \"Chromium\";v=\"145\"\r\n"
        "sec-ch-ua-mobile: ?0\r\n"
        "Connection: close\r\n\r\n",
        path, SOMEACG_HOST, SOMEACG_UA);
    if (n <= 0 || n >= (int)sizeof(req) || proxy_conn_write(conn, req, n) < 0) {
        proxy_conn_close(conn);
        return ESP_FAIL;
    }

    size_t total = 0;
    char tmp[2048];
    while (1) {
        int r = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
        if (r <= 0) break;
        size_t cpy = (total + (size_t)r < out_size - 1) ? (size_t)r : (out_size - 1 - total);
        if (cpy > 0) {
            memcpy(out + total, tmp, cpy);
            total += cpy;
        }
    }
    out[total] = '\0';
    proxy_conn_close(conn);

    int status = 0;
    if (strncmp(out, "HTTP/", 5) == 0) {
        const char *sp = strchr(out, ' ');
        if (sp) status = atoi(sp + 1);
    }

    char *body = strstr(out, "\r\n\r\n");
    if (!body) return ESP_FAIL;
    body += 4;
    size_t blen = total - (size_t)(body - out);
    memmove(out, body, blen);
    out[blen] = '\0';

    if (status != 200) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t someacg_fetch_list_raw(int page, char **out_raw)
{
    *out_raw = NULL;
    char *buf = calloc(1, LIST_BUF_SIZE);
    if (!buf) return ESP_ERR_NO_MEM;

    esp_err_t err = http_proxy_is_enabled()
        ? someacg_list_request_proxy(page, buf, LIST_BUF_SIZE)
        : someacg_list_request_direct(page, buf, LIST_BUF_SIZE);

    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    if (looks_like_html_challenge(buf)) {
        free(buf);
        return ESP_FAIL;
    }

    *out_raw = buf;
    return ESP_OK;
}

static cJSON *find_items_array(cJSON *root)
{
    if (cJSON_IsArray(root)) return root;
    if (!cJSON_IsObject(root)) return NULL;

    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(items)) return items;
    items = cJSON_GetObjectItem(root, "list");
    if (cJSON_IsArray(items)) return items;
    items = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsArray(items)) return items;
    if (cJSON_IsObject(items)) {
        cJSON *inner = cJSON_GetObjectItem(items, "items");
        if (cJSON_IsArray(inner)) return inner;
    }
    return NULL;
}

static const char *get_str(cJSON *obj, const char *name)
{
    cJSON *it = cJSON_GetObjectItem(obj, name);
    return cJSON_IsString(it) ? it->valuestring : "";
}

static void build_image_url(const char *file_name, char *out, size_t out_size)
{
    if (!file_name || !file_name[0]) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_size, SOMEACG_CDN_FMT, file_name);
}

static esp_err_t resolve_chat_id(const char *chat_id_in, char *chat_id_out, size_t out_size)
{
    if (chat_id_in && chat_id_in[0]) {
        snprintf(chat_id_out, out_size, "%s", chat_id_in);
        return ESP_OK;
    }

    char ch[16] = {0};
    if (message_bus_get_latest_client_context(ch, sizeof(ch), chat_id_out, out_size) == ESP_OK &&
        strcmp(ch, MIMI_CHAN_TELEGRAM) == 0 && chat_id_out[0]) {
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t tool_someacg_list_execute(const char *input_json, char *output, size_t output_size)
{
    int page = 1;
    cJSON *in = cJSON_Parse(input_json);
    if (in) {
        cJSON *p = cJSON_GetObjectItem(in, "page");
        if (cJSON_IsNumber(p) && p->valueint > 0) page = p->valueint;
    }

    char *raw = NULL;
    esp_err_t err = someacg_fetch_list_raw(page, &raw);
    if (err != ESP_OK) {
        cJSON_Delete(in);
        snprintf(output, output_size,
                 "Error: someacg list fetch failed (possibly blocked by security checkpoint)");
        return err;
    }

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) {
        cJSON_Delete(in);
        snprintf(output, output_size, "Error: someacg list invalid JSON");
        return ESP_FAIL;
    }

    cJSON *arr_src = find_items_array(root);
    if (!cJSON_IsArray(arr_src)) {
        cJSON_Delete(root);
        cJSON_Delete(in);
        snprintf(output, output_size, "Error: someacg list has no array items");
        return ESP_FAIL;
    }

    cJSON *ret = cJSON_CreateObject();
    cJSON_AddStringToObject(ret, "source", "someacg");
    cJSON_AddNumberToObject(ret, "page", page);
    cJSON *arr = cJSON_AddArrayToObject(ret, "items");

    int count = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr_src) {
        if (count >= LIST_LIMIT) break;
        const char *file_name = get_str(it, "file_name");
        if (!file_name[0]) continue;

        char image_url[512];
        build_image_url(file_name, image_url, sizeof(image_url));

        cJSON *row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "id", get_str(it, "_id"));
        cJSON_AddStringToObject(row, "title", get_str(it, "title"));
        cJSON_AddStringToObject(row, "desc", get_str(it, "desc"));
        cJSON_AddStringToObject(row, "file_name", file_name);
        cJSON_AddStringToObject(row, "image_url", image_url);
        cJSON_AddItemToArray(arr, row);
        count++;
    }

    cJSON_AddNumberToObject(ret, "count", count);
    char *out_json = cJSON_PrintUnformatted(ret);
    cJSON_Delete(ret);
    cJSON_Delete(root);
    cJSON_Delete(in);

    if (!out_json) {
        snprintf(output, output_size, "Error: failed to format someacg list result");
        return ESP_FAIL;
    }

    snprintf(output, output_size, "%s", out_json);
    free(out_json);
    return ESP_OK;
}

esp_err_t tool_someacg_send_image_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *in = cJSON_Parse(input_json);
    if (!in) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *file_name = cJSON_GetStringValue(cJSON_GetObjectItem(in, "file_name"));
    const char *image_url_in = cJSON_GetStringValue(cJSON_GetObjectItem(in, "image_url"));
    const char *chat_id_in = cJSON_GetStringValue(cJSON_GetObjectItem(in, "chat_id"));
    const char *caption = cJSON_GetStringValue(cJSON_GetObjectItem(in, "caption"));

    char image_url[512] = {0};
    if (image_url_in && strncmp(image_url_in, "https://", 8) == 0) {
        snprintf(image_url, sizeof(image_url), "%s", image_url_in);
    } else if (file_name && file_name[0]) {
        build_image_url(file_name, image_url, sizeof(image_url));
    }

    if (!image_url[0]) {
        cJSON_Delete(in);
        snprintf(output, output_size, "Error: need image_url or file_name");
        return ESP_ERR_INVALID_ARG;
    }

    char chat_id[32] = {0};
    esp_err_t cid_err = resolve_chat_id(chat_id_in, chat_id, sizeof(chat_id));
    if (cid_err != ESP_OK) {
        cJSON_Delete(in);
        snprintf(output, output_size, "Error: missing chat_id and no active Telegram context");
        return cid_err;
    }

    esp_err_t send_err = telegram_send_photo(chat_id, image_url, caption && caption[0] ? caption : NULL);
    cJSON_Delete(in);

    if (send_err != ESP_OK) {
        snprintf(output, output_size, "Error: Telegram sendPhoto failed for %s", image_url);
        return send_err;
    }

    snprintf(output, output_size, "OK: sent image to chat %s: %s", chat_id, image_url);
    return ESP_OK;
}

esp_err_t tool_someacg_send_random_execute(const char *input_json, char *output, size_t output_size)
{
    int page = 1;
    const char *chat_id_in = NULL;
    const char *caption = NULL;

    cJSON *in = cJSON_Parse(input_json);
    if (in) {
        cJSON *p = cJSON_GetObjectItem(in, "page");
        if (cJSON_IsNumber(p) && p->valueint > 0) page = p->valueint;
        chat_id_in = cJSON_GetStringValue(cJSON_GetObjectItem(in, "chat_id"));
        caption = cJSON_GetStringValue(cJSON_GetObjectItem(in, "caption"));
    }

    char *raw = NULL;
    esp_err_t err = someacg_fetch_list_raw(page, &raw);
    if (err != ESP_OK) {
        cJSON_Delete(in);
        snprintf(output, output_size,
                 "Error: someacg list fetch failed (possibly blocked by security checkpoint)");
        return err;
    }

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) {
        cJSON_Delete(in);
        snprintf(output, output_size, "Error: someacg list invalid JSON");
        return ESP_FAIL;
    }

    cJSON *arr = find_items_array(root);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        cJSON_Delete(in);
        snprintf(output, output_size, "Error: someacg list has no array items");
        return ESP_FAIL;
    }

    int total = cJSON_GetArraySize(arr);
    if (total <= 0) {
        cJSON_Delete(root);
        cJSON_Delete(in);
        snprintf(output, output_size, "Error: someacg list empty");
        return ESP_FAIL;
    }

    int pick = (int)(esp_random() % (uint32_t)total);
    cJSON *item = cJSON_GetArrayItem(arr, pick);
    const char *file_name = item ? get_str(item, "file_name") : "";
    const char *title = item ? get_str(item, "title") : "";

    char image_url[512] = {0};
    build_image_url(file_name, image_url, sizeof(image_url));

    if (!image_url[0]) {
        cJSON_Delete(root);
        cJSON_Delete(in);
        snprintf(output, output_size, "Error: random item has no file_name");
        return ESP_FAIL;
    }

    char chat_id[32] = {0};
    esp_err_t cid_err = resolve_chat_id(chat_id_in, chat_id, sizeof(chat_id));
    if (cid_err != ESP_OK) {
        cJSON_Delete(root);
        cJSON_Delete(in);
        snprintf(output, output_size, "Error: missing chat_id and no active Telegram context");
        return cid_err;
    }

    char cap_buf[192] = {0};
    if (caption && caption[0]) {
        snprintf(cap_buf, sizeof(cap_buf), "%s", caption);
    } else if (title && title[0]) {
        snprintf(cap_buf, sizeof(cap_buf), "%s", title);
    }

    esp_err_t send_err = telegram_send_photo(chat_id, image_url, cap_buf[0] ? cap_buf : NULL);
    cJSON_Delete(root);
    cJSON_Delete(in);

    if (send_err != ESP_OK) {
        snprintf(output, output_size, "Error: Telegram sendPhoto failed for %s", image_url);
        return send_err;
    }

    snprintf(output, output_size,
             "OK: random image sent to %s (page=%d index=%d file=%s)",
             chat_id, page, pick, file_name);
    return ESP_OK;
}

/* Backward-compatible aliases */
esp_err_t tool_pixiv_rank_execute(const char *input_json, char *output, size_t output_size)
{
    return tool_someacg_list_execute(input_json, output, output_size);
}

esp_err_t tool_pixiv_send_image_execute(const char *input_json, char *output, size_t output_size)
{
    return tool_someacg_send_image_execute(input_json, output, output_size);
}
