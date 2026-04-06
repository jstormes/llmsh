#include <stdlib.h>
#include <signal.h>
#include "test.h"
#include "history.h"
#include "llm.h"
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

    cJSON *arr = cJSON_Parse(json);
    ASSERT_NOT_NULL(arr);
    ASSERT(cJSON_IsArray(arr));
    /* system + user + assistant = 3 */
    ASSERT(cJSON_GetArraySize(arr) == 3);

    cJSON_Delete(arr);
    free(json);
    history_cleanup();
}

static void test_tool_protocol(void)
{
    history_init();
    history_add_user("list files");

    /* Simulate assistant response with tool calls */
    tool_call_t tc = {
        .id = "call_123",
        .name = "ls",
        .arguments = "{\"path\":\".\"}"
    };
    history_add_assistant_tool_calls(NULL, &tc, 1);

    /* Simulate tool result */
    history_add_tool_result("call_123", "ls", "file1\nfile2\n");

    char *json = history_build_messages("sys");
    ASSERT_NOT_NULL(json);

    cJSON *arr = cJSON_Parse(json);
    ASSERT_NOT_NULL(arr);

    /* system + user + assistant(tool_calls) + tool = 4 */
    ASSERT(cJSON_GetArraySize(arr) == 4);

    /* Check assistant message has tool_calls */
    cJSON *asst = cJSON_GetArrayItem(arr, 2);
    ASSERT_NOT_NULL(asst);
    cJSON *role = cJSON_GetObjectItem(asst, "role");
    ASSERT_STR_EQ(role->valuestring, "assistant");
    cJSON *tcs = cJSON_GetObjectItem(asst, "tool_calls");
    ASSERT_NOT_NULL(tcs);
    ASSERT(cJSON_IsArray(tcs));
    ASSERT(cJSON_GetArraySize(tcs) == 1);

    /* Check tool_call has id and function */
    cJSON *tc0 = cJSON_GetArrayItem(tcs, 0);
    cJSON *tc_id = cJSON_GetObjectItem(tc0, "id");
    ASSERT_STR_EQ(tc_id->valuestring, "call_123");

    /* Check tool result message */
    cJSON *tool_msg = cJSON_GetArrayItem(arr, 3);
    cJSON *tool_role = cJSON_GetObjectItem(tool_msg, "role");
    ASSERT_STR_EQ(tool_role->valuestring, "tool");
    cJSON *tool_id = cJSON_GetObjectItem(tool_msg, "tool_call_id");
    ASSERT_STR_EQ(tool_id->valuestring, "call_123");
    cJSON *content = cJSON_GetObjectItem(tool_msg, "content");
    ASSERT(strstr(content->valuestring, "file1") != NULL);

    cJSON_Delete(arr);
    free(json);
    history_cleanup();
}

static void test_eviction(void)
{
    history_init();
    for (int i = 0; i < 70; i++) {
        history_add_user("msg");
    }
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
    RUN_TEST(test_tool_protocol);
    RUN_TEST(test_eviction);

    TEST_SUMMARY();
}
