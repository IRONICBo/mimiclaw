#pragma once

#ifndef LLM_PROVIDER_H
#define LLM_PROVIDER_H

#include "esp_err.h"
#include "cJSON.h"
#include <stddef.h>
#include <stdbool.h>

/* Import types from llm_proxy.h to avoid duplication */
#include "llm_proxy.h"

/**
 * LLM Provider Abstraction Layer
 * 
 * Supports multiple LLM providers:
 * - Anthropic Claude (Messages API)
 * - Alibaba Qwen (OpenAI-compatible API)
 */

/* Provider type enum */
typedef enum {
    LLM_PROVIDER_CLAUDE = 0,
    LLM_PROVIDER_QWEN = 1,
} llm_provider_type_t;

/* Provider interface */
typedef struct llm_provider {
    llm_provider_type_t type;
    
    /**
     * Initialize the provider
     */
    esp_err_t (*init)(void);
    
    /**
     * Simple chat (no tools, backward compatible)
     */
    esp_err_t (*chat)(const char *system_prompt, const char *messages_json,
                      char *response_buf, size_t buf_size);
    
    /**
     * Chat with tools support
     */
    esp_err_t (*chat_tools)(const char *system_prompt,
                           cJSON *messages,
                           const char *tools_json,
                           llm_response_t *resp);
    
    /**
     * Set API key
     */
    esp_err_t (*set_api_key)(const char *api_key);
    
    /**
     * Set model
     */
    esp_err_t (*set_model)(const char *model);
    
    /**
     * Get provider name
     */
    const char* (*get_name)(void);
    
} llm_provider_t;

/* Provider registration */
const llm_provider_t* llm_get_claude_provider(void);
const llm_provider_t* llm_get_qwen_provider(void);

#endif /* LLM_PROVIDER_H */
