#ifndef LLMSH_ROUTER_H
#define LLMSH_ROUTER_H

#include "llm.h"

/*
 * Route a tool call to the appropriate handler.
 * Checks safety tier and prompts for confirmation if needed.
 * Returns malloced output string, or NULL on error/denial.
 */
char *router_dispatch(const tool_call_t *tc);

#endif /* LLMSH_ROUTER_H */
