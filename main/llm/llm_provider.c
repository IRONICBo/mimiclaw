#include "llm_provider.h"
#include <stdlib.h>
#include <string.h>

void llm_response_init(llm_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));
}

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

esp_err_t llm_response_add_tool_call(llm_response_t *resp, const char *id, 
                                     const char *name, const char *input_json)
{
    /* Grow array if needed */
    if (resp->call_count >= resp->call_capacity) {
        int new_cap = resp->call_capacity == 0 ? 4 : resp->call_capacity * 2;
        llm_tool_call_t *new_calls = realloc(resp->calls, new_cap * sizeof(llm_tool_call_t));
        if (!new_calls) return ESP_ERR_NO_MEM;
        resp->calls = new_calls;
        resp->call_capacity = new_cap;
    }
    
    llm_tool_call_t *call = &resp->calls[resp->call_count];
    memset(call, 0, sizeof(*call));
    
    if (id) {
        strncpy(call->id, id, sizeof(call->id) - 1);
    }
    if (name) {
        strncpy(call->name, name, sizeof(call->name) - 1);
    }
    if (input_json) {
        call->input = strdup(input_json);
        call->input_len = strlen(input_json);
    }
    
    resp->call_count++;
    return ESP_OK;
}
