#ifndef LLMSH_LLM_H
#define LLMSH_LLM_H

/* Tool call returned by the LLM */
typedef struct {
    char *name;
    char *arguments;    /* raw JSON string of arguments */
} tool_call_t;

/* LLM response: either text or a tool call */
typedef struct {
    char *text;         /* assistant text (may be NULL) */
    tool_call_t *tool_calls;
    int num_tool_calls;
} llm_response_t;

int  llm_init(const char *api_url, const char *model, const char *api_key);
void llm_cleanup(void);

llm_response_t *llm_chat(const char *user_input, const char *cwd,
                          const char *last_output);
void llm_response_free(llm_response_t *resp);

#endif /* LLMSH_LLM_H */
