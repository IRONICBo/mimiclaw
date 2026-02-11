#pragma once

#include "llm_provider.h"
#include "esp_err.h"
#include "cJSON.h"

/**
 * Qwen LLM Provider
 * 
 * Implements Alibaba Qwen API using OpenAI-compatible interface
 * Supports OAuth device code flow for authentication
 */

/**
 * Initialize Qwen provider
 */
esp_err_t llm_qwen_init(void);

/**
 * Simple chat without tools
 */
esp_err_t llm_qwen_chat(const char *system_prompt, const char *messages_json,
                        char *response_buf, size_t buf_size);

/**
 * Chat with tools support
 */
esp_err_t llm_qwen_chat_tools(const char *system_prompt,
                              cJSON *messages,
                              const char *tools_json,
                              llm_response_t *resp);

/**
 * Set Qwen API key
 */
esp_err_t llm_qwen_set_api_key(const char *api_key);

/**
 * Set Qwen model
 */
esp_err_t llm_qwen_set_model(const char *model);

/**
 * Get provider name
 */
const char* llm_qwen_get_name(void);

/**
 * Get the Qwen provider interface
 */
const llm_provider_t* llm_get_qwen_provider(void);
