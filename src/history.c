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

static message_t messages[LLMSH_MAX_HISTORY * 3]; /* room for user + assistant + tool */
static int msg_count = 0;

void history_init(void)
{
    msg_count = 0;
}

void history_cleanup(void)
{
    for (int i = 0; i < msg_count; i++) {
        free(messages[i].role);
        free(messages[i].content);
    }
    msg_count = 0;
}

static void add_message(const char *role, const char *content)
{
    int max = LLMSH_MAX_HISTORY * 3;

    /* Evict oldest if full (keep at least system prompt space) */
    if (msg_count >= max) {
        free(messages[0].role);
        free(messages[0].content);
        memmove(&messages[0], &messages[1], (max - 1) * sizeof(message_t));
        msg_count = max - 1;
    }

    messages[msg_count].role = strdup(role);
    messages[msg_count].content = strdup(content);
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
    /* Truncate large outputs */
    char buf[LLMSH_MAX_OUTPUT_CAPTURE];
    if (strlen(result) >= sizeof(buf)) {
        memcpy(buf, result, sizeof(buf) - 20);
        strcpy(buf + sizeof(buf) - 20, "\n[truncated]");
        add_message("user", buf); /* tool results sent as user context */
    } else {
        char *msg = malloc(strlen(tool_name) + strlen(result) + 32);
        sprintf(msg, "[%s output]:\n%s", tool_name, result);
        add_message("user", msg);
        free(msg);
    }
}

char *history_build_messages(const char *system_prompt)
{
    cJSON *arr = cJSON_CreateArray();

    /* System message first */
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", system_prompt);
    cJSON_AddItemToArray(arr, sys);

    /* Add history */
    for (int i = 0; i < msg_count; i++) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", messages[i].role);
        cJSON_AddStringToObject(msg, "content", messages[i].content);
        cJSON_AddItemToArray(arr, msg);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}
