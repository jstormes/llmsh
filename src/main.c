#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "config.h"
#include "llm.h"
#include "router.h"
#include "builtin.h"
#include "history.h"
#include "serverconf.h"
#include "pathscan.h"
#include "streams.h"

static volatile sig_atomic_t interrupted = 0;
static server_config_t *g_servers = NULL;

static void sigint_handler(int sig)
{
    (void)sig;
    interrupted = 1;
}

/* SSE streaming callback: write each token to stdchat as it arrives */
static void chat_token_cb(const char *token, void *userdata)
{
    (void)userdata;
    stream_chat_output(token);
}

static void show_help(void)
{
    stream_chat_output(
        "llmsh - natural language shell\n"
        "\n"
        "Type plain English OR standard shell commands. llmsh understands both.\n"
        "\n"
        "Natural language examples:\n"
        "  show me what's in this directory\n"
        "  find all .c files larger than 10k\n"
        "  count the lines of code in src/\n"
        "  search for TODO comments in the project\n"
        "\n"
        "Direct shell commands also work:\n"
        "  ls -la\n"
        "  gcc -o foo foo.c\n"
        "  grep -r TODO src/ | wc -l\n"
        "  make clean && make\n"
        "\n"
        "Output streams:\n"
        "  fd 1 (stdout)   Tool output (hidden by default, use -v to show)\n"
        "  fd 2 (stderr)   Errors and confirmations\n"
        "  fd 3 (stdchat)  LLM responses (visible by default)\n"
        "\n"
        "  Redirect: llmsh 3>review.txt    Pipe: llmsh 3>&1 | less\n"
        "\n"
        "Flags:\n"
        "  -v               Show tool output (verbose)\n"
        "  -q               Hide tool output (default)\n"
        "  -h               Show this help\n"
        "\n"
        "Built-in tools (no confirmation needed):\n"
        "  ls, cat, head, wc, grep, pwd, cd, read_file\n"
        "\n"
        "Write tools (confirmation required):\n"
        "  cp, mv, mkdir, write_file\n"
        "\n"
        "Dangerous tools (explicit confirmation):\n"
        "  rm, run (arbitrary shell commands with pipes/redirection)\n"
        "\n"
        "Commands:\n"
        "  /server              List configured servers\n"
        "  /server <name>       Switch to a different LLM server\n"
        "  /clear               Clear conversation history\n"
        "  /verbose             Toggle tool output visibility\n"
        "  help                 Show this help message\n"
        "  exit, quit           Exit the shell\n"
        "\n"
        "Configuration:\n"
        "  ~/.llmshrc           Server configuration (INI format)\n"
        "  ~/.llmsh_history     Command history\n"
    );
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
        char msg[256];
        snprintf(msg, sizeof(msg), "Switched to %s (%s)\n", s->name, s->model);
        stream_chat_output(msg);
    }

    /* Clear conversation history on server switch */
    history_cleanup();
    history_init();

    return 1;
}

int main(int argc, char **argv)
{
    /* Parse command line flags */
    int opt;
    while ((opt = getopt(argc, argv, "vqh")) != -1) {
        switch (opt) {
        case 'v': streams_verbose = 1; break;
        case 'q': streams_verbose = 0; break;
        case 'h':
            streams_init();
            show_help();
            streams_cleanup();
            return 0;
        }
    }

    /* Load server config */
    g_servers = serverconf_load();
    const server_entry_t *active = serverconf_active(g_servers);

    /* Read piped stdin if not a terminal */
    char *piped_input = NULL;
    if (!isatty(STDIN_FILENO)) {
        size_t pi_len = 0, pi_cap = 0;
        char buf[4096];
        ssize_t n;
        while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
            if (pi_len + n + 1 > pi_cap) {
                pi_cap = (pi_cap == 0) ? 8192 : pi_cap * 2;
                piped_input = realloc(piped_input, pi_cap);
            }
            memcpy(piped_input + pi_len, buf, n);
            pi_len += n;
        }
        if (piped_input) piped_input[pi_len] = '\0';

        /* Reopen /dev/tty as stdin for interactive prompts */
        FILE *tty = fopen("/dev/tty", "r");
        if (tty) {
            dup2(fileno(tty), STDIN_FILENO);
            fclose(tty);
        }
    }

    /* Setup */
    signal(SIGINT, sigint_handler);
    streams_init();
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

    /* Build PATH command hash table */
    int ncmds = pathscan_init();

    {
        char banner[512];
        snprintf(banner, sizeof(banner),
                 "llmsh - natural language shell\n"
                 "Server: %s (%s) | %d commands in PATH\n"
                 "Type natural language or shell commands. 'help' for usage. 'exit' to quit.\n\n",
                 active->name, active->model, ncmds);
        stream_chat_output(banner);
    }

    char cwd[4096];
    char *last_output = NULL;

    /* One-shot mode: remaining args are the query */
    if (optind < argc) {
        /* Concatenate remaining args */
        size_t qlen = 0;
        for (int i = optind; i < argc; i++)
            qlen += strlen(argv[i]) + 1;
        char *query = malloc(qlen);
        query[0] = '\0';
        for (int i = optind; i < argc; i++) {
            if (i > optind) strcat(query, " ");
            strcat(query, argv[i]);
        }

        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, ".");

        int first_word_is_cmd = 0;
        char *matched_cmds = pathscan_match_input(query, &first_word_is_cmd);

        /* If we have piped stdin, prepend it as context */
        if (piped_input) {
            size_t full_len = strlen(query) + strlen(piped_input) + 64;
            char *full_query = malloc(full_len);
            snprintf(full_query, full_len,
                     "[stdin content]:\n%s\n\n[user request]: %s",
                     piped_input, query);
            history_add_user(full_query);
            llm_response_t *resp = llm_chat_stream(full_query, cwd, NULL,
                                             matched_cmds, first_word_is_cmd,
                                             chat_token_cb, NULL);
            free(full_query);
            free(query);
            free(matched_cmds);
            free(piped_input);

            /* Agentic loop */
            int max_iter = g_servers->max_iterations;
            int iterations = 0;
            while (resp && resp->num_tool_calls > 0
                   && !interrupted && iterations < max_iter) {
                iterations++;
                if (resp->text && resp->text[0]) {
                    stream_chat_output("\n"); /* text already streamed */
                    history_add_assistant(resp->text);
                }
                free(last_output);
                last_output = NULL;
                for (int i = 0; i < resp->num_tool_calls && !interrupted; i++) {
                    char *result = router_dispatch(&resp->tool_calls[i]);
                    if (result) {
                        stream_tool_output(result);
                        if (result[0] && result[strlen(result)-1] != '\n')
                            stream_tool_output("\n");
                        history_add_tool_result(resp->tool_calls[i].name, result);
                        free(last_output);
                        last_output = result;
                    }
                }
                llm_response_free(resp);
                if (interrupted) break;
                if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");
                resp = llm_chat_stream(NULL, cwd, last_output, NULL, 0,
                                       chat_token_cb, NULL);
            }
            if (resp && resp->text && resp->text[0]) {
                stream_chat_output("\n"); /* text already streamed */
                history_add_assistant(resp->text);
            }
            llm_response_free(resp);
            free(last_output);
            write_history(histfile);
            history_cleanup();
            llm_cleanup();
            pathscan_cleanup();
            streams_cleanup();
            serverconf_free(g_servers);
            return 0;
        }

        history_add_user(query);
        llm_response_t *resp = llm_chat_stream(query, cwd, NULL,
                                         matched_cmds, first_word_is_cmd,
                                         chat_token_cb, NULL);
        free(query);
        free(matched_cmds);

        /* Run agentic loop for one-shot too */
        int max_iter = g_servers->max_iterations;
        int iterations = 0;
        while (resp && resp->num_tool_calls > 0
               && !interrupted && iterations < max_iter) {
            iterations++;

            if (resp->text && resp->text[0]) {
                stream_chat_output("\n"); /* text already streamed */
                history_add_assistant(resp->text);
            }

            free(last_output);
            last_output = NULL;

            for (int i = 0; i < resp->num_tool_calls && !interrupted; i++) {
                char *result = router_dispatch(&resp->tool_calls[i]);
                if (result) {
                    stream_tool_output(result);
                    if (result[0] && result[strlen(result)-1] != '\n')
                        stream_tool_output("\n");
                    history_add_tool_result(resp->tool_calls[i].name, result);
                    free(last_output);
                    last_output = result;
                }
            }

            llm_response_free(resp);
            if (interrupted) break;

            if (!getcwd(cwd, sizeof(cwd)))
                strcpy(cwd, ".");
            resp = llm_chat_stream(NULL, cwd, last_output, NULL, 0,
                                   chat_token_cb, NULL);
        }

        /* Text was already streamed; just add trailing newline and save to history */
        if (resp && resp->text && resp->text[0]) {
            stream_chat_output("\n");
            history_add_assistant(resp->text);
        }

        llm_response_free(resp);
        free(last_output);
        write_history(histfile);
        history_cleanup();
        llm_cleanup();
        pathscan_cleanup();
        streams_cleanup();
        serverconf_free(g_servers);
        return 0;
    }

    /* Interactive REPL */
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
            stream_chat_output("\n");
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
        if (strcmp(line, "help") == 0 || strcmp(line, "/help") == 0) {
            show_help();
            free(line);
            continue;
        }

        if (strcmp(line, "/clear") == 0) {
            history_cleanup();
            history_init();
            stream_chat_output("Conversation history cleared.\n");
            free(line);
            continue;
        }

        if (strcmp(line, "/verbose") == 0) {
            streams_verbose = !streams_verbose;
            stream_chat_output(streams_verbose
                ? "Tool output: visible\n"
                : "Tool output: hidden\n");
            free(line);
            continue;
        }

        if (handle_server_cmd(line)) {
            free(line);
            continue;
        }

        /* Match words in input against PATH commands */
        int first_word_is_cmd = 0;
        char *matched_cmds = pathscan_match_input(line, &first_word_is_cmd);

        /* Send to LLM with command hints */
        history_add_user(line);
        llm_response_t *resp = llm_chat_stream(line, cwd, last_output,
                                         matched_cmds, first_word_is_cmd,
                                         chat_token_cb, NULL);
        free(line);
        free(matched_cmds);

        if (!resp) {
            fprintf(stderr, "llmsh: LLM request failed\n");
            continue;
        }

        /* Agentic loop: keep going while the LLM makes tool calls */
        int max_iter = g_servers->max_iterations;
        int iterations = 0;
        while (resp && resp->num_tool_calls > 0
               && !interrupted
               && iterations < max_iter) {

            iterations++;

            /* Print assistant text before tool calls if any */
            if (resp->text && resp->text[0]) {
                stream_chat_output("\n"); /* text already streamed */
                history_add_assistant(resp->text);
            }

            /* Execute all tool calls */
            free(last_output);
            last_output = NULL;

            for (int i = 0; i < resp->num_tool_calls && !interrupted; i++) {
                char *result = router_dispatch(&resp->tool_calls[i]);
                if (result) {
                    stream_tool_output(result);
                    if (result[0] && result[strlen(result)-1] != '\n')
                        stream_tool_output("\n");

                    history_add_tool_result(resp->tool_calls[i].name, result);

                    free(last_output);
                    last_output = result;
                }
            }

            llm_response_free(resp);

            if (interrupted) break;

            /* Send results back to LLM for next round */
            if (!getcwd(cwd, sizeof(cwd)))
                strcpy(cwd, ".");
            resp = llm_chat_stream(NULL, cwd, last_output, NULL, 0,
                                   chat_token_cb, NULL);
        }

        /* Final text already streamed; just add newline and save history */
        if (resp && resp->text && resp->text[0]) {
            stream_chat_output("\n");
            history_add_assistant(resp->text);
        }

        if (iterations >= max_iter)
            fprintf(stderr, "llmsh: max iterations reached (%d)\n", max_iter);

        llm_response_free(resp);
    }

    free(last_output);
    free(piped_input);
    write_history(histfile);
    history_cleanup();
    llm_cleanup();
    pathscan_cleanup();
    streams_cleanup();
    serverconf_free(g_servers);

    return 0;
}
