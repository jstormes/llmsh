# Chat I/O: A Fifth File Descriptor for AI-Native Shells

## Motivation

Unix gives every process three file descriptors:

| fd | Name | Direction | Purpose |
|----|------|-----------|---------|
| 0 | stdin | in | Program input (data) |
| 1 | stdout | out | Program output (data) |
| 2 | stderr | out | Diagnostics and errors |

This model works well for programs that transform data. But an LLM-driven
shell has a fundamentally different communication channel: **conversation**.
The LLM's responses are not data output (stdout) and not errors (stderr) —
they're a dialogue with the user.

llmsh extends the Unix model with two additional file descriptors:

| fd | Name | Direction | Purpose |
|----|------|-----------|---------|
| 3 | chatout | out | LLM responses to the user |
| 4 | chatin | in | Instructions/context to the LLM |

## The Five Streams

```
                    ┌─────────────┐
  stdin (0) ──────→ │             │ ──────→ stdout (1)   tool/command output
                    │    llmsh    │
  chatin (4) ─────→ │             │ ──────→ stderr (2)   errors, confirmations
   (LLM context)    │             │
                    │             │ ──────→ chatout (3)   LLM responses
                    └─────────────┘
```

### stdin (fd 0) — User Input
Standard input. In interactive mode, the user types at the prompt. In piped
mode, data flows in as context for the LLM:

```bash
cat error.log | llmsh "what went wrong?"
```

### stdout (fd 1) — Tool Output
Output from commands and tools executed by the shell. When the LLM runs `ls`
or `grep`, their output goes here. This is the "data" stream — it can be
piped, redirected, and processed by other programs:

```bash
llmsh -v "find large files" | sort -n
```

### stderr (fd 2) — Errors and Status
Error messages, safety confirmation prompts, and diagnostic info. Follows
the standard Unix convention:

```bash
llmsh "do something" 2>errors.log
```

### chatout (fd 3) — LLM Responses
The LLM's conversational text — answers, explanations, summaries. This is
the "dialogue" stream. By default it goes to the terminal. It can be
redirected to capture the LLM's response:

```bash
llmsh "review this code" 3>review.txt
llmsh "explain the bug" 3>&1 | less
```

### chatin (fd 4) — LLM Context (proposed)
A channel for feeding persistent instructions, context, or system prompts
to the LLM from outside the interactive session. This separates
"instructions for the LLM" from "input data to process."

```bash
# Feed project-specific instructions
llmsh 4< .llmsh_prompt

# Set behavioral constraints
echo "always respond in French" | llmsh 4<&0

# Feed a persona or role
llmsh 4< personas/security-auditor.txt "review the code"
```

## Use Cases for chatin

### 1. Project-Specific Instructions
Each project directory can have a `.llmsh_prompt` file with instructions
tailored to that codebase:

```
# .llmsh_prompt
This is a C project using llama.cpp for LLM inference.
The code follows Linux kernel style. No C++ features.
Test with: make test
The API server runs on http://ai:8080
```

```bash
llmsh 4< .llmsh_prompt
```

### 2. Role/Persona Selection
Pre-written personas for different tasks:

```bash
# Security audit mode
llmsh 4< personas/security.txt "review auth.c"

# Code review mode
llmsh 4< personas/reviewer.txt "review the PR"

# Teaching mode
llmsh 4< personas/tutor.txt
```

### 3. LLM-to-LLM Composition
Chain LLMs where one's chat output feeds another's chat input:

```bash
# One LLM analyzes, another summarizes
llmsh "analyze the codebase" 3>&1 | llmsh 4<&0 "summarize this analysis"

# Expert panel: each LLM gets the previous one's output
llmsh 4< personas/architect.txt "design the API" 3>&1 \
  | llmsh 4< personas/security.txt "review this design" 3>&1 \
  | llmsh 4< personas/implementer.txt "implement this"
```

### 4. Automated Pipelines
Scripts that orchestrate the LLM with structured context:

```bash
#!/bin/bash
# CI review bot
git diff HEAD~1 | llmsh 4< ci/review-prompt.txt "review this diff" 3> review.md
```

### 5. Dynamic Context Injection
A parent process or monitoring tool feeds live context:

```bash
# Feed live system state as context
(while true; do uptime; sleep 60; done) | llmsh 4<&0 "monitor the system"
```

## Design Principles

### Follows Unix Conventions
- Each fd has a single purpose and direction
- Redirection works with standard shell syntax (`3>file`, `4<file`)
- Defaults are sensible (terminal for interactive, closed for scripts)
- Composable with pipes and other Unix tools

### Separation of Concerns
- **stdin** carries data to process
- **chatin** carries instructions for the LLM
- These are different: `cat file | llmsh "summarize"` sends data, while
  `llmsh 4< instructions.txt` sends behavioral context

### No Ambiguity
Without chatin, everything mixed into the prompt:
```bash
# Is this data or instructions? Ambiguous.
echo "Be concise. Also here is the log: $(cat error.log)" | llmsh "analyze"
```

With chatin:
```bash
# Clear separation
echo "Be concise" > /tmp/instructions
cat error.log | llmsh 4< /tmp/instructions "analyze this log"
```

## Implementation Notes

### chatout (fd 3) — Currently Implemented
- `streams_init()` opens `/dev/tty` on fd 3 if not already redirected
- `stream_chat_output()` writes LLM text to fd 3
- Streaming tokens go directly to fd 3 as they arrive
- User can redirect: `llmsh 3>file.txt`

### chatin (fd 4) — Proposed
Implementation approach:
1. In `streams_init()`, check if fd 4 is open
2. If open, read all content into a buffer (like piped stdin)
3. Prepend the chatin content to the system prompt as additional context
4. Close fd 4 after reading (one-shot context injection)
5. For persistent/streaming chatin: read in a background thread or check
   between agentic loop iterations

### Priority of Context
When multiple context sources exist:
1. System prompt (hardcoded in llm.c) — base behavior
2. chatin (fd 4) — project/session instructions
3. Conversation history — prior messages
4. User input (stdin/prompt) — current request

### Interaction with Existing Features
- **One-shot mode**: `llmsh 4< ctx.txt "question"` — chatin + query
- **Piped stdin**: `cat data | llmsh 4< ctx.txt "process"` — data + instructions
- **Hybrid pipes**: `ls | summarize 4< ctx.txt` — command output + LLM instructions
- **Server switching**: chatin persists across server switches (it's session context)

## Comparison with Existing Approaches

| Approach | How it works | Limitation |
|----------|-------------|------------|
| CLAUDE.md | File read by the tool | Tool-specific, not composable |
| .env files | Environment variables | String length limits, no structure |
| System prompts | Hardcoded in binary | Can't customize per-project |
| Config files | ~/.llmshrc settings | Settings, not free-form context |
| **chatin (fd 4)** | Standard fd redirection | Composable, Unix-native |

## Future Possibilities

### Bidirectional Chat Protocol
If chatin is a pipe (not a file), it could be a live channel:

```bash
# Interactive agent-to-agent communication
mkfifo /tmp/chat
llmsh-agent-a 3>/tmp/chat &
llmsh-agent-b 4</tmp/chat
```

### Chat as a Network Protocol
Extend to TCP/Unix sockets for remote chat channels:

```bash
llmsh 4< /dev/tcp/orchestrator/9000
```

### Multi-Agent Orchestration
A conductor process manages multiple llmsh instances via their chat fds:

```
conductor
  ├── llmsh (analyst)   4←─ "analyze X" ──→ 3 ─┐
  ├── llmsh (reviewer)  4←─ analyst output ──→ 3 ─┤
  └── llmsh (writer)    4←─ review output ──→ 3 ─→ final.md
```
