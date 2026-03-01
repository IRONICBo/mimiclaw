#pragma once

#include "esp_err.h"
#include <stddef.h>

/* Backward-compatible implementation names; now powered by someacg APIs. */
esp_err_t tool_pixiv_rank_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_pixiv_send_image_execute(const char *input_json, char *output, size_t output_size);

/* Preferred tool aliases exposed in registry. */
esp_err_t tool_someacg_list_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_someacg_send_image_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_someacg_send_random_execute(const char *input_json, char *output, size_t output_size);
