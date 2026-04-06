#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "config.h"
#include "llm.h"
#include "router.h"
#include "builtin.h"
#include "history.h"
#include "serverconf.h"

static volatile sig_atomic_t interrupted = 0;
static server_config_t *g_servers = NULL;

static void sigint_handler(int sig)
{
    (void)sig;
    interrupted = 1;
}

/* Handle /server commands. Returns 1 if handled, 0 if not a server command. */
static int handle_server_cmd(const char *line)
{
    if (strncmp(line, "/server", 7) != 0)
        return 0;

    const char *arg = line + 7;
    while (*arg == ' ') arg++;

    if (*arg == '\0') {
        /* List servers */
        serverconf_list(g_servers);
        return 1;
    }

    /* Switch server */
    if (serverconf_switch(g_servers, arg) != 0) {
        fprintf(stderr, "llmsh: unknown server '%s'\n", arg);
        return 1;
    }

    const server_entry_t *s = serverconf_active(g_servers);
    llm_cleanup();
    if (llm_init(s->api_url, s->model, s->api_key) != 0) {
        fprintf(stderr, "llmsh: failed to connect to %s\n", s->name);
    } else {
        printf("Switched to %s (%s)\n", s->name, s->model);
    }

    /* Clear conversation history on server switch */
    history_cleanup();
    history_init();

    return 1;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Load server config */
    g_servers = serverconf_load();
    const server_entry_t *active = serverconf_active(g_servers);

    /* Setup */
    signal(SIGINT, sigint_handler);
    builtin_init();
    history_init();

    /* Readline history - load from file */
    char histfile[4096];
    snprintf(histfile, sizeof(histfile), "%s/.llmsh_history",
             getenv("HOME") ? getenv("HOME") : ".");
    read_history(histfile);

    if (llm_init(active->api_url, active->model, active->api_key) != 0) {
        fprintf(stderr, "llmsh: failed to initialize LLM client\n");
        return 1;
    }

    printf("llmsh - natural language shell\n");
    printf("Server: %s (%s)\n", active->name, active->model);
    printf("Type natural language commands. /server to list/switch. 'exit' to quit.\n\n");

    char cwd[4096];
    char *last_output = NULL;

    for (;;) {
        interrupted = 0;
        active = serverconf_active(g_servers);

        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, ".");

        char prompt[4200];
        snprintf(prompt, sizeof(prompt), "%s@%s> ",
                 active->model ? active->model : "llmsh", cwd);

        char *line = readline(prompt);
        if (!line) {
            printf("\n");
            break;
        }

        if (line[0] == '\0') {
            free(line);
            continue;
        }

        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            free(line);
            break;
        }

        /* Add to readline history */
        add_history(line);

        /* Handle built-in shell commands */
        if (handle_server_cmd(line)) {
            free(line);
            continue;
        }

        /* Send to LLM */
        history_add_user(line);
        llm_response_t *resp = llm_chat(line, cwd, last_output);
        free(line);

        if (!resp) {
            fprintf(stderr, "llmsh: LLM request failed\n");
            continue;
        }

        /* Handle tool calls */
        free(last_output);
        last_output = NULL;

        if (resp->num_tool_calls > 0) {
            for (int i = 0; i < resp->num_tool_calls && !interrupted; i++) {
                char *result = router_dispatch(&resp->tool_calls[i]);
                if (result) {
                    printf("%s", result);
                    if (result[0] && result[strlen(result)-1] != '\n')
                        printf("\n");

                    history_add_tool_result(resp->tool_calls[i].name, result);

                    /* Keep last output for context */
                    free(last_output);
                    last_output = result;
                }
            }
        }

        /* Print assistant text if any */
        if (resp->text && resp->text[0]) {
            printf("%s\n", resp->text);
            history_add_assistant(resp->text);
        }

        llm_response_free(resp);
    }

    free(last_output);
    write_history(histfile);
    history_cleanup();
    llm_cleanup();
    serverconf_free(g_servers);

    return 0;
}
