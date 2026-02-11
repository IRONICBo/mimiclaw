#include "llm_proxy.h"
#include "llm_provider.h"
#include "mimi_config.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "llm_proxy";

/* Active provider */
static const llm_provider_t *s_provider = NULL;

/* Forward declarations for provider getters */
extern const llm_provider_t* llm_get_claude_provider(void);
extern const llm_provider_t* llm_get_qwen_provider(void);

/* Initialize LLM proxy */
esp_err_t llm_proxy_init(void)
{
    /* Select provider based on configuration */
#if MIMI_LLM_PROVIDER == 1
    s_provider = llm_get_qwen_provider();
    ESP_LOGI(TAG, "Using Qwen LLM provider");
#else
    s_provider = llm_get_claude_provider();
    ESP_LOGI(TAG, "Using Claude LLM provider");
#endif

    if (s_provider && s_provider->init) {
        return s_provider->init();
    }
    
    ESP_LOGW(TAG, "No LLM provider available");
    return ESP_OK;
}

/* Simple chat */
esp_err_t llm_chat(const char *system_prompt, const char *messages_json,
                   char *response_buf, size_t buf_size)
{
    if (!s_provider || !s_provider->chat) {
        snprintf(response_buf, buf_size, "Error: No LLM provider available");
        return ESP_ERR_INVALID_STATE;
    }
    
    return s_provider->chat(system_prompt, messages_json, response_buf, buf_size);
}

/* Chat with tools */
esp_err_t llm_chat_tools(const char *system_prompt,
                         cJSON *messages,
                         const char *tools_json,
                         llm_response_t *resp)
{
    if (!s_provider || !s_provider->chat_tools) {
        memset(resp, 0, sizeof(*resp));
        return ESP_ERR_INVALID_STATE;
    }
    
    return s_provider->chat_tools(system_prompt, messages, tools_json, resp);
}

/* Response management */
void llm_response_free(llm_response_t *resp)
{
    free(resp->text);
    resp->text = NULL;
    resp->text_len = 0;
    
    for (int i = 0; i < resp->call_count; i++) {
        free(resp->calls[i].input);
    }
    free(resp->calls);
    resp->calls = NULL;
    resp->call_count = 0;
    resp->call_capacity = 0;
    resp->tool_use = false;
}

/* Set API key */
esp_err_t llm_set_api_key(const char *api_key)
{
    if (!s_provider || !s_provider->set_api_key) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_provider->set_api_key(api_key);
}

/* Set model */
esp_err_t llm_set_model(const char *model)
{
    if (!s_provider || !s_provider->set_model) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_provider->set_model(model);
}
