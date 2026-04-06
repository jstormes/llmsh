#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "history.h"
#include "config.h"
#include "cJSON.h"

typedef struct {
    char *role;     /* "system", "user", "assistant", "tool" */
    char *content;
} message_t;

static message_t *messages = NULL;
static int msg_count = 0;
static int msg_cap = 0;
static int msg_max = 0;  /* max before eviction (LLMSH_MAX_HISTORY * 3) */

void history_init(void)
{
    msg_count = 0;
    msg_max = LLMSH_MAX_HISTORY * 3;
    if (!messages) {
        msg_cap = msg_max;
        messages = calloc(msg_cap, sizeof(message_t));
    }
}

void history_cleanup(void)
{
    for (int i = 0; i < msg_count; i++) {
        free(messages[i].role);
        free(messages[i].content);
        messages[i].role = NULL;
        messages[i].content = NULL;
    }
    msg_count = 0;
}

static void add_message(const char *role, const char *content)
{
    if (!role || !content) return;

    /* Evict oldest if at capacity */
    if (msg_count >= msg_max) {
        free(messages[0].role);
        free(messages[0].content);
        memmove(&messages[0], &messages[1], (msg_count - 1) * sizeof(message_t));
        msg_count--;
    }

    /* Grow if needed (shouldn't happen after init, but be safe) */
    if (msg_count >= msg_cap) {
        msg_cap = (msg_cap == 0) ? 64 : msg_cap * 2;
        messages = realloc(messages, msg_cap * sizeof(message_t));
        if (!messages) return; /* OOM */
    }

    char *r = strdup(role);
    char *c = strdup(content);
    if (!r || !c) { free(r); free(c); return; } /* OOM */

    messages[msg_count].role = r;
    messages[msg_count].content = c;
    msg_count++;
}

void history_add_user(const char *text)
{
    add_message("user", text);
}

void history_add_assistant(const char *text)
{
    add_message("assistant", text);
}

void history_add_tool_result(const char *tool_name, const char *result)
{
    if (!tool_name || !result) return;

    /* Truncate large outputs */
    char buf[LLMSH_MAX_OUTPUT_CAPTURE];
    if (strlen(result) >= sizeof(buf)) {
        memcpy(buf, result, sizeof(buf) - 20);
        strcpy(buf + sizeof(buf) - 20, "\n[truncated]");
        add_message("user", buf);
    } else {
        size_t len = strlen(tool_name) + strlen(result) + 32;
        char *msg = malloc(len);
        if (!msg) return;
        snprintf(msg, len, "[%s output]:\n%s", tool_name, result);
        add_message("user", msg);
        free(msg);
    }
}

char *history_build_messages(const char *system_prompt)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    /* System message first */
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", system_prompt);
    cJSON_AddItemToArray(arr, sys);

    /* Add history */
    for (int i = 0; i < msg_count; i++) {
        if (!messages[i].role || !messages[i].content) continue;
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", messages[i].role);
        cJSON_AddStringToObject(msg, "content", messages[i].content);
        cJSON_AddItemToArray(arr, msg);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}
