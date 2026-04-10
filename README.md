# Claudius

**Lightweight C++ agent orchestrator for the Claude API.**

- Talks to the Claude API over raw TLS (no libcurl, no HTTP library)
- Enforces a master constitution — formal, terse, token-efficient (derived from [JuliusBrussee/caveman](https://github.com/JuliusBrussee/caveman)) — cutting ~75% of output tokens
- Supports per-agent constitutions with custom personality, goals, rules, and three brevity levels: `lite`, `full`, `ultra`
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
claudius --init
claudius
```

### From source

```bash
git clone https://github.com/tylerreckart/claudius.git
cd claudius
./install.sh
```

The install script handles dependencies (`brew bundle` on macOS, `apt`/`dnf` on Linux), builds a release binary, installs `claudius` and `claudius-cli` to `/usr/local/bin`, and runs `claudius --init` on first install.

### Manual build

#### macOS

```bash
brew bundle
mkdir build && cd build
cmake .. -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
make -j$(sysctl -n hw.ncpu)
sudo cp claudius /usr/local/bin/
```

#### Linux (Ubuntu/Debian)

```bash
sudo apt install cmake libssl-dev build-essential
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo cp claudius /usr/local/bin/
```

#### Debug build with AddressSanitizer

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCLAUDIUS_ASAN=ON
make -j$(nproc)
```

## Quick Start

```bash
# Set your API key
export ANTHROPIC_API_KEY="sk-ant-..."

# Initialize config, generate auth token, create example agents
claudius --init

# Interactive mode
claudius

# Or start server for remote access
claudius --serve --port 9077
```

## Usage Modes

### Interactive REPL

```
$ claudius
[claudius] > hello, what agents are available?
Three agents loaded: reviewer, researcher, devops.
  [in:342 out:15]

[claudius] > /use reviewer
Switched to: reviewer

[reviewer] > check this: if (x = 5) return true;
Assignment, not comparison. `x = 5` → `x == 5`. Fix: `if (x == 5)`.
  [in:198 out:20]

[claudius] > /status
SYSTEM STATUS
agents:3 | total_in:540 total_out:35
  reviewer | code-reviewer | msgs:2 | in:198 out:20 | reqs:1
  researcher | research-analyst | msgs:0 | in:0 out:0 | reqs:0
  devops | infrastructure-engineer | msgs:0 | in:0 out:0 | reqs:0
```

### Server Mode (remote access)

```bash
# Start server
claudius --serve --port 9077

# From another machine
claudius-cli myserver.local 9077 <your-token>

# Or manually with nc
nc myserver.local 9077
AUTH <your-token>
SEND reviewer check this function for bugs: void f() { int* p; *p = 5; }
QUIT
```

### One-shot

```bash
claudius --send reviewer "review: if (arr.length = 0) return;"
```

## Server Protocol

Line-based TCP protocol. All commands are newline-terminated.

| Command | Description |
|---------|-------------|
| `AUTH <token>` | Authenticate (required first) |
| `SEND <agent> <msg>` | Send message to agent |
| `ASK <query>` | Ask Claudius master about system state |
| `LIST` | List agents |
| `STATUS` | Full system status |
| `CREATE <id> [json]` | Create agent with optional JSON config |
| `REMOVE <id>` | Remove agent |
| `RESET <id>` | Clear agent history |
| `TOKENS` | Global token usage |
| `HELP` | Command list |
| `QUIT` | Disconnect |

Responses prefixed with `OK` or `ERR`.

## Agent Constitution

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

### Brevity Levels

| Level | Style | Token Savings |
|-------|-------|--------------|
| `lite` | Full grammar, no filler or hedging. Professional prose. | ~40% |
| `full` | Drop articles, fragments permitted. Short, declarative. | ~65% |
| `ultra` | Maximum compression. Abbreviations, arrows, minimal words. | ~75% |

### Constitution Layering

Every agent's system prompt is built in layers:

1. **Claudius base** — voice rules, compression doctrine, brevity mode, exception handling
2. **Identity** — agent name and role
3. **Goal** — the agent's governing objective
4. **Rules** — explicit behavioral constraints

The master Claudius agent uses the same system but is configured for orchestration and meta-queries about system state.

## File Structure

```
~/.claudius/
├── api_key              # Anthropic API key
├── auth_tokens          # SHA-256 hashed access tokens
└── agents/
    ├── reviewer.json    # Code review (ultra)
    ├── researcher.json  # Research analyst (lite)
    └── devops.json      # Infrastructure (full)
```

## License

MIT
