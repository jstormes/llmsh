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

/* ── Shared helpers ──────────────────────────────────────────────── */

/* Build the system prompt with context */
static char *build_system_prompt(const char *cwd, const char *last_output,
                                  const char *matched_cmds, int first_word_is_cmd)
{
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

    return sys_buf;
}

/* Build the JSON request body. Caller frees returned string. */
static char *build_request_body(const char *sys_prompt, int streaming)
{
    char *messages_json = history_build_messages(sys_prompt);
    if (!messages_json) return NULL;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", g_model);

    cJSON *msgs = cJSON_Parse(messages_json);
    free(messages_json);
    if (msgs) cJSON_AddItemToObject(req, "messages", msgs);

    cJSON *tools = cJSON_Parse(TOOLS_JSON);
    if (tools) cJSON_AddItemToObject(req, "tools", tools);

    cJSON_AddNumberToObject(req, "max_tokens", LLMSH_MAX_TOKENS);

    if (streaming)
        cJSON_AddBoolToObject(req, "stream", 1);

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

/* Build curl headers. Caller frees with curl_slist_free_all. */
static struct curl_slist *build_headers(void)
{
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (g_api_key) {
        char auth[512];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", g_api_key);
        headers = curl_slist_append(headers, auth);
    }
    return headers;
}

/* ── Non-streaming (original) ────────────────────────────────────── */

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

static llm_response_t *parse_full_response(const char *json_str)
{
    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        fprintf(stderr, "llmsh: failed to parse LLM response\n");
        return NULL;
    }

    llm_response_t *result = calloc(1, sizeof(*result));

    cJSON *choices = cJSON_GetObjectItem(json, "choices");
    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;

    if (message) {
        cJSON *content = cJSON_GetObjectItem(message, "content");
        if (content && cJSON_IsString(content) && content->valuestring[0])
            result->text = strdup(content->valuestring);

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
    (void)user_input;

    char *sys_prompt = build_system_prompt(cwd, last_output, matched_cmds, first_word_is_cmd);
    char *body = build_request_body(sys_prompt, 0);
    free(sys_prompt);
    if (!body) return NULL;

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return NULL; }

    struct curl_buf response = {NULL, 0};
    struct curl_slist *headers = build_headers();

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

    llm_response_t *result = parse_full_response(response.data);
    free(response.data);
    return result;
}

/* ── SSE Streaming ───────────────────────────────────────────────── */

typedef struct {
    /* Line buffer for accumulating partial SSE lines */
    char *line_buf;
    size_t line_len;
    size_t line_cap;

    /* Accumulated full text */
    char *full_text;
    size_t text_len;
    size_t text_cap;

    /* Tool call accumulation (streamed as indexed fragments) */
    tool_call_t tool_calls[LLMSH_MAX_STREAM_TOOL_CALLS];
    /* Argument buffers (accumulated across chunks) */
    char *tc_args[LLMSH_MAX_STREAM_TOOL_CALLS];
    size_t tc_args_len[LLMSH_MAX_STREAM_TOOL_CALLS];
    size_t tc_args_cap[LLMSH_MAX_STREAM_TOOL_CALLS];
    int num_tool_calls;

    /* Callback */
    llm_token_cb token_cb;
    void *userdata;
} sse_state_t;

static void sse_state_init(sse_state_t *st, llm_token_cb cb, void *ud)
{
    memset(st, 0, sizeof(*st));
    st->token_cb = cb;
    st->userdata = ud;
}

static void sse_state_cleanup(sse_state_t *st)
{
    free(st->line_buf);
    /* full_text and tool_calls are transferred to response, don't free here */
}

static void sse_append_text(sse_state_t *st, const char *s, size_t len)
{
    if (st->text_len + len + 1 > st->text_cap) {
        st->text_cap = (st->text_cap == 0) ? 1024 : st->text_cap * 2;
        while (st->text_len + len + 1 > st->text_cap)
            st->text_cap *= 2;
        st->full_text = realloc(st->full_text, st->text_cap);
    }
    memcpy(st->full_text + st->text_len, s, len);
    st->text_len += len;
    st->full_text[st->text_len] = '\0';
}

static void sse_append_tc_args(sse_state_t *st, int idx, const char *s, size_t len)
{
    if (idx < 0 || idx >= LLMSH_MAX_STREAM_TOOL_CALLS) return;
    size_t *al = &st->tc_args_len[idx];
    size_t *ac = &st->tc_args_cap[idx];
    if (*al + len + 1 > *ac) {
        *ac = (*ac == 0) ? 256 : *ac * 2;
        while (*al + len + 1 > *ac) *ac *= 2;
        st->tc_args[idx] = realloc(st->tc_args[idx], *ac);
    }
    memcpy(st->tc_args[idx] + *al, s, len);
    *al += len;
    st->tc_args[idx][*al] = '\0';
}

/* Process a single SSE data line (everything after "data: ") */
static void sse_process_data(sse_state_t *st, const char *data)
{
    /* End of stream */
    if (strcmp(data, "[DONE]") == 0)
        return;

    cJSON *json = cJSON_Parse(data);
    if (!json) return;

    cJSON *choices = cJSON_GetObjectItem(json, "choices");
    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    cJSON *delta = choice0 ? cJSON_GetObjectItem(choice0, "delta") : NULL;

    if (delta) {
        /* Text content delta */
        cJSON *content = cJSON_GetObjectItem(delta, "content");
        if (content && cJSON_IsString(content) && content->valuestring[0]) {
            const char *tok = content->valuestring;
            size_t toklen = strlen(tok);
            sse_append_text(st, tok, toklen);
            if (st->token_cb)
                st->token_cb(tok, st->userdata);
        }

        /* Tool call deltas */
        cJSON *tcs = cJSON_GetObjectItem(delta, "tool_calls");
        if (tcs && cJSON_IsArray(tcs)) {
            int n = cJSON_GetArraySize(tcs);
            for (int i = 0; i < n; i++) {
                cJSON *tc = cJSON_GetArrayItem(tcs, i);

                /* Get index */
                cJSON *idx_j = cJSON_GetObjectItem(tc, "index");
                int idx = idx_j ? idx_j->valueint : st->num_tool_calls;
                if (idx >= LLMSH_MAX_STREAM_TOOL_CALLS) continue;

                /* Track how many tool calls we have */
                if (idx >= st->num_tool_calls)
                    st->num_tool_calls = idx + 1;

                /* Function name (first chunk for this tool call) */
                cJSON *fn = cJSON_GetObjectItem(tc, "function");
                if (fn) {
                    cJSON *name = cJSON_GetObjectItem(fn, "name");
                    if (name && cJSON_IsString(name) && !st->tool_calls[idx].name)
                        st->tool_calls[idx].name = strdup(name->valuestring);

                    /* Arguments fragment */
                    cJSON *args = cJSON_GetObjectItem(fn, "arguments");
                    if (args && cJSON_IsString(args)) {
                        size_t alen = strlen(args->valuestring);
                        if (alen > 0)
                            sse_append_tc_args(st, idx, args->valuestring, alen);
                    }
                }
            }
        }
    }

    cJSON_Delete(json);
}

/* Process buffered data, splitting on newlines and handling SSE lines */
static void sse_process_buffer(sse_state_t *st)
{
    size_t start = 0;
    for (size_t i = 0; i < st->line_len; i++) {
        if (st->line_buf[i] == '\n') {
            st->line_buf[i] = '\0';
            char *line = st->line_buf + start;

            /* Skip empty lines (SSE keepalives) */
            if (line[0] != '\0') {
                if (strncmp(line, "data: ", 6) == 0)
                    sse_process_data(st, line + 6);
                else if (strncmp(line, "data:", 5) == 0)
                    sse_process_data(st, line + 5);
                /* Ignore other SSE fields (event:, id:, retry:) */
            }

            start = i + 1;
        }
    }

    /* Keep unprocessed bytes (incomplete line) */
    if (start > 0 && start < st->line_len) {
        memmove(st->line_buf, st->line_buf + start, st->line_len - start);
        st->line_len -= start;
    } else if (start == st->line_len) {
        st->line_len = 0;
    }
}

static size_t curl_sse_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    sse_state_t *st = userdata;
    size_t total = size * nmemb;

    /* Append to line buffer */
    if (st->line_len + total + 1 > st->line_cap) {
        st->line_cap = (st->line_cap == 0) ? LLMSH_SSE_BUF_SIZE : st->line_cap * 2;
        while (st->line_len + total + 1 > st->line_cap)
            st->line_cap *= 2;
        st->line_buf = realloc(st->line_buf, st->line_cap);
    }
    memcpy(st->line_buf + st->line_len, ptr, total);
    st->line_len += total;
    st->line_buf[st->line_len] = '\0';

    /* Process complete lines */
    sse_process_buffer(st);

    return total;
}

llm_response_t *llm_chat_stream(const char *user_input, const char *cwd,
                                 const char *last_output,
                                 const char *matched_cmds, int first_word_is_cmd,
                                 llm_token_cb token_cb, void *userdata)
{
    /* Fall back to non-streaming if no callback */
    if (!token_cb)
        return llm_chat(user_input, cwd, last_output, matched_cmds, first_word_is_cmd);

    (void)user_input;

    char *sys_prompt = build_system_prompt(cwd, last_output, matched_cmds, first_word_is_cmd);
    char *body = build_request_body(sys_prompt, 1);
    free(sys_prompt);
    if (!body) return NULL;

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return NULL; }

    sse_state_t sse;
    sse_state_init(&sse, token_cb, userdata);

    struct curl_slist *headers = build_headers();

    curl_easy_setopt(curl, CURLOPT_URL, g_api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_sse_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sse);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L); /* longer timeout for streaming */

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    if (res != CURLE_OK) {
        fprintf(stderr, "llmsh: curl error: %s\n", curl_easy_strerror(res));
        sse_state_cleanup(&sse);
        return NULL;
    }

    /* Build response from accumulated state */
    llm_response_t *result = calloc(1, sizeof(*result));

    if (sse.full_text && sse.text_len > 0)
        result->text = sse.full_text;  /* transfer ownership */
    else
        free(sse.full_text);

    if (sse.num_tool_calls > 0) {
        result->num_tool_calls = sse.num_tool_calls;
        result->tool_calls = calloc(sse.num_tool_calls, sizeof(tool_call_t));
        for (int i = 0; i < sse.num_tool_calls; i++) {
            result->tool_calls[i].name = sse.tool_calls[i].name; /* transfer */
            result->tool_calls[i].arguments = sse.tc_args[i];    /* transfer */
        }
    }

    sse_state_cleanup(&sse);
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
