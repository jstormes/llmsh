#include <stdlib.h>
#include <signal.h>
#include "test.h"
#include "pathscan.h"

/* Required by llm.o */
volatile sig_atomic_t interrupted = 0;

static void test_init(void)
{
    int count = pathscan_init();
    ASSERT(count > 0);
    /* Common commands should be found */
    ASSERT(pathscan_lookup("ls") == 1);
    ASSERT(pathscan_lookup("grep") == 1);
    ASSERT(pathscan_lookup("gcc") == 1);
}

static void test_lookup_missing(void)
{
    ASSERT(pathscan_lookup("zzz_nonexistent_cmd_zzz") == 0);
    ASSERT(pathscan_lookup("") == 0);
}

static void test_match_input_first_word(void)
{
    int fw = 0;
    char *m = pathscan_match_input("ls -la /tmp", &fw);
    ASSERT(fw == 1);
    ASSERT_NOT_NULL(m);
    free(m);
}

static void test_match_input_no_match(void)
{
    int fw = 0;
    char *m = pathscan_match_input("show me the files", &fw);
    /* "show" is unlikely to be in PATH */
    ASSERT(fw == 0);
    /* m may or may not be NULL depending on whether "me", "the", "files" match */
    free(m);
}

static void test_match_input_middle_word(void)
{
    int fw = 0;
    char *m = pathscan_match_input("please run grep on this", &fw);
    /* "please" is not a command */
    ASSERT(fw == 0);
    /* but "grep" should be in the match list */
    if (m) {
        ASSERT(strstr(m, "grep") != NULL);
    }
    free(m);
}

static void test_cleanup(void)
{
    pathscan_cleanup();
    /* After cleanup, lookups should fail */
    ASSERT(pathscan_lookup("ls") == 0);
}

int main(void)
{
    fprintf(stderr, "test_pathscan:\n");

    RUN_TEST(test_init);
    RUN_TEST(test_lookup_missing);
    RUN_TEST(test_match_input_first_word);
    RUN_TEST(test_match_input_no_match);
    RUN_TEST(test_match_input_middle_word);
    RUN_TEST(test_cleanup);

    TEST_SUMMARY();
}
