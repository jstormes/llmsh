#ifndef LLMSH_HISTORY_H
#define LLMSH_HISTORY_H

/* Manage conversation history for LLM context */

void history_init(void);
void history_cleanup(void);

void history_add_user(const char *text);
void history_add_assistant(const char *text);
void history_add_tool_result(const char *tool_name, const char *result);

/* Build the messages JSON array for the API call. Caller frees. */
char *history_build_messages(const char *system_prompt);

#endif /* LLMSH_HISTORY_H */
