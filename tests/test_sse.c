#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "test.h"
#include "llm.h"

/* Required by llm.o */
volatile sig_atomic_t interrupted = 0;

/* Accumulator for testing streaming callbacks */
static char token_buf[4096];
static int token_pos = 0;
static char think_buf[4096];
static int think_pos = 0;

static void reset_bufs(void)
{
    token_pos = 0; token_buf[0] = '\0';
    think_pos = 0; think_buf[0] = '\0';
}

static void test_token_cb(const char *token, void *ud)
{
    (void)ud;
    int len = strlen(token);
    if (token_pos + len < (int)sizeof(token_buf)) {
        memcpy(token_buf + token_pos, token, len);
        token_pos += len;
        token_buf[token_pos] = '\0';
    }
}

static void test_think_cb(const char *token, void *ud)
{
    (void)ud;
    int len = strlen(token);
    if (think_pos + len < (int)sizeof(think_buf)) {
        memcpy(think_buf + think_pos, token, len);
        think_pos += len;
        think_buf[think_pos] = '\0';
    }
}

/*
 * We can't directly test sse_process_data since it's static in llm.c.
 * Instead, test through llm_chat_stream with a mock server would be ideal,
 * but that requires network. So we test the public API contracts.
 */

static void test_llm_init_cleanup(void)
{
    int r = llm_init("http://localhost:1/v1/chat/completions", "test", NULL);
    ASSERT(r == 0);
    llm_cleanup();
}

static void test_llm_response_free_null(void)
{
    /* Should not crash */
    llm_response_free(NULL);
    ASSERT(1);
}

static void test_llm_response_free_empty(void)
{
    llm_response_t *resp = calloc(1, sizeof(*resp));
    llm_response_free(resp);
    ASSERT(1);
}

static void test_llm_response_free_with_data(void)
{
    llm_response_t *resp = calloc(1, sizeof(*resp));
    resp->text = strdup("hello");
    resp->num_tool_calls = 1;
    resp->tool_calls = calloc(1, sizeof(tool_call_t));
    resp->tool_calls[0].id = strdup("call_1");
    resp->tool_calls[0].name = strdup("ls");
    resp->tool_calls[0].arguments = strdup("{\"path\":\".\"}");
    llm_response_free(resp);
    ASSERT(1);
}

static void test_stream_cbs_struct(void)
{
    reset_bufs();
    llm_stream_cbs cbs = {
        .on_token = test_token_cb,
        .on_thinking = test_think_cb,
        .userdata = NULL
    };
    /* Verify struct is well-formed */
    ASSERT(cbs.on_token != NULL);
    ASSERT(cbs.on_thinking != NULL);

    /* Invoke callbacks directly */
    cbs.on_token("hello ", cbs.userdata);
    cbs.on_token("world", cbs.userdata);
    ASSERT_STR_EQ(token_buf, "hello world");

    cbs.on_thinking("thinking...", cbs.userdata);
    ASSERT_STR_EQ(think_buf, "thinking...");
}

static void test_chat_fallback_no_cbs(void)
{
    /* llm_chat_stream with NULL cbs should fall back to llm_chat */
    /* This will fail to connect but shouldn't crash */
    llm_init("http://localhost:1/v1/chat/completions", "test", NULL);
    llm_response_t *resp = llm_chat_stream(
        "test", ".", NULL, NULL, 0, NULL);
    /* Should return NULL (connection refused) */
    ASSERT_NULL(resp);
    llm_cleanup();
}

int main(void)
{
    fprintf(stderr, "test_sse:\n");

    RUN_TEST(test_llm_init_cleanup);
    RUN_TEST(test_llm_response_free_null);
    RUN_TEST(test_llm_response_free_empty);
    RUN_TEST(test_llm_response_free_with_data);
    RUN_TEST(test_stream_cbs_struct);
    RUN_TEST(test_chat_fallback_no_cbs);

    TEST_SUMMARY();
}
