#include <stdlib.h>
#include <signal.h>
#include "test.h"
#include "history.h"
#include "cJSON.h"

/* Required by llm.o */
volatile sig_atomic_t interrupted = 0;

static void test_init_cleanup(void)
{
    history_init();
    history_cleanup();
    /* Should not crash on double cleanup */
    history_cleanup();
}

static void test_add_and_build(void)
{
    history_init();
    history_add_user("hello");
    history_add_assistant("hi there");

    char *json = history_build_messages("system prompt");
    ASSERT_NOT_NULL(json);

    /* Should be valid JSON array */
    cJSON *arr = cJSON_Parse(json);
    ASSERT_NOT_NULL(arr);
    ASSERT(cJSON_IsArray(arr));

    /* Should have 3 messages: system + user + assistant */
    ASSERT(cJSON_GetArraySize(arr) == 3);

    cJSON_Delete(arr);
    free(json);
    history_cleanup();
}

static void test_tool_result(void)
{
    history_init();
    history_add_user("list files");
    history_add_tool_result("ls", "file1\nfile2\n");

    char *json = history_build_messages("sys");
    ASSERT_NOT_NULL(json);
    /* Should contain the tool result */
    ASSERT(strstr(json, "file1") != NULL);
    free(json);
    history_cleanup();
}

static void test_eviction(void)
{
    history_init();
    /* Add more messages than the buffer can hold (MAX_HISTORY * 3 = 60) */
    for (int i = 0; i < 70; i++) {
        history_add_user("msg");
    }
    /* Should not crash — oldest messages evicted */
    char *json = history_build_messages("sys");
    ASSERT_NOT_NULL(json);
    free(json);
    history_cleanup();
}

int main(void)
{
    fprintf(stderr, "test_history:\n");

    RUN_TEST(test_init_cleanup);
    RUN_TEST(test_add_and_build);
    RUN_TEST(test_tool_result);
    RUN_TEST(test_eviction);

    TEST_SUMMARY();
}
