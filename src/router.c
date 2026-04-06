#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "router.h"
#include "builtin.h"
#include "exec.h"
#include "safety.h"
#include "config.h"
#include "cJSON.h"

/* Handle the special "run" tool for arbitrary command pipelines */
static char *handle_run(const char *args_json)
{
    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid arguments");

    cJSON *pipeline = cJSON_GetObjectItem(args, "pipeline");
    if (!pipeline || !cJSON_IsArray(pipeline)) {
        cJSON_Delete(args);
        return strdup("error: pipeline must be an array of commands");
    }

    int n = cJSON_GetArraySize(pipeline);
    const char **cmds = calloc(n, sizeof(char *));
    for (int i = 0; i < n; i++) {
        cJSON *cmd = cJSON_GetArrayItem(pipeline, i);
        cmds[i] = cmd ? cmd->valuestring : "";
    }

    /* Build description for confirmation */
    char desc[2048] = "Execute: ";
    for (int i = 0; i < n; i++) {
        if (i > 0) strcat(desc, " | ");
        strncat(desc, cmds[i], sizeof(desc) - strlen(desc) - 4);
    }

    cJSON *stdin_f  = cJSON_GetObjectItem(args, "stdin_file");
    cJSON *stdout_f = cJSON_GetObjectItem(args, "stdout_file");
    cJSON *append_j = cJSON_GetObjectItem(args, "append");

    if (stdin_f && cJSON_IsString(stdin_f)) {
        strncat(desc, " < ", sizeof(desc) - strlen(desc) - 1);
        strncat(desc, stdin_f->valuestring, sizeof(desc) - strlen(desc) - 1);
    }
    if (stdout_f && cJSON_IsString(stdout_f)) {
        int app = append_j && cJSON_IsTrue(append_j);
        strncat(desc, app ? " >> " : " > ", sizeof(desc) - strlen(desc) - 1);
        strncat(desc, stdout_f->valuestring, sizeof(desc) - strlen(desc) - 1);
    }

    /* Confirm before execution */
    if (!safety_confirm(desc)) {
        free(cmds);
        cJSON_Delete(args);
        return strdup("[denied by user]");
    }

    const char *sin  = (stdin_f && cJSON_IsString(stdin_f)) ? stdin_f->valuestring : NULL;
    const char *sout = (stdout_f && cJSON_IsString(stdout_f)) ? stdout_f->valuestring : NULL;
    int append = (append_j && cJSON_IsTrue(append_j));

    char *result = exec_pipeline(cmds, n, sin, sout, append);

    free(cmds);
    cJSON_Delete(args);
    return result ? result : strdup("(no output)");
}

char *router_dispatch(const tool_call_t *tc)
{
    if (!tc || !tc->name)
        return strdup("error: invalid tool call");

    /* Special case: run */
    if (strcmp(tc->name, "run") == 0)
        return handle_run(tc->arguments ? tc->arguments : "{}");

    /* Look up builtin */
    const builtin_t *bi = builtin_find(tc->name);
    if (!bi)
        return strdup("error: unknown tool");

    /* Check safety tier */
    if (bi->safety_tier >= SAFETY_CONFIRM) {
        char desc[512];
        snprintf(desc, sizeof(desc), "%s: %s",
                 bi->name, tc->arguments ? tc->arguments : "{}");
        if (!safety_confirm(desc))
            return strdup("[denied by user]");
    }

    return bi->handler(tc->arguments ? tc->arguments : "{}");
}
