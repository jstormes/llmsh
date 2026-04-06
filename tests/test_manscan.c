#include <stdlib.h>
#include <signal.h>
#include "test.h"
#include "manscan.h"

/* Required by llm.o */
volatile sig_atomic_t interrupted = 0;

static void test_init(void)
{
    int count = manscan_init();
    ASSERT(count > 100); /* should have many entries */
}

static void test_whatis_found(void)
{
    char *s = manscan_whatis("ls");
    ASSERT_NOT_NULL(s);
    /* Should contain "list" or "directory" */
    if (s) ASSERT(strstr(s, "list") != NULL || strstr(s, "director") != NULL);
    free(s);
}

static void test_whatis_missing(void)
{
    char *s = manscan_whatis("zzz_nonexistent_zzz");
    ASSERT_NULL(s);
}

static void test_detail(void)
{
    char *d = manscan_detail("ls", 2048);
    ASSERT_NOT_NULL(d);
    if (d) {
        /* Should contain NAME and SYNOPSIS sections */
        ASSERT(strstr(d, "NAME") != NULL || strstr(d, "ls") != NULL);
    }
    free(d);
}

static void test_detail_truncated(void)
{
    char *d = manscan_detail("ls", 256);
    ASSERT_NOT_NULL(d);
    if (d) ASSERT(strlen(d) <= 256);
    free(d);
}

static void test_detail_invalid(void)
{
    /* Shell injection attempt */
    char *d = manscan_detail("; rm -rf /", 1024);
    ASSERT_NULL(d);
}

static void test_enrich_pipeline(void)
{
    const char *cmds[] = {"ls -la", "grep foo"};
    char *ctx = manscan_enrich_pipeline(cmds, 2);
    ASSERT_NOT_NULL(ctx);
    if (ctx) {
        ASSERT(strstr(ctx, "ls:") != NULL);
        ASSERT(strstr(ctx, "grep:") != NULL);
    }
    free(ctx);
}

static void test_cleanup(void)
{
    manscan_cleanup();
    char *s = manscan_whatis("ls");
    ASSERT_NULL(s);
}

int main(void)
{
    fprintf(stderr, "test_manscan:\n");

    RUN_TEST(test_init);
    RUN_TEST(test_whatis_found);
    RUN_TEST(test_whatis_missing);
    RUN_TEST(test_detail);
    RUN_TEST(test_detail_truncated);
    RUN_TEST(test_detail_invalid);
    RUN_TEST(test_enrich_pipeline);
    RUN_TEST(test_cleanup);

    TEST_SUMMARY();
}
