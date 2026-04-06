#ifndef LLMSH_SHELL_H
#define LLMSH_SHELL_H

#include "llm.h"

/*
 * Run the agentic loop: execute tool calls, feed results back to the LLM,
 * repeat until the LLM responds with only text or max iterations hit.
 *
 * resp:     initial LLM response (consumed/freed by this function)
 * max_iter: maximum number of tool-call rounds
 * cbs:      streaming callbacks
 *
 * Returns: final text response (malloced), or NULL
 */
char *shell_agentic_loop(llm_response_t *resp, int max_iter,
                          llm_stream_cbs *cbs);

/*
 * Send a query to the LLM with optional piped context and run the
 * full agentic loop. Returns final text (malloced), or NULL.
 */
char *shell_query(const char *query, const char *context,
                  int max_iter, llm_stream_cbs *cbs);

/*
 * Execute a command line: split on |, run command segments directly,
 * hand off to LLM at first non-command segment.
 *
 * Returns: output string (malloced) — either command output or LLM answer
 */
char *shell_execute(const char *cmdline, int max_iter,
                     llm_stream_cbs *cbs);

/*
 * Handle a built-in slash command (/server, /clear, /verbose, etc.)
 * Returns 1 if handled, 0 if not a slash command.
 */
int shell_handle_slash(const char *line);

/*
 * Check if input is an exit command.
 */
int shell_is_exit(const char *line);

#endif /* LLMSH_SHELL_H */
