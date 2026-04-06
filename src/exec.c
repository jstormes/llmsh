#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#include "exec.h"
#include "config.h"

/* Parse a command string into argv (simple space splitting, respects quotes) */
static char **parse_argv(const char *cmd, int *argc_out)
{
    int cap = 8, n = 0;
    char **argv = calloc(cap, sizeof(char *));
    const char *p = cmd;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char quote = 0;
        if (*p == '"' || *p == '\'') {
            quote = *p++;
        }

        const char *start = p;
        while (*p && (quote ? *p != quote : (*p != ' ' && *p != '\t')))
            p++;

        size_t len = p - start;
        char *arg = malloc(len + 1);
        memcpy(arg, start, len);
        arg[len] = '\0';

        if (quote && *p == quote) p++;

        if (n + 1 >= cap) {
            cap *= 2;
            argv = realloc(argv, cap * sizeof(char *));
        }
        argv[n++] = arg;
    }

    argv[n] = NULL;
    *argc_out = n;
    return argv;
}

static void free_argv(char **argv)
{
    for (int i = 0; argv[i]; i++)
        free(argv[i]);
    free(argv);
}

char *exec_pipeline(const char **pipeline, int n_cmds,
                    const char *stdin_file, const char *stdout_file,
                    int append)
{
    if (n_cmds <= 0) return strdup("error: empty pipeline");

    int pipefds[2 * (n_cmds - 1)];

    /* Create all pipes */
    for (int i = 0; i < n_cmds - 1; i++) {
        if (pipe(pipefds + 2 * i) < 0) {
            return strdup("error: pipe() failed");
        }
    }

    /* For capturing output of the last command */
    int capture_pipe[2] = {-1, -1};
    if (!stdout_file) {
        if (pipe(capture_pipe) < 0)
            return strdup("error: pipe() failed");
    }

    pid_t pids[n_cmds];

    for (int i = 0; i < n_cmds; i++) {
        int argc;
        char **argv = parse_argv(pipeline[i], &argc);
        if (argc == 0) {
            free_argv(argv);
            continue;
        }

        pids[i] = fork();
        if (pids[i] < 0) {
            free_argv(argv);
            return strdup("error: fork() failed");
        }

        if (pids[i] == 0) {
            /* Child process */

            /* stdin: from file, or from previous pipe */
            if (i == 0 && stdin_file) {
                int fd = open(stdin_file, O_RDONLY);
                if (fd >= 0) { dup2(fd, STDIN_FILENO); close(fd); }
            } else if (i > 0) {
                dup2(pipefds[2 * (i - 1)], STDIN_FILENO);
            }

            /* stdout: to file, to next pipe, or to capture pipe */
            if (i == n_cmds - 1 && stdout_file) {
                int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
                int fd = open(stdout_file, flags, 0644);
                if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }
            } else if (i < n_cmds - 1) {
                dup2(pipefds[2 * i + 1], STDOUT_FILENO);
            } else if (capture_pipe[1] >= 0) {
                dup2(capture_pipe[1], STDOUT_FILENO);
            }

            /* Close all pipe fds */
            for (int j = 0; j < 2 * (n_cmds - 1); j++)
                close(pipefds[j]);
            if (capture_pipe[0] >= 0) close(capture_pipe[0]);
            if (capture_pipe[1] >= 0) close(capture_pipe[1]);

            execvp(argv[0], argv);
            fprintf(stderr, "llmsh: %s: %s\n", argv[0], strerror(errno));
            _exit(127);
        }

        free_argv(argv);
    }

    /* Parent: close all pipe write ends */
    for (int i = 0; i < 2 * (n_cmds - 1); i++)
        close(pipefds[i]);

    if (capture_pipe[1] >= 0)
        close(capture_pipe[1]);

    /* Read captured output */
    char *output = NULL;
    if (capture_pipe[0] >= 0) {
        size_t len = 0, cap = 0;
        char buf[4096];
        ssize_t n;

        while ((n = read(capture_pipe[0], buf, sizeof(buf))) > 0) {
            if (len + n + 1 > cap) {
                cap = (cap == 0) ? 4096 : cap * 2;
                if (cap > LLMSH_MAX_OUTPUT_CAPTURE) cap = LLMSH_MAX_OUTPUT_CAPTURE;
                output = realloc(output, cap);
            }
            if (len + n < cap) {
                memcpy(output + len, buf, n);
                len += n;
            }
        }
        close(capture_pipe[0]);

        if (output) output[len] = '\0';
    }

    /* Wait for all children */
    for (int i = 0; i < n_cmds; i++)
        waitpid(pids[i], NULL, 0);

    return output ? output : strdup("");
}
