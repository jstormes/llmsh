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

static volatile sig_atomic_t interrupted = 0;

static void sigint_handler(int sig)
{
    (void)sig;
    interrupted = 1;
}

int main(int argc, char **argv)
{
    const char *api_url = getenv("LLMSH_API_URL");
    const char *model   = getenv("LLMSH_MODEL");
    const char *api_key = getenv("LLMSH_API_KEY");

    if (!api_url) api_url = LLMSH_DEFAULT_API_URL;
    if (!model)   model   = LLMSH_DEFAULT_MODEL;

    (void)argc;
    (void)argv;

    /* Setup */
    signal(SIGINT, sigint_handler);
    builtin_init();
    history_init();

    /* Readline history - load from file */
    char histfile[4096];
    snprintf(histfile, sizeof(histfile), "%s/.llmsh_history", getenv("HOME") ? getenv("HOME") : ".");
    read_history(histfile);

    if (llm_init(api_url, model, api_key) != 0) {
        fprintf(stderr, "llmsh: failed to initialize LLM client\n");
        return 1;
    }

    printf("llmsh - natural language shell\n");
    printf("Type natural language commands. 'exit' or Ctrl-D to quit.\n\n");

    char cwd[4096];
    char *last_output = NULL;

    for (;;) {
        interrupted = 0;

        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, ".");

        char prompt[4200];
        snprintf(prompt, sizeof(prompt), "%s %s", cwd, LLMSH_PROMPT);

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

    return 0;
}
