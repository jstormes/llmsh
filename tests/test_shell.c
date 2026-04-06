#include <stdlib.h>
#include <signal.h>
#include "test.h"
#include "shell.h"
#include "pathscan.h"
#include "manscan.h"
#include "streams.h"
#include "history.h"
#include "builtin.h"
#include "router.h"
#include "serverconf.h"
#include "llm.h"

/* Signal flag — must match main.c definition */
volatile sig_atomic_t interrupted = 0;

static shell_ctx_t ctx;

static void setup(void)
{
    ctx.servers = serverconf_load();
    ctx.cbs = NULL;
    ctx.last_output = NULL;
    ctx.max_iterations = ctx.servers->max_iterations;

    streams_init();
    history_init();
    pathscan_init();
    manscan_init();
    builtin_init();
    router_init(ctx.servers);
}

static void teardown(void)
{
    free(ctx.last_output);
    ctx.last_output = NULL;
    history_cleanup();
    pathscan_cleanup();
    manscan_cleanup();
    streams_cleanup();
    serverconf_free(ctx.servers);
}

/* ── shell_is_exit tests ─────────────────────────────────────────── */

static void test_is_exit(void)
{
    ASSERT(shell_is_exit("exit") == 1);
    ASSERT(shell_is_exit("quit") == 1);
    ASSERT(shell_is_exit("/exit") == 1);
    ASSERT(shell_is_exit("/quit") == 1);
    ASSERT(shell_is_exit("ls") == 0);
    ASSERT(shell_is_exit("help") == 0);
    ASSERT(shell_is_exit("EXIT") == 0);
    ASSERT(shell_is_exit("") == 0);
}

/* ── shell_handle_slash tests ────────────────────────────────────── */

static void test_handle_slash_help(void)
{
    ASSERT(shell_handle_slash(&ctx, "help") == 1);
    ASSERT(shell_handle_slash(&ctx, "/help") == 1);
}

static void test_handle_slash_clear(void)
{
    history_add_user("test");
    ASSERT(shell_handle_slash(&ctx, "/clear") == 1);
    history_add_user("after clear");
}

static void test_handle_slash_verbose(void)
{
    int before = streams_verbose;
    ASSERT(shell_handle_slash(&ctx, "/verbose") == 1);
    ASSERT(streams_verbose != before);
    shell_handle_slash(&ctx, "/verbose");
    ASSERT(streams_verbose == before);
}

static void test_handle_slash_labels(void)
{
    int before = streams_label_mode;
    ASSERT(shell_handle_slash(&ctx, "/labels") == 1);
    ASSERT(streams_label_mode != before);
    shell_handle_slash(&ctx, "/labels");
}

static void test_handle_slash_debug(void)
{
    ASSERT(shell_handle_slash(&ctx, "/debug") == 1);
    ASSERT(streams_label_mode == 2);
    shell_handle_slash(&ctx, "/debug");
    ASSERT(streams_label_mode == 0);
}

static void test_handle_slash_unknown(void)
{
    ASSERT(shell_handle_slash(&ctx, "ls -la") == 0);
    ASSERT(shell_handle_slash(&ctx, "tell me about grep") == 0);
    ASSERT(shell_handle_slash(&ctx, "/unknown") == 0);
}

static void test_handle_slash_server_list(void)
{
    ASSERT(shell_handle_slash(&ctx, "/server") == 1);
}

static void test_handle_slash_server_bad(void)
{
    ASSERT(shell_handle_slash(&ctx, "/server nonexistent") == 1);
}

int main(void)
{
    fprintf(stderr, "test_shell:\n");
    setup();

    RUN_TEST(test_is_exit);
    RUN_TEST(test_handle_slash_help);
    RUN_TEST(test_handle_slash_clear);
    RUN_TEST(test_handle_slash_verbose);
    RUN_TEST(test_handle_slash_labels);
    RUN_TEST(test_handle_slash_debug);
    RUN_TEST(test_handle_slash_unknown);
    RUN_TEST(test_handle_slash_server_list);
    RUN_TEST(test_handle_slash_server_bad);

    teardown();
    TEST_SUMMARY();
}
