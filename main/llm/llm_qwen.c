#include "llm_qwen.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "llm_qwen";

static char s_api_key[256] = {0};
static char s_model[64] = MIMI_QWEN_DEFAULT_MODEL;

/* Response buffer */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t resp_buf_init(resp_buf_t *rb, size_t initial_cap)
{
    rb->data = heap_caps_calloc(1, initial_cap, MALLOC_CAP_SPIRAM);
    if (!rb->data) return ESP_ERR_NO_MEM;
    rb->len = 0;
    rb->cap = initial_cap;
    return ESP_OK;
}

static esp_err_t resp_buf_append(resp_buf_t *rb, const char *data, size_t len)
{
    while (rb->len + len >= rb->cap) {
        size_t new_cap = rb->cap * 2;
        char *tmp = heap_caps_realloc(rb->data, new_cap, MALLOC_CAP_SPIRAM);
        if (!tmp) return ESP_ERR_NO_MEM;
        rb->data = tmp;
        rb->cap = new_cap;
    }
    memcpy(rb->data + rb->len, data, len);
    rb->len += len;
    rb->data[rb->len] = '\0';
    return ESP_OK;
}

static void resp_buf_free(resp_buf_t *rb)
{
    free(rb->data);
    rb->data = NULL;
    rb->len = 0;
    rb->cap = 0;
}

/* HTTP event handler */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        resp_buf_append(rb, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

/* Initialize Qwen provider */
esp_err_t llm_qwen_init(void)
{
    /* Start with build-time defaults */
    if (MIMI_SECRET_QWEN_API_KEY[0] != '\0') {
        strncpy(s_api_key, MIMI_SECRET_QWEN_API_KEY, sizeof(s_api_key) - 1);
    }
    if (MIMI_SECRET_QWEN_MODEL[0] != '\0') {
        strncpy(s_model, MIMI_SECRET_QWEN_MODEL, sizeof(s_model) - 1);
    }

    /* NVS overrides take highest priority */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_QWEN, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[256] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_QWEN_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_api_key, tmp, sizeof(s_api_key) - 1);
        }
        len = sizeof(tmp);
        memset(tmp, 0, sizeof(tmp));
        if (nvs_get_str(nvs, MIMI_NVS_KEY_QWEN_MODEL, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_model, tmp, sizeof(s_model) - 1);
        }
        nvs_close(nvs);
    }

    if (s_api_key[0]) {
        ESP_LOGI(TAG, "Qwen LLM provider initialized (model: %s)", s_model);
    } else {
        ESP_LOGW(TAG, "No Qwen API key configured");
    }
    return ESP_OK;
}

/* HTTP call using direct method */
static esp_err_t qwen_http_direct(const char *post_data, resp_buf_t *rb, int *out_status)
{
    esp_http_client_config_t config = {
        .url = MIMI_QWEN_API_URL,
        .event_handler = http_event_handler,
        .user_data = rb,
        .timeout_ms = 120 * 1000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    
    /* Qwen uses Bearer token authentication (OpenAI-compatible) */
    char auth_header[300];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);
    
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

/* HTTP call via proxy */
static esp_err_t qwen_http_via_proxy(const char *post_data, resp_buf_t *rb, int *out_status)
{
    /* Extract host from MIMI_QWEN_API_URL */
    const char *host = "dashscope.aliyuncs.com";  /* Default Qwen API host */
    
    proxy_conn_t *conn = proxy_conn_open(host, 443, 30000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    int body_len = strlen(post_data);
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "POST /compatible-mode/v1/chat/completions HTTP/1.1\\r\\n"
        "Host: %s\\r\\n"
        "Content-Type: application/json\\r\\n"
        "Authorization: Bearer %s\\r\\n"
        "Content-Length: %d\\r\\n"
        "Connection: close\\r\\n\\r\\n",
        host, s_api_key, body_len);

    if (proxy_conn_write(conn, header, hlen) < 0 ||
        proxy_conn_write(conn, post_data, body_len) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    /* Read response */
    char tmp[4096];
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 120000);
        if (n <= 0) break;
        if (resp_buf_append(rb, tmp, n) != ESP_OK) break;
    }
    proxy_conn_close(conn);

    /* Parse status line */
    *out_status = 0;
    if (rb->len > 5 && strncmp(rb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(rb->data, ' ');
        if (sp) *out_status = atoi(sp + 1);
    }

    /* Strip HTTP headers */
    char *body = strstr(rb->data, "\\r\\n\\r\\n");
    if (body) {
        body += 4;
        size_t blen = rb->len - (body - rb->data);
        memmove(rb->data, body, blen);
        rb->len = blen;
        rb->data[rb->len] = '\0';
    }

    return ESP_OK;
}

/* Shared HTTP dispatcher */
static esp_err_t qwen_http_call(const char *post_data, resp_buf_t *rb, int *out_status)
{
    if (http_proxy_is_enabled()) {
        return qwen_http_via_proxy(post_data, rb, out_status);
    } else {
        return qwen_http_direct(post_data, rb, out_status);
    }
}

/* Extract text from OpenAI-compatible response */
static void extract_text_openai(cJSON *root, char *buf, size_t size)
{
    buf[0] = '\0';
    
    /* OpenAI format: choices[0].message.content */
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || !cJSON_IsArray(choices)) return;
    
    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    if (!choice) return;
    
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    if (!message) return;
    
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsString(content)) {
        strncpy(buf, content->valuestring, size - 1);
        buf[size - 1] = '\0';
    }
}

/* Simple chat without tools */
esp_err_t llm_qwen_chat(const char *system_prompt, const char *messages_json,
                        char *response_buf, size_t buf_size)
{
    if (s_api_key[0] == '\0') {
        snprintf(response_buf, buf_size, "Error: No Qwen API key configured");
        return ESP_ERR_INVALID_STATE;
    }

    /* Build OpenAI-compatible request */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", s_model);
    cJSON_AddNumberToObject(body, "max_tokens", MIMI_QWEN_MAX_TOKENS);
    
    /* Parse or create messages array */
    cJSON *messages = cJSON_Parse(messages_json);
    if (!messages) {
        /* Create simple user message */
        messages = cJSON_CreateArray();
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", messages_json);
        cJSON_AddItemToArray(messages, msg);
    }
    
    /* Prepend system message if provided */
    if (system_prompt && system_prompt[0]) {
        cJSON *sys_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_msg, "role", "system");
        cJSON_AddStringToObject(sys_msg, "content", system_prompt);
        
        /* Insert at beginning */
        cJSON *new_messages = cJSON_CreateArray();
        cJSON_AddItemToArray(new_messages, sys_msg);
        
        cJSON *item;
        cJSON_ArrayForEach(item, messages) {
            cJSON_AddItemToArray(new_messages, cJSON_Duplicate(item, 1));
        }
        cJSON_Delete(messages);
        messages = new_messages;
    }
    
    cJSON_AddItemToObject(body, "messages", messages);

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) {
        snprintf(response_buf, buf_size, "Error: Failed to build request");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Calling Qwen API (model: %s, body: %d bytes)",
             s_model, (int)strlen(post_data));

    resp_buf_t rb;
    if (resp_buf_init(&rb, MIMI_QWEN_STREAM_BUF_SIZE) != ESP_OK) {
        free(post_data);
        snprintf(response_buf, buf_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = qwen_http_call(post_data, &rb, &status);
    free(post_data);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        resp_buf_free(&rb);
        snprintf(response_buf, buf_size, "Error: HTTP request failed (%s)",
                 esp_err_to_name(err));
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "API returned status %d", status);
        snprintf(response_buf, buf_size, "API error (HTTP %d): %.200s",
                 status, rb.data ? rb.data : "");
        resp_buf_free(&rb);
        return ESP_FAIL;
    }

    /* Parse JSON response */
    cJSON *root = cJSON_Parse(rb.data);
    resp_buf_free(&rb);

    if (!root) {
        snprintf(response_buf, buf_size, "Error: Failed to parse response");
        return ESP_FAIL;
    }

    extract_text_openai(root, response_buf, buf_size);
    cJSON_Delete(root);

    if (response_buf[0] == '\0') {
        snprintf(response_buf, buf_size, "No response from Qwen API");
    } else {
        ESP_LOGI(TAG, "Qwen response: %d bytes", (int)strlen(response_buf));
    }

    return ESP_OK;
}

/* Chat with tools support */
esp_err_t llm_qwen_chat_tools(const char *system_prompt,
                              cJSON *messages,
                              const char *tools_json,
                              llm_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));

    if (s_api_key[0] == '\0') return ESP_ERR_INVALID_STATE;

    /* Build OpenAI-compatible request */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", s_model);
    cJSON_AddNumberToObject(body, "max_tokens", MIMI_QWEN_MAX_TOKENS);

    /* Deep-copy messages */
    cJSON *msgs_copy = cJSON_Duplicate(messages, 1);
    
    /* Prepend system message if provided */
    if (system_prompt && system_prompt[0]) {
        cJSON *sys_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_msg, "role", "system");
        cJSON_AddStringToObject(sys_msg, "content", system_prompt);
        
        cJSON *new_messages = cJSON_CreateArray();
        cJSON_AddItemToArray(new_messages, sys_msg);
        
        cJSON *item;
        cJSON_ArrayForEach(item, msgs_copy) {
            cJSON_AddItemToArray(new_messages, cJSON_Duplicate(item, 1));
        }
        cJSON_Delete(msgs_copy);
        msgs_copy = new_messages;
    }
    
    cJSON_AddItemToObject(body, "messages", msgs_copy);

    /* Add tools if provided (OpenAI format) */
    if (tools_json) {
        cJSON *tools = cJSON_Parse(tools_json);
        if (tools) {
            cJSON_AddItemToObject(body, "tools", tools);
        }
    }

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Calling Qwen API with tools (model: %s, body: %d bytes)",
             s_model, (int)strlen(post_data));

    /* HTTP call */
    resp_buf_t rb;
    if (resp_buf_init(&rb, MIMI_QWEN_STREAM_BUF_SIZE) != ESP_OK) {
        free(post_data);
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = qwen_http_call(post_data, &rb, &status);
    free(post_data);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        resp_buf_free(&rb);
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "API error %d: %.500s", status, rb.data ? rb.data : "");
        resp_buf_free(&rb);
        return ESP_FAIL;
    }

    /* Parse JSON response */
    cJSON *root = cJSON_Parse(rb.data);
    resp_buf_free(&rb);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse API response JSON");
        return ESP_FAIL;
    }

    /* Extract content from choices[0].message */
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (choices && cJSON_IsArray(choices)) {
        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        if (choice) {
            cJSON *message = cJSON_GetObjectItem(choice, "message");
            if (message) {
                /* Extract text content */
                cJSON *content = cJSON_GetObjectItem(message, "content");
                if (content && cJSON_IsString(content) && content->valuestring[0]) {
                    size_t len = strlen(content->valuestring);
                    resp->text = calloc(1, len + 1);
                    if (resp->text) {
                        memcpy(resp->text, content->valuestring, len);
                        resp->text_len = len;
                    }
                }
                
                /* Extract tool calls (OpenAI format) */
                cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
                if (tool_calls && cJSON_IsArray(tool_calls)) {
                    resp->tool_use = true;
                    
                    cJSON *call_item;
                    cJSON_ArrayForEach(call_item, tool_calls) {
                        if (resp->call_count >= MIMI_MAX_TOOL_CALLS) break;
                        
                        cJSON *func = cJSON_GetObjectItem(call_item, "function");
                        if (!func) continue;
                        
                        /* Allocate tool call */
                        if (resp->call_count >= resp->call_capacity) {
                            int new_cap = resp->call_capacity == 0 ? 4 : resp->call_capacity * 2;
                            llm_tool_call_t *new_calls = realloc(resp->calls, 
                                                                  new_cap * sizeof(llm_tool_call_t));
                            if (!new_calls) break;
                            resp->calls = new_calls;
                            resp->call_capacity = new_cap;
                        }
                        
                        llm_tool_call_t *call = &resp->calls[resp->call_count];
                        memset(call, 0, sizeof(*call));
                        
                        /* Extract call ID */
                        cJSON *id = cJSON_GetObjectItem(call_item, "id");
                        if (id && cJSON_IsString(id)) {
                            strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
                        }
                        
                        /* Extract function name */
                        cJSON *name = cJSON_GetObjectItem(func, "name");
                        if (name && cJSON_IsString(name)) {
                            strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
                        }
                        
                        /* Extract arguments (as JSON string) */
                        cJSON *args = cJSON_GetObjectItem(func, "arguments");
                        if (args && cJSON_IsString(args)) {
                            call->input = strdup(args->valuestring);
                            call->input_len = strlen(args->valuestring);
                        }
                        
                        resp->call_count++;
                    }
                }
            }
            
            /* Check finish_reason */
            cJSON *finish_reason = cJSON_GetObjectItem(choice, "finish_reason");
            if (finish_reason && cJSON_IsString(finish_reason)) {
                if (strcmp(finish_reason->valuestring, "tool_calls") == 0) {
                    resp->tool_use = true;
                }
            }
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Response: %d bytes text, %d tool calls, stop=%s",
             (int)resp->text_len, resp->call_count,
             resp->tool_use ? "tool_calls" : "stop");

    return ESP_OK;
}

/* Set API key */
esp_err_t llm_qwen_set_api_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_QWEN, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_QWEN_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_api_key, api_key, sizeof(s_api_key) - 1);
    ESP_LOGI(TAG, "Qwen API key saved");
    return ESP_OK;
}

/* Set model */
esp_err_t llm_qwen_set_model(const char *model)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_QWEN, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_QWEN_MODEL, model));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_model, model, sizeof(s_model) - 1);
    ESP_LOGI(TAG, "Qwen model set to: %s", s_model);
    return ESP_OK;
}

/* Get provider name */
const char* llm_qwen_get_name(void)
{
    return "qwen";
}

/* Provider interface */
static const llm_provider_t s_qwen_provider = {
    .type = LLM_PROVIDER_QWEN,
    .init = llm_qwen_init,
    .chat = llm_qwen_chat,
    .chat_tools = llm_qwen_chat_tools,
    .set_api_key = llm_qwen_set_api_key,
    .set_model = llm_qwen_set_model,
    .get_name = llm_qwen_get_name,
};

const llm_provider_t* llm_get_qwen_provider(void)
{
    return &s_qwen_provider;
}
