#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "llm.h"
#include "history.h"
#include "config.h"
#include "cJSON.h"

static char *g_api_url = NULL;
static char *g_model   = NULL;
static char *g_api_key = NULL;

/* Tools definition sent with every request */
static const char *TOOLS_JSON =
"["
"  {\"type\":\"function\",\"function\":{\"name\":\"ls\",\"description\":\"List directory contents\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Directory path (default: .)\"},\"flags\":{\"type\":\"string\",\"description\":\"Flags like -la\"}}}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"cat\",\"description\":\"Display file contents\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File to read\"}},\"required\":[\"path\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":\"Read file with optional line range\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"start_line\":{\"type\":\"integer\"},\"end_line\":{\"type\":\"integer\"}},\"required\":[\"path\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"write_file\",\"description\":\"Write content to a file\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"pwd\",\"description\":\"Print working directory\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"cd\",\"description\":\"Change directory\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"cp\",\"description\":\"Copy files\",\"parameters\":{\"type\":\"object\",\"properties\":{\"src\":{\"type\":\"string\"},\"dst\":{\"type\":\"string\"}},\"required\":[\"src\",\"dst\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"mv\",\"description\":\"Move/rename files\",\"parameters\":{\"type\":\"object\",\"properties\":{\"src\":{\"type\":\"string\"},\"dst\":{\"type\":\"string\"}},\"required\":[\"src\",\"dst\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"rm\",\"description\":\"Remove files or directories\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"recursive\":{\"type\":\"boolean\"}},\"required\":[\"path\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"mkdir\",\"description\":\"Create directories\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"parents\":{\"type\":\"boolean\"}},\"required\":[\"path\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"grep\",\"description\":\"Search file contents\",\"parameters\":{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"recursive\":{\"type\":\"boolean\"},\"ignore_case\":{\"type\":\"boolean\"}},\"required\":[\"pattern\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"head\",\"description\":\"Show first N lines of a file\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"lines\":{\"type\":\"integer\"}},\"required\":[\"path\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"wc\",\"description\":\"Count lines/words/chars\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"flags\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
"  {\"type\":\"function\",\"function\":{\"name\":\"run\",\"description\":\"Execute a shell command pipeline. Use for any command not covered by builtins.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"pipeline\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Array of commands to pipe together\"},\"stdin_file\":{\"type\":\"string\"},\"stdout_file\":{\"type\":\"string\"},\"append\":{\"type\":\"boolean\"}},\"required\":[\"pipeline\"]}}}"
"]";

static const char *SYSTEM_PROMPT =
    "You are llmsh, a natural language Unix shell. The user can type either "
    "plain English OR standard shell commands. You must determine which:\n\n"
    "1. DIRECT COMMAND: If the input looks like a standard shell command "
    "(starts with a known command name, has flags like -la, pipes |, "
    "redirections >, etc.), execute it directly via the 'run' tool exactly "
    "as typed. Do not modify or interpret it. Pass the entire command line "
    "as a single pipeline entry, splitting only on pipes (|).\n\n"
    "2. NATURAL LANGUAGE: If the input is English describing what the user "
    "wants, translate their intent into the appropriate tool calls.\n\n"
    "3. AMBIGUOUS: If you cannot tell whether input is a command or English "
    "(e.g., a single word like 'make' could be the build tool or a request), "
    "ask the user to clarify.\n\n"
    "You have built-in tools for common file operations (ls, cat, cp, mv, rm, "
    "mkdir, grep, head, wc, read_file, write_file, cd, pwd) and a 'run' tool "
    "for arbitrary shell command pipelines.\n\n"
    "Rules:\n"
    "- Prefer built-in tools over 'run' when possible for natural language.\n"
    "- For direct commands, always use 'run' to preserve exact behavior.\n"
    "- Use 'run' with pipeline array for pipes: [\"grep -r TODO\", \"wc -l\"].\n"
    "- Be concise in text responses. Show results, not explanations.\n"
    "- For destructive operations, the safety system will handle confirmation.";

/* curl write callback */
struct curl_buf {
    char *data;
    size_t size;
};

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct curl_buf *buf = userdata;
    size_t total = size * nmemb;
    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

int llm_init(const char *api_url, const char *model, const char *api_key)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_api_url = strdup(api_url);
    g_model   = strdup(model);
    g_api_key = api_key ? strdup(api_key) : NULL;
    return 0;
}

void llm_cleanup(void)
{
    free(g_api_url);
    free(g_model);
    free(g_api_key);
    curl_global_cleanup();
}

llm_response_t *llm_chat(const char *user_input, const char *cwd,
                          const char *last_output,
                          const char *matched_cmds, int first_word_is_cmd)
{
    (void)user_input; /* already in history */

    /* Build context-aware system prompt */
    size_t sys_len = strlen(SYSTEM_PROMPT) + strlen(cwd) + 512;
    if (last_output) sys_len += strlen(last_output);
    if (matched_cmds) sys_len += strlen(matched_cmds) + 256;

    char *sys_buf = malloc(sys_len);
    int off = snprintf(sys_buf, sys_len,
             "%s\n\nCurrent directory: %s\n",
             SYSTEM_PROMPT, cwd);

    if (matched_cmds) {
        off += snprintf(sys_buf + off, sys_len - off,
                 "\nThe following words in the user's input match installed "
                 "system commands: %s\n", matched_cmds);
        if (first_word_is_cmd) {
            off += snprintf(sys_buf + off, sys_len - off,
                     "NOTE: The FIRST word is a known command — this is very "
                     "likely a direct shell command. Execute it via 'run'.\n");
        }
    }

    if (last_output) {
        snprintf(sys_buf + off, sys_len - off,
                 "\nLast command output (truncated):\n%s\n", last_output);
    }

    /* Build messages array using history */
    char *messages_json = history_build_messages(sys_buf);
    free(sys_buf);
    if (!messages_json)
        return NULL;

    /* Build request body */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", g_model);

    /* Parse and add messages */
    cJSON *msgs = cJSON_Parse(messages_json);
    free(messages_json);
    if (msgs)
        cJSON_AddItemToObject(req, "messages", msgs);

    /* Parse and add tools */
    cJSON *tools = cJSON_Parse(TOOLS_JSON);
    if (tools)
        cJSON_AddItemToObject(req, "tools", tools);

    cJSON_AddNumberToObject(req, "max_tokens", LLMSH_MAX_TOKENS);

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    /* HTTP request */
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(body);
        return NULL;
    }

    struct curl_buf response = {NULL, 0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    if (g_api_key) {
        char auth[512];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", g_api_key);
        headers = curl_slist_append(headers, auth);
    }

    curl_easy_setopt(curl, CURLOPT_URL, g_api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    if (res != CURLE_OK) {
        fprintf(stderr, "llmsh: curl error: %s\n", curl_easy_strerror(res));
        free(response.data);
        return NULL;
    }

    /* Parse response */
    cJSON *json = cJSON_Parse(response.data);
    free(response.data);
    if (!json) {
        fprintf(stderr, "llmsh: failed to parse LLM response\n");
        return NULL;
    }

    llm_response_t *result = calloc(1, sizeof(*result));

    /* Extract from choices[0].message */
    cJSON *choices = cJSON_GetObjectItem(json, "choices");
    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;

    if (message) {
        /* Text content */
        cJSON *content = cJSON_GetObjectItem(message, "content");
        if (content && cJSON_IsString(content) && content->valuestring[0])
            result->text = strdup(content->valuestring);

        /* Tool calls */
        cJSON *tcs = cJSON_GetObjectItem(message, "tool_calls");
        if (tcs && cJSON_IsArray(tcs)) {
            int n = cJSON_GetArraySize(tcs);
            result->tool_calls = calloc(n, sizeof(tool_call_t));
            result->num_tool_calls = n;

            for (int i = 0; i < n; i++) {
                cJSON *tc = cJSON_GetArrayItem(tcs, i);
                cJSON *fn = cJSON_GetObjectItem(tc, "function");
                if (fn) {
                    cJSON *name = cJSON_GetObjectItem(fn, "name");
                    cJSON *args = cJSON_GetObjectItem(fn, "arguments");
                    if (name && cJSON_IsString(name))
                        result->tool_calls[i].name = strdup(name->valuestring);
                    if (args && cJSON_IsString(args))
                        result->tool_calls[i].arguments = strdup(args->valuestring);
                }
            }
        }
    }

    cJSON_Delete(json);
    return result;
}

void llm_response_free(llm_response_t *resp)
{
    if (!resp) return;
    free(resp->text);
    for (int i = 0; i < resp->num_tool_calls; i++) {
        free(resp->tool_calls[i].name);
        free(resp->tool_calls[i].arguments);
    }
    free(resp->tool_calls);
    free(resp);
}
