#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "streams.h"

FILE *stdchat = NULL;
int streams_verbose = 1;
int streams_label_mode = 0;

/* Track whether we're at the start of a line for each stream (for prefixing) */
static int chat_sol = 1;   /* start-of-line for chat */
static int tool_sol = 1;   /* start-of-line for tool output */
static int think_sol = 1;  /* start-of-line for thinking */

void streams_init(void)
{
    if (fcntl(LLMSH_FD_CHAT, F_GETFD) >= 0) {
        stdchat = fdopen(LLMSH_FD_CHAT, "w");
    } else {
        int tty_fd = open("/dev/tty", O_WRONLY);
        if (tty_fd >= 0) {
            if (tty_fd != LLMSH_FD_CHAT) {
                dup2(tty_fd, LLMSH_FD_CHAT);
                close(tty_fd);
            }
            stdchat = fdopen(LLMSH_FD_CHAT, "w");
        }
    }

    if (!stdchat)
        stdchat = stderr;
}

void streams_cleanup(void)
{
    if (stdchat && stdchat != stderr && stdchat != stdout) {
        fclose(stdchat);
        stdchat = NULL;
    }
}

/*
 * Write text with optional line-by-line label prefixing.
 * Tracks start-of-line state per stream so labels appear at each new line.
 */
static void labeled_write(FILE *fp, const char *label, const char *color,
                           const char *text, int *sol)
{
    if (!text || !fp) return;

    if (!streams_label_mode) {
        fputs(text, fp);
        fflush(fp);
        return;
    }

    for (const char *p = text; *p; p++) {
        if (*sol) {
            fprintf(fp, "%s[%s]%s ", color, label, "\033[0m");
            *sol = 0;
        }
        fputc(*p, fp);
        if (*p == '\n')
            *sol = 1;
    }
    fflush(fp);
}

void stream_tool_output(const char *text)
{
    if (streams_verbose && text)
        labeled_write(stdout, "stdout", "\033[36m", text, &tool_sol);
}

void stream_chat_output(const char *text)
{
    if (text && stdchat)
        labeled_write(stdchat, "chat", "\033[32m", text, &chat_sol);
}

void stream_think_output(const char *text)
{
    if (!text || !stdchat || !streams_label_mode) return;
    labeled_write(stdchat, "think", "\033[35m", text, &think_sol);
}

void stream_tool_call(const char *name, const char *args)
{
    if (!streams_label_mode || !stdchat) return;
    fprintf(stdchat, "\033[33m[tool]\033[0m %s(%s)\n", name, args ? args : "");
    fflush(stdchat);
}

void stream_api_output(const char *direction, const char *text)
{
    if (streams_label_mode < 2 || !stdchat || !text) return;
    fprintf(stdchat, "\033[90m[api%s]\033[0m %s\n", direction, text);
    fflush(stdchat);
}
