#ifndef LLMSH_STREAMS_H
#define LLMSH_STREAMS_H

#include <stdio.h>

#define LLMSH_FD_CHAT 3

/* Global FILE* for chat output (fd 3) */
extern FILE *stdchat;

/* Whether tool output (fd 1) should be displayed. 1 = show (default), 0 = hidden */
extern int streams_verbose;

/* Initialize streams: set up fd 3 for chat output */
void streams_init(void);

/* Cleanup */
void streams_cleanup(void);

/* Write tool output to stdout (only if verbose) */
void stream_tool_output(const char *text);

/* Write LLM chat text to stdchat (fd 3) */
void stream_chat_output(const char *text);

#endif /* LLMSH_STREAMS_H */
