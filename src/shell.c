#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include "shell.h"
#include "llm.h"
#include "router.h"
#include "history.h"
#include "pathscan.h"
#include "streams.h"
#include "serverconf.h"
#include "exec.h"

/* Defined in main.c, shared */
extern volatile sig_atomic_t interrupted;
extern server_config_t *g_servers;
extern char *g_last_output;

/* ── Helpers ─────────────────────────────────────────────────────── */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t')) *end-- = '\0';
    return s;
}

static void first_word(const char *s, char *out, size_t out_sz)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t i = 0;
    while (*s && *s != ' ' && *s != '\t' && i < out_sz - 1)
        out[i++] = *s++;
    out[i] = '\0';
}

/* ── Agentic loop ────────────────────────────────────────────────── */

char *shell_agentic_loop(llm_response_t *resp, int max_iter,
                          llm_stream_cbs *cbs)
{
    char cwd[4096];
    int iterations = 0;
    char *final_text = NULL;

    while (resp && resp->num_tool_calls > 0
           && !interrupted && iterations < max_iter) {
        iterations++;

        /* Assistant text before tool calls */
        if (resp->text && resp->text[0]) {
            stream_chat_output("\n");
            history_add_assistant(resp->text);
        }

        /* Execute tool calls */
        free(g_last_output);
        g_last_output = NULL;

        for (int i = 0; i < resp->num_tool_calls && !interrupted; i++) {
            char *result = router_dispatch(&resp->tool_calls[i]);
            if (result) {
                stream_tool_output(result);
                if (result[0] && result[strlen(result)-1] != '\n')
                    stream_tool_output("\n");
                history_add_tool_result(resp->tool_calls[i].name, result);
                free(g_last_output);
                g_last_output = result;
            }
        }

        llm_response_free(resp);
        resp = NULL;

        if (interrupted) break;

        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");
        resp = llm_chat_stream(NULL, cwd, g_last_output, NULL, 0, cbs);
    }

    /* Final text */
    if (resp && resp->text && resp->text[0]) {
        stream_chat_output("\n");
        history_add_assistant(resp->text);
        final_text = strdup(resp->text);
    }

    if (iterations >= max_iter)
        fprintf(stderr, "llmsh: max iterations reached (%d)\n", max_iter);

    llm_response_free(resp);
    return final_text;
}

/* ── Query ───────────────────────────────────────────────────────── */

char *shell_query(const char *query, const char *context,
                  int max_iter, llm_stream_cbs *cbs)
{
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");

    /* Build full query with optional context */
    char *full_query;
    if (context) {
        size_t len = strlen(query) + strlen(context) + 64;
        full_query = malloc(len);
        snprintf(full_query, len,
                 "[context]:\n%s\n\n[user request]: %s",
                 context, query);
    } else {
        full_query = strdup(query);
    }

    int fw = 0;
    char *mc = pathscan_match_input(full_query, &fw);

    history_add_user(full_query);
    streams_llm_active = 1;
    llm_response_t *resp = llm_chat_stream(full_query, cwd, g_last_output,
                                            mc, fw, cbs);
    free(full_query);
    free(mc);

    if (!resp) {
        streams_llm_active = 0;
        fprintf(stderr, "llmsh: LLM request failed\n");
        return NULL;
    }

    char *result = shell_agentic_loop(resp, max_iter, cbs);
    streams_llm_active = 0;
    return result;
}

/* ── Split and execute ───────────────────────────────────────────── */

/*
 * Split cmdline on '|'. Command segments run directly.
 * First non-command segment triggers LLM handoff.
 * Returns: 0 = fully executed, 1 = LLM needed
 */
static int split_pipeline(const char *cmdline,
                           char **llm_prompt, char **pipe_context)
{
    *llm_prompt = NULL;
    *pipe_context = NULL;

    char *copy = strdup(cmdline);
    char *segments[64];
    int nseg = 0;

    char *saveptr = NULL;
    char *seg = strtok_r(copy, "|", &saveptr);
    while (seg && nseg < 64) {
        segments[nseg++] = trim(seg);
        seg = strtok_r(NULL, "|", &saveptr);
    }

    /* Find first non-command segment */
    int handoff = nseg;
    for (int i = 0; i < nseg; i++) {
        char word[256];
        first_word(segments[i], word, sizeof(word));
        if (!pathscan_lookup(word)) {
            handoff = i;
            break;
        }
    }

    /* Execute command segments */
    char *cmd_output = NULL;
    if (handoff > 0) {
        const char *cmds[64];
        for (int i = 0; i < handoff; i++)
            cmds[i] = segments[i];
        cmd_output = exec_pipeline(cmds, handoff, NULL, NULL, 0);
    }

    if (handoff < nseg) {
        /* Rejoin remaining as LLM prompt */
        size_t plen = 0;
        for (int i = handoff; i < nseg; i++)
            plen += strlen(segments[i]) + 3;
        char *prompt = malloc(plen + 1);
        prompt[0] = '\0';
        for (int i = handoff; i < nseg; i++) {
            if (i > handoff) strcat(prompt, " | ");
            strcat(prompt, segments[i]);
        }
        *llm_prompt = prompt;
        *pipe_context = cmd_output;
        free(copy);
        return 1;
    }

    /* All commands — show output */
    if (cmd_output) {
        stream_tool_output(cmd_output);
        if (cmd_output[0] && cmd_output[strlen(cmd_output)-1] != '\n')
            stream_tool_output("\n");
    }
    *pipe_context = cmd_output;
    free(copy);
    return 0;
}

char *shell_execute(const char *cmdline, int max_iter,
                     llm_stream_cbs *cbs)
{
    /* Handle cd specially */
    if (strncmp(cmdline, "cd", 2) == 0
        && (cmdline[2] == '\0' || cmdline[2] == ' ' || cmdline[2] == '\t')) {
        const char *path = cmdline + 2;
        while (*path == ' ' || *path == '\t') path++;
        if (*path == '\0') path = getenv("HOME");
        if (path && chdir(path) != 0)
            fprintf(stderr, "cd: %s: %s\n", path, strerror(errno));
        return NULL;
    }

    char *llm_prompt = NULL;
    char *pipe_context = NULL;
    int needs_llm = split_pipeline(cmdline, &llm_prompt, &pipe_context);

    if (needs_llm && llm_prompt) {
        char *result = shell_query(llm_prompt, pipe_context, max_iter, cbs);
        free(llm_prompt);
        free(pipe_context);
        return result;
    }

    /* Fully executed — update g_last_output */
    free(g_last_output);
    g_last_output = pipe_context;
    free(llm_prompt);
    return NULL;
}

/* ── Slash commands ──────────────────────────────────────────────── */

int shell_handle_slash(const char *line)
{
    if (strcmp(line, "help") == 0 || strcmp(line, "/help") == 0) {
        stream_chat_output(
            "llmsh - natural language shell\n"
            "\n"
            "Type plain English OR standard shell commands. llmsh understands both.\n"
            "\n"
            "Natural language examples:\n"
            "  show me what's in this directory\n"
            "  find all .c files larger than 10k\n"
            "  count the lines of code in src/\n"
            "\n"
            "Direct shell commands also work:\n"
            "  ls -la\n"
            "  gcc -o foo foo.c\n"
            "  grep -r TODO src/ | wc -l\n"
            "\n"
            "Hybrid pipes: ls | show me the markdown files\n"
            "\n"
            "Output streams:\n"
            "  fd 1 (stdout)   Tool output (visible by default)\n"
            "  fd 2 (stderr)   Errors and confirmations\n"
            "  fd 3 (stdchat)  LLM responses (visible by default)\n"
            "\n"
            "Flags: -v verbose  -q quiet  -l labels  -d debug  -h help\n"
            "Keys:  Shift+Tab cycle servers  Up/Down history\n"
            "\n"
            "Commands:\n"
            "  /server [name]   List or switch servers\n"
            "  /clear           Clear conversation history\n"
            "  /verbose         Toggle tool output\n"
            "  /labels          Toggle stream labels\n"
            "  /debug           Toggle debug mode\n"
            "  exit, /exit      Exit the shell\n"
        );
        return 1;
    }

    if (strcmp(line, "/clear") == 0) {
        history_cleanup();
        history_init();
        free(g_last_output);
        g_last_output = NULL;
        stream_chat_output("Conversation history cleared.\n");
        return 1;
    }

    if (strcmp(line, "/verbose") == 0) {
        streams_verbose = !streams_verbose;
        stream_chat_output(streams_verbose
            ? "Tool output: visible\n"
            : "Tool output: hidden\n");
        return 1;
    }

    if (strcmp(line, "/labels") == 0) {
        streams_label_mode = streams_label_mode ? 0 : 1;
        stream_chat_output(streams_label_mode
            ? "Labels: on\n" : "Labels: off\n");
        return 1;
    }

    if (strcmp(line, "/debug") == 0) {
        streams_label_mode = (streams_label_mode == 2) ? 0 : 2;
        stream_chat_output(streams_label_mode == 2
            ? "Debug mode: on (labels + API)\n"
            : "Debug mode: off\n");
        return 1;
    }

    /* /server command */
    if (strncmp(line, "/server", 7) == 0) {
        const char *arg = line + 7;
        while (*arg == ' ') arg++;

        if (*arg == '\0') {
            serverconf_list(g_servers);
            return 1;
        }

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

        history_cleanup();
        history_init();
        free(g_last_output);
        g_last_output = NULL;
        return 1;
    }

    return 0;
}

int shell_is_exit(const char *line)
{
    return strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0
        || strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0;
}
