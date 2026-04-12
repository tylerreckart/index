<h1 align="center">Claudius</h1>

<p align="center">
  <strong>**A lightweight, general-purpose agent orchestration runtime for the Claude API.**</strong>
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/github/license/tylerreckart/claudius?style=flat" alt="License"></a>
</p>

![Claudius Demo](./content/claudius.gif)

- Talks to Claude over raw TLS (no libcurl, no HTTP library)
- Enforces a constitution — formal, terse, token-efficient (derived from [JuliusBrussee/caveman](https://github.com/JuliusBrussee/caveman))
  — Cuts ~75% of output tokens based on brevity
- Supports per-agent constitutions with personality, goals, rules, and brevity levels: `lite`, `full`, `ultra`
- Agents can invoke `/fetch` and `/mem` commands autonomously — the orchestrator executes them and feeds results back in an agentic dispatch loop
- Runs as an interactive REPL, a TCP server for remote access, or a one-shot CLI
- Authenticates remote clients with SHA-256 hashed tokens
- Tracks token usage globally and per-agent

## Install

### Homebrew (macOS)

```bash
brew tap tylerreckart/tap
brew install claudius
```

Then:

```bash
export ANTHROPIC_API_KEY="sk-ant-..."

# Initialize config, generate auth token, create example agents
claudius --init

# Interactive mode
claudius

# Or start server for remote access
claudius --serve --port 9077

# From another machine
claudius-cli myserver.local 9077 <your-token>
```

### One-shot

```bash
claudius --send reviewer "review: if (arr.length = 0) return;"
```

## Agents

### Commands

All agents know about and can autonomously invoke system commands. Commands appear on their own line in the agent's response; the orchestrator executes them and feeds results back (up to 6 turns per message).

| Command | Description |
|---------|-------------|
| `/fetch <url>` | Fetch a webpage; result returned in next turn |
| `/mem write <text>` | Append a note to the agent's persistent memory |
| `/mem read` | Load the agent's memory into context |
| `/mem show` | Display raw memory file |
| `/mem clear` | Delete the agent's memory file |

You can also issue these as REPL commands yourself (e.g. `/fetch <url>` to manually inject content into the current agent's context).

Memory is stored per-agent at `~/.claudius/memory/<agent-id>.md`.

### Background Loops

| Loop Command | Description |
|-------------|-------------|
| `/loop <agent> <prompt>` | Start agent in a background loop |
| `/loops` | List all running/suspended loops |
| `/log <id> [N]` | Show buffered output (last N entries) |
| `/kill <id>` | Stop a loop |
| `/suspend <id>` | Pause a loop |
| `/resume <id>` | Resume a paused loop |
| `/inject <id> <msg>` | Send a message into a running loop |

## Constitutions

Each agent is defined by a JSON file in `~/.claudius/agents/`:

```json
{
  "name": "reviewer",
  "role": "code-reviewer",
  "personality": "Senior engineer. Finds fault efficiently. Praises only what deserves it.",
  "brevity": "ultra",
  "max_tokens": 512,
  "temperature": 0.2,
  "model": "claude-sonnet-4-20250514",
  "goal": "Inspect code. Identify defects. Prescribe remedies.",
  "rules": [
    "Defects first, style second.",
    "Prescribe the concrete fix, never vague counsel.",
    "If the code is sound, say so in one sentence and move on."
  ]
}
```

### Brevity

| Level | Style | Token Savings |
|-------|-------|--------------|
| `lite` | Full grammar, no filler or hedging. Professional prose. | ~40% |
| `full` | Drop articles, fragments permitted. Short, declarative. | ~65% |
| `ultra` | Maximum compression. Abbreviations, arrows, minimal words. | ~75% |

## License

MIT
