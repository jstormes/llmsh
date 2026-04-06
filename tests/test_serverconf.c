#include <stdlib.h>
#include <signal.h>
#include "test.h"
#include "serverconf.h"

/* Required by llm.o */
volatile sig_atomic_t interrupted = 0;

static void test_load_defaults(void)
{
    /* With no ~/.llmshrc override, should fall back to env/defaults */
    server_config_t *conf = serverconf_load();
    ASSERT_NOT_NULL(conf);
    ASSERT(conf->count >= 1);
    ASSERT(conf->active == 0);
    ASSERT(conf->max_iterations > 0);
    ASSERT(conf->man_enrich >= 0);
    serverconf_free(conf);
}

static void test_active_server(void)
{
    server_config_t *conf = serverconf_load();
    const server_entry_t *s = serverconf_active(conf);
    ASSERT_NOT_NULL(s);
    ASSERT_NOT_NULL(s->name);
    ASSERT_NOT_NULL(s->api_url);
    serverconf_free(conf);
}

static void test_switch_valid(void)
{
    server_config_t *conf = serverconf_load();
    if (conf->count > 1) {
        const char *second_name = conf->servers[1].name;
        ASSERT(serverconf_switch(conf, second_name) == 0);
        ASSERT(conf->active == 1);
    }
    serverconf_free(conf);
}

static void test_switch_invalid(void)
{
    server_config_t *conf = serverconf_load();
    ASSERT(serverconf_switch(conf, "nonexistent_server_xyz") == -1);
    ASSERT(conf->active == 0); /* unchanged */
    serverconf_free(conf);
}

int main(void)
{
    fprintf(stderr, "test_serverconf:\n");

    RUN_TEST(test_load_defaults);
    RUN_TEST(test_active_server);
    RUN_TEST(test_switch_valid);
    RUN_TEST(test_switch_invalid);

    TEST_SUMMARY();
}
