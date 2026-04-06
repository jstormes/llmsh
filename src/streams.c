#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "streams.h"

FILE *stdchat = NULL;
int streams_verbose = 0;

void streams_init(void)
{
    /*
     * Check if fd 3 is already open (parent shell may have redirected it,
     * e.g., llmsh 3>review.txt). If so, just fdopen it.
     * If not, open /dev/tty and dup2 to fd 3 so chat goes to terminal.
     */
    if (fcntl(LLMSH_FD_CHAT, F_GETFD) >= 0) {
        /* fd 3 already open — parent shell redirected it */
        stdchat = fdopen(LLMSH_FD_CHAT, "w");
    } else {
        /* fd 3 not open — point it at the terminal */
        int tty_fd = open("/dev/tty", O_WRONLY);
        if (tty_fd >= 0) {
            if (tty_fd != LLMSH_FD_CHAT) {
                dup2(tty_fd, LLMSH_FD_CHAT);
                close(tty_fd);
            }
            stdchat = fdopen(LLMSH_FD_CHAT, "w");
        }
    }

    /* Fallback: if we couldn't set up fd 3, use stderr */
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

void stream_tool_output(const char *text)
{
    if (streams_verbose && text) {
        fputs(text, stdout);
        fflush(stdout);
    }
}

void stream_chat_output(const char *text)
{
    if (text && stdchat) {
        fputs(text, stdchat);
        fflush(stdchat);
    }
}
