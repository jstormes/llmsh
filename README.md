# llmsh - Natural Language Shell

A minimal C shell that uses an LLM to translate natural language into Unix operations.

## Architecture

```
User (English) → LLM (tool calls) → Builtins / External Commands → Output
```

- **Built-in tools** (BusyBox-style): ls, cat, cp, mv, rm, mkdir, grep, head, wc, cd, pwd, read_file, write_file
- **External execution**: Any CLI command with full pipe and redirection support via the `run` tool
- **Safety tiers**: Auto (read-only), Confirm (writes), Danger (destructive/arbitrary exec)

## Building

```bash
# Requires libcurl-dev
sudo apt install libcurl4-openssl-dev   # Debian/Ubuntu

make
```

## Usage

```bash
# Set your LLM endpoint (defaults to localhost:8080)
export LLMSH_API_URL="http://localhost:8080/v1/chat/completions"
export LLMSH_MODEL="default"
export LLMSH_API_KEY="your-key"    # optional

./llmsh
```

Then just type in English:

```
/home/user llmsh> show me the largest files in this directory
/home/user llmsh> find all TODO comments in the source code
/home/user llmsh> make a backup of config.yaml
/home/user llmsh> count the lines of C code in src/
```

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `LLMSH_API_URL` | `http://localhost:8080/v1/chat/completions` | LLM API endpoint |
| `LLMSH_MODEL` | `default` | Model name |
| `LLMSH_API_KEY` | (none) | Bearer token for API auth |

## License

MIT
