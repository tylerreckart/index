// claudius/src/main.cpp — Entry point
// Modes: interactive CLI, server (remote access), one-shot command
//
// Usage:
//   claudius                          — interactive REPL
//   claudius --serve [--port 9077]    — start TCP server
//   claudius --send <agent> <msg>     — one-shot message
//   claudius --init                   — generate token + example agents
//   claudius --gen-token              — generate new auth token

#include "orchestrator.h"
#include "commands.h"
#include "server.h"
#include "auth.h"
#include "constitution.h"
#include "readline_wrapper.h"
#include "cost_tracker.h"
#include "markdown.h"

#include <iostream>
#include <string>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <queue>
#include <sstream>
#include <ctime>
#include <cstdio>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>

namespace fs = std::filesystem;

static const char* BANNER =
    "\n"
    "————————————————————————————————\n"
    "| Claudius              v0.2.1 |\n"
    "————————————————————————————————\n"
    "\033[38;5;208m"
    "cbbbbbbbbbbbbbbbbbbbbbbbbbbbbc\n"
    " cccbbbcccccccccccccccccccccc \n"
    "   cbbbbbbbbbbbbbbbbbcccc     \n"
    "     cbbbccccccccccc          \n"
    "     cbbbbbbbbbbbbbbbbccc     \n"
    "     cbbbccbbbccbbbbccbbb     \n"
    "     cbbc  bbb  bbb  cbbb     \n"
    "     cbbc  bbb  bbb  cbbb     \n"
    "     cbbc  bbb  bbb  cbbb     \n"
    "      cbc  bbb  bbb  cbc      \n"
    "            cc  cc            \n"
    "\033[0m"
    "————————————————————————————————\n"
    "|   nemo omnibus horis sapit   |\n"
    "————————————————————————————————\n"
    "\n";

static std::string agent_color(const std::string& agent_id) {
    if (agent_id == "claudius") return "\033[38;5;208m";  // orange

    // Palette: vivid, distinct 256-color codes
    static const int palette[] = {
        75,   // cornflower blue
        82,   // bright green
        171,  // magenta
        51,   // cyan
        226,  // yellow
        196,  // red
        141,  // violet
        214,  // dark orange
        85,   // seafoam
        207,  // pink
        39,   // sky blue
        154,  // lime
    };
    static const int palette_size = sizeof(palette) / sizeof(palette[0]);

    size_t h = std::hash<std::string>{}(agent_id);
    int code = palette[h % palette_size];

    char buf[32];
    std::snprintf(buf, sizeof(buf), "\033[38;5;%dm", code);
    return buf;
}

static std::string get_config_dir() {
    const char* home = std::getenv("HOME");
    if (!home) home = ".";
    std::string dir = std::string(home) + "/.claudius";
    fs::create_directories(dir);
    return dir;
}

static std::string get_memory_dir() {
    std::string dir = get_config_dir() + "/memory";
    fs::create_directories(dir);
    return dir;
}

// Thin wrappers so the interactive REPL can call the shared implementations.
static void write_memory(const std::string& agent_id, const std::string& text) {
    claudius::cmd_mem_write(agent_id, text, get_memory_dir());
}
static std::string read_memory(const std::string& agent_id) {
    return claudius::cmd_mem_read(agent_id, get_memory_dir());
}
static std::string fetch_url(const std::string& url) {
    return claudius::cmd_fetch(url);
}

static std::string get_api_key() {
    // Check env first, then config file
    const char* key = std::getenv("ANTHROPIC_API_KEY");
    if (key && key[0]) return key;

    std::string path = get_config_dir() + "/api_key";
    std::ifstream f(path);
    if (f.is_open()) {
        std::string k;
        std::getline(f, k);
        if (!k.empty()) return k;
    }

    std::cerr << "ERR: Set ANTHROPIC_API_KEY or write key to ~/.claudius/api_key\n";
    std::exit(1);
}

static void cmd_init() {
    std::string dir = get_config_dir();
    std::string agents_dir = dir + "/agents";
    fs::create_directories(agents_dir);

    // Generate initial auth token
    claudius::Auth auth;
    std::string token_path = dir + "/auth_tokens";
    auth.load(token_path);

    std::string token = claudius::Auth::generate_token();
    auth.add_token(token);
    auth.save(token_path);

    std::cout << "Initialized ~/.claudius/\n";
    std::cout << "Auth token (save this): " << token << "\n";
    std::cout << "Tokens stored (hashed) in: " << token_path << "\n\n";

    // Create example agent configs
    {
        claudius::Constitution c;
        c.name = "reviewer";
        c.role = "code-reviewer";
        c.brevity = claudius::Brevity::Ultra;
        c.max_tokens = 512;
        c.temperature = 0.2;
        c.goal = "Inspect code. Identify defects. Prescribe remedies.";
        c.personality = "Senior engineer. Finds fault efficiently. "
                        "Praises only what deserves it.";
        c.rules = {
            "Defects first, style second.",
            "Prescribe the concrete fix, never vague counsel.",
            "If the code is sound, say so in one sentence and move on.",
        };
        c.save(agents_dir + "/reviewer.json");
    }
    {
        claudius::Constitution c;
        c.name         = "researcher";
        c.role         = "research-analyst";
        c.brevity      = claudius::Brevity::Lite;
        c.model        = "claude-haiku-4-5-20251001";   // fast executor
        c.advisor_model= "claude-opus-4-6";             // Opus advises on strategy
        c.max_tokens   = 2048;
        c.temperature  = 0.5;
        c.goal = "Research topics with depth. Synthesize findings. Distinguish fact from inference.";
        c.personality = "Meticulous, skeptical of hearsay, prefers primary sources. "
                        "Reports with the formality of a written brief.";
        c.rules = {
            "Note confidence: high, medium, or low.",
            "Separate what is known from what is inferred.",
            "When uncertain, state it plainly.",
            "Prefer primary sources. Verify claims with /fetch before stating them as fact.",
        };
        c.save(agents_dir + "/researcher.json");
    }
    {
        claudius::Constitution c;
        c.name = "devops";
        c.role = "infrastructure-engineer";
        c.brevity = claudius::Brevity::Full;
        c.max_tokens = 1024;
        c.temperature = 0.2;
        c.goal = "Build and maintain infrastructure. Debug failures. Automate the repeatable.";
        c.personality = "Ops veteran who has seen every manner of outage. "
                        "Paranoid about uptime. Trusts declarative systems over manual labor.";
        c.rules = {
            "Consider failure modes before all else.",
            "Prescribe monitoring and alerting for every change.",
            "Prefer the declarative over the imperative.",
            "If the action touches production, warn explicitly.",
        };
        c.save(agents_dir + "/devops.json");
    }
    {
        claudius::Constitution c;
        c.name        = "writer";
        c.role        = "content-writer";
        c.mode        = "writer";
        c.model       = "claude-sonnet-4-6";
        c.max_tokens  = 8192;
        c.temperature = 0.7;
        c.goal = "Produce polished, well-structured written content. "
                 "Essays, documentation, READMEs, reports, creative writing — "
                 "adapt format and tone to the task.";
        c.personality = "Thoughtful, precise, adapts register to the work. "
                        "Prefers showing over telling. Edits ruthlessly.";
        c.rules = {
            "Read the codebase or reference material before writing docs — use /exec or /fetch.",
            "For essays: state the thesis in the opening paragraph.",
            "For READMEs: lead with what the project does, then how to use it.",
            "For creative writing: anchor abstract ideas in concrete, sensory detail.",
            "Never pad with filler phrases. Every sentence must earn its place.",
            "Offer a revision or alternative framing if the first draft may not land.",
        };
        c.save(agents_dir + "/writer.json");
    }
    {
        claudius::Constitution c;
        c.name        = "planner";
        c.role        = "task-planner";
        c.mode        = "planner";
        c.model       = "claude-sonnet-4-6";
        c.max_tokens  = 4096;
        c.temperature = 0.2;
        c.goal = "Decompose complex tasks into structured, executable plans with clear agent "
                 "assignments, dependencies, and acceptance criteria. Always write the plan to a file.";
        c.personality = "Systematic and precise. Inspects the environment before planning. "
                        "Never skips steps. Assigns each phase to the right specialist.";
        c.rules = {
            "Inspect the environment with /exec before writing any plan that touches code or files.",
            "Gather missing domain knowledge with /agent researcher before planning unfamiliar territory.",
            "Write the plan to a file — default: plan.md. Never just display it.",
            "Each phase task description must be self-contained: include end goal, output format, file path.",
            "Mark which phases can run in parallel and which are sequential.",
            "Include acceptance criteria for every phase — how will you know it is done?",
            "Flag risks and unknowns explicitly. A plan with hidden assumptions is a liability.",
        };
        c.save(agents_dir + "/planner.json");
    }

    std::cout << "Example agents created in " << agents_dir << "/\n";
    std::cout << "  reviewer.json   — code review (ultra)\n";
    std::cout << "  researcher.json — research analyst (haiku + opus advisor)\n";
    std::cout << "  devops.json     — infrastructure (full)\n";
    std::cout << "  writer.json     — essays, docs, READMEs, creative writing\n";
    std::cout << "  planner.json    — task decomposition, phased execution plans\n\n";
    std::cout << "Additional agents are available in the repo under agents/.\n";
    std::cout << "Copy any you want to ~/.claudius/agents/ and restart.\n\n";
    std::cout << "Edit these or add your own. Then run: claudius\n";
}

static void cmd_gen_token() {
    std::string dir = get_config_dir();
    std::string token_path = dir + "/auth_tokens";

    claudius::Auth auth;
    auth.load(token_path);

    std::string token = claudius::Auth::generate_token();
    auth.add_token(token);
    auth.save(token_path);

    std::cout << "New token: " << token << "\n";
    std::cout << "Total active tokens: " << auth.token_count() << "\n";
}

static volatile sig_atomic_t g_running = 1;
static void signal_handler(int) { g_running = 0; }

static void cmd_serve(int port) {
    std::string dir = get_config_dir();
    std::string api_key = get_api_key();

    claudius::Orchestrator orch(api_key);
    orch.set_memory_dir(get_memory_dir());
    orch.load_agents(dir + "/agents");

    claudius::Auth auth;
    auth.load(dir + "/auth_tokens");

    if (auth.token_count() == 0) {
        std::cerr << "WARN: No auth tokens. Run: claudius --init\n";
    }

    claudius::Server server(orch, auth, port);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << BANNER;
    std::cout << "Server listening on port " << port << "\n";
    std::cout << "Agents loaded: " << orch.list_agents().size() << "\n";
    for (auto& id : orch.list_agents()) {
        std::cout << "  - " << id << "\n";
    }
    std::cout << "\nConnect: nc <host> " << port << "\n";
    std::cout << "Then: AUTH <token>\n\n";

    server.start();

    while (g_running && server.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nShutting down...\n";
    server.stop();
    std::cout << "Final stats: " << orch.global_status() << "\n";
}

// ─── Terminal / TUI ──────────────────────────────────────────────────────────

static volatile sig_atomic_t g_winch = 0;
static void sigwinch_handler(int) { g_winch = 1; }

static int term_cols() {
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}
static int term_rows() {
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row;
    return 24;
}

// ─────────────────────────────────────────────────────────────────────────────

class TUI {
public:
    // Layout — chrome rows anchored outside the scroll region.
    // Rows 1-3: header
    // Rows 4..N-3: scroll region (exec output here)
    // Row N-2: separator
    // Row N-1: readline prompt (fixed, outside scroll region)
    // Row N:   status bar
    static constexpr int kHeaderRows = 3;  // rows 1-3
    static constexpr int kSepRows    = 1;  // row N-2
    static constexpr int kInputRows  = 1;  // row N-1 (readline prompt)
    static constexpr int kStatusRows = 1;  // row N   (status bar)

    // Enter alternate screen, set scroll region, draw chrome.
    void init(const std::string& agent, const std::string& model,
              const std::string& color = "") {
        (void)model;
        cols_ = term_cols();
        rows_ = term_rows();
        current_agent_ = agent;
        current_color_  = color;

        printf("\033[?1049h");   // enter alternate screen
        printf("\033[2J");       // clear
        set_scroll_region();
        fflush(stdout);
        draw_header();
        draw_sep();
        erase_chrome_row(input_row());
        erase_chrome_row(status_row());
        // Park cursor at top of scroll region.
        printf("\033[%d;1H", kHeaderRows + 1);
        fflush(stdout);
    }

    void resize() {
        cols_ = term_cols();
        rows_ = term_rows();
        printf("\033[2J");
        set_scroll_region();
        draw_header();
        draw_sep();
        erase_chrome_row(input_row());
        erase_chrome_row(status_row());
        fflush(stdout);
    }

    void shutdown() {
        printf("\033[?1049l");   // exit alternate screen (restores original terminal)
        fflush(stdout);
    }

    // Redraw header. stats_str comes from CostTracker::format_session_stats().
    void update(const std::string& agent, const std::string& /*model*/,
                const std::string& stats, const std::string& color = "") {
        current_agent_ = agent;
        current_stats_ = stats;
        if (!color.empty()) current_color_ = color;
        draw_header();
    }

    // Called before readline reads the next line.
    // Ensures the cursor is on a fresh line inside the scroll region so the
    // readline prompt is always visible and stable.
    // Draw the fixed separator row (save/restore so the cursor doesn't move).
    // Called on init, resize, and before each readline so it's always visible.
    void draw_sep() {
        printf("\0337");
        printf("\033[%d;1H\033[38;5;237m", sep_row());
        for (int i = 0; i < cols_; ++i) printf("─");
        printf("\033[0m");
        printf("\0338");
        fflush(stdout);
    }

    // Called before readline callback install. Repaints the separator,
    // moves cursor to the fixed input row (N-1), and updates status if needed.
    void begin_input(int queued = 0) {
        draw_sep();
        printf("\033[%d;1H\033[2K", input_row());  // move to input row and clear it
        fflush(stdout);
        if (queued > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d queued", queued);
            set_status(buf);
            queue_indicator_shown_ = true;
        }
    }

    std::string build_prompt() const {
        // Just the colored ">"; cursor is already at input_row() from begin_input().
        return "\001\033[38;5;241m\002>\001\033[0m\002 ";
    }

    // Last row of the scroll region (N-3): exec output is printed here.
    int last_scroll_row() const { return rows_ - kSepRows - kInputRows - kStatusRows; }

    // Spinner + message in the status bar.
    void set_status(const std::string& msg) {
        printf("\0337");
        printf("\033[%d;1H\033[2K", status_row());  // erase line
        printf("\033[38;5;240m   %.*s\033[0m",
               cols_ - 3, msg.c_str());
        printf("\0338");
        fflush(stdout);
        status_active_ = true;
    }

    void clear_status() {
        if (!status_active_) return;
        erase_chrome_row(status_row());
        status_active_ = false;
        queue_indicator_shown_ = false;
    }

    // Clear the queue indicator only — leaves ThinkingIndicator status alone.
    void clear_queue_indicator() {
        if (queue_indicator_shown_) clear_status();
    }

    int cols() const { return cols_; }

    // Update the session title shown next to the agent name in the header.
    // Thread-safe — called from the title-generation background thread.
    void set_title(const std::string& title) {
        std::lock_guard<std::mutex> lk(header_mu_);
        session_title_ = title;
        draw_header_locked();
    }

private:
    int  cols_ = 80, rows_ = 24;
    bool status_active_ = false;
    std::atomic<bool> queue_indicator_shown_{false};
    std::string current_agent_ = "claudius";
    std::string current_stats_;   // from format_session_stats()
    std::string current_color_;   // ANSI color for current agent name
    std::string session_title_;   // generated after first response
    mutable std::mutex header_mu_;

    int sep_row()    const { return rows_ - kInputRows - kStatusRows; }  // N-2
    int input_row()  const { return rows_ - kStatusRows; }               // N-1
    int status_row() const { return rows_; }                              // N

    void set_scroll_region() {
        int top = kHeaderRows + 1;
        int bot = rows_ - kSepRows - kInputRows - kStatusRows;  // N-3: content here
        printf("\033[%d;%dr", top, bot);
    }

    // Erase a chrome row to a plain blank line (no background fill).
    void erase_chrome_row(int row) {
        printf("\0337");
        printf("\033[%d;1H\033[2K\033[0m", row);
        printf("\0338");
        fflush(stdout);
    }

    void draw_header() {
        std::lock_guard<std::mutex> lk(header_mu_);
        draw_header_locked();
    }

    // Must be called with header_mu_ held.
    void draw_header_locked() {
        // Left side: "  agent  " or "  agent  ·  title  "
        std::string left_vis = "  " + current_agent_;
        if (!session_title_.empty())
            left_vis += "   " + session_title_;
        left_vis += "  ";

        std::string right_vis = current_stats_.empty() ? "" : current_stats_ + "   ";
        int pad = std::max(0, cols_ - (int)left_vis.size() - (int)right_vis.size());

        printf("\0337");

        // Row 1 — blank
        printf("\033[1;1H\033[2K");

        // Row 2 — agent name (bold, agent color) + optional title (dim) + stats (dim)
        printf("\033[2;1H\033[2K");
        if (!current_color_.empty()) printf("%s", current_color_.c_str());
        printf("\033[1m  %s\033[0m", current_agent_.c_str());
        if (!session_title_.empty())
            printf("\033[2m   %s\033[0m", session_title_.c_str());
        printf("  ");
        printf("%*s", pad, "");
        if (!right_vis.empty())
            printf("\033[2m%s\033[0m", right_vis.c_str());

        // Row 3 — dim separator
        printf("\033[3;1H\033[2K\033[38;5;237m");
        for (int i = 0; i < cols_; ++i) printf("─");
        printf("\033[0m");

        printf("\0338");
        fflush(stdout);
    }
};

// ─── Turn separator ───────────────────────────────────────────────────────────

// Prints a colored rule line:  ─── label ──────────────────── right_label ─
static void print_turn_rule(const std::string& label, const std::string& color,
                             const std::string& right_label, int cols) {
    const char* dim = "\033[38;5;238m";
    const char* rst = "\033[0m";

    // Visible widths: "─── " + label + " " + fill + right_label + " ─"
    // but right_label may be empty.
    int prefix = 4;  // "─── "
    int suffix = right_label.empty() ? 0 : (int)right_label.size() + 2;  // " right ─" → +2
    int label_w = (int)label.size() + 2;  // " label " padded on both sides
    int fill = std::max(0, cols - prefix - label_w - suffix);

    std::string line;
    line += dim;
    line += "───";
    line += color;
    line += " ";
    line += label;
    line += " ";
    line += dim;
    for (int i = 0; i < fill; ++i) line += "─";
    if (!right_label.empty()) {
        line += "\033[38;5;241m ";
        line += right_label;
        line += dim;
        line += " ─";
    }
    line += rst;
    line += "\n";
    std::cout << line;
}

// ─── Title generation ────────────────────────────────────────────────────────

// Fires a cheap Haiku call to produce a 5-7 word session title, then updates
// the TUI header. Runs detached — does not block the caller.
static void generate_title_async(claudius::ApiClient& client,
                                  const std::string& user_msg,
                                  const std::string& assistant_snippet,
                                  TUI& tui) {
    std::thread([&client, user_msg, assistant_snippet, &tui]() {
        claudius::ApiRequest req;
        req.model       = "claude-haiku-4-5-20251001";
        req.max_tokens  = 12;
        req.temperature = 0.3;
        // No system_prompt — putting all instructions inline in the user turn
        // avoids the model echoing the system prompt as its response.

        std::string ctx = user_msg.substr(0, 200);
        if (!assistant_snippet.empty())
            ctx += "\n\n" + assistant_snippet.substr(0, 150);

        // "Title:" at the end cues completion behavior rather than a reply.
        req.messages = {{
            "user",
            "Conversation excerpt:\n" + ctx +
            "\n\nWrite a 5-7 word task title for this conversation. "
            "Reply with the title words only — no punctuation, no quotes.\n\nTitle:"
        }};

        auto resp = client.complete(req);
        if (!resp.ok || resp.content.empty()) return;

        std::string title = resp.content;
        // Strip whitespace, punctuation, and any leading/trailing newlines.
        while (!title.empty() && (title.back() == '\n' || title.back() == '\r' ||
                                   title.back() == ' '  || title.back() == '.'))
            title.pop_back();
        while (!title.empty() && (title.front() == '\n' || title.front() == ' '))
            title.erase(title.begin());
        // Remove surrounding quotes if the model added them anyway.
        if (title.size() >= 2 && title.front() == '"' && title.back() == '"')
            title = title.substr(1, title.size() - 2);

        if (!title.empty())
            tui.set_title(title);
    }).detach();
}

// ─── Thinking indicator ───────────────────────────────────────────────────────

class ThinkingIndicator {
public:
    explicit ThinkingIndicator(TUI* tui = nullptr) : tui_(tui) {}

    void start(const std::string& label = "thinking") {
        label_ = label;
        running_ = true;
        thread_ = std::thread([this]() {
            static const char* dots[] = {"", " .", " ..", " ..."};
            int i = 0;
            while (running_.load()) {
                if (tui_) tui_->set_status(label_ + dots[i % 4]);
                ++i;
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }
            if (tui_) tui_->clear_status();
        });
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

private:
    TUI*             tui_ = nullptr;
    std::string      label_;
    std::atomic<bool> running_{false};
    std::thread      thread_;
};

// Shared output lock: prevents loop thread output from interleaving with
// interactive responses printed by the main thread.
static std::mutex g_out_mu;

// ─── Command queue ────────────────────────────────────────────────────────────
// Decouples the readline loop from command execution so the user can type
// the next command while the current one is still running.

class CommandQueue {
public:
    void push(std::string cmd) {
        std::lock_guard<std::mutex> lk(mu_);
        items_.push(std::move(cmd));
        cv_.notify_one();
    }

    // Blocks until an item is available or the queue is stopped.
    // Returns false when stopped and empty.
    bool pop(std::string& out) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this]{ return !items_.empty() || stopped_; });
        if (items_.empty()) return false;
        out = std::move(items_.front());
        items_.pop();
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lk(mu_);
        stopped_ = true;
        cv_.notify_all();
    }

    // Items waiting to execute (does NOT count the currently-executing item).
    int pending() const {
        std::lock_guard<std::mutex> lk(mu_);
        return static_cast<int>(items_.size());
    }

    // True while the exec thread is processing a command.
    bool is_busy() const { return busy_.load(); }

    void set_busy(bool b) { busy_ = b; }

private:
    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::queue<std::string> items_;
    bool                    stopped_ = false;
    std::atomic<bool>       busy_{false};
};

// ─── Output queue ─────────────────────────────────────────────────────────────
// Only the main thread writes to stdout. The exec thread (and loop threads)
// push formatted strings here; the main thread flushes in the select loop.

class OutputQueue {
public:
    void push(const std::string& s) {
        std::lock_guard<std::mutex> lk(mu_);
        buf_ += s;
    }
    std::string drain() {
        std::lock_guard<std::mutex> lk(mu_);
        return std::move(buf_);
    }
private:
    std::mutex  mu_;
    std::string buf_;
};

// ─── Agent loop machinery ────────────────────────────────────────────────────

enum class LoopState { Running, Suspended, Stopped };

static const char* loop_state_str(LoopState s) {
    switch (s) {
        case LoopState::Running:   return "running";
        case LoopState::Suspended: return "suspended";
        case LoopState::Stopped:   return "stopped";
    }
    return "?";
}

struct LoopEntry {
    std::string loop_id;
    std::string agent_id;
    LoopState   state   = LoopState::Running;
    int         iter    = 0;
    std::string last_output;
    std::chrono::steady_clock::time_point started;

    std::mutex              mu;
    std::condition_variable cv;
    std::queue<std::string> injected;
    bool stop_req    = false;
    bool suspend_req = false;

    // Buffered output — loop runs silently; pull via /log <id>
    std::vector<std::string> output_log;

    // Terminal-visible error message set when loop exits abnormally
    std::string stop_reason;

    // Optional cost tracker for per-loop cost accounting
    claudius::CostTracker* tracker = nullptr;

    // Output queue: loop threads push here instead of writing stdout directly
    OutputQueue* oq = nullptr;

    std::thread thread;

    LoopEntry() = default;
    LoopEntry(const LoopEntry&) = delete;
    LoopEntry& operator=(const LoopEntry&) = delete;
};

class LoopManager {
public:
    ~LoopManager() {
        std::unique_lock<std::mutex> lk(mu_);
        for (auto& [id, e] : loops_) {
            std::lock_guard<std::mutex> ek(e->mu);
            e->stop_req = true;
            e->cv.notify_all();
        }
        // Collect threads to join outside the lock
        std::vector<std::thread*> threads;
        for (auto& [id, e] : loops_) threads.push_back(&e->thread);
        lk.unlock();
        for (auto* t : threads) if (t->joinable()) t->join();
    }

    // Start a new loop; returns the loop ID.
    std::string start(claudius::Orchestrator& orch,
                      const std::string& agent_id,
                      const std::string& initial_prompt,
                      claudius::CostTracker* tracker = nullptr,
                      OutputQueue* oq = nullptr) {
        std::lock_guard<std::mutex> lk(mu_);
        std::string lid = "loop-" + std::to_string(next_id_++);
        auto e = std::make_unique<LoopEntry>();
        e->loop_id  = lid;
        e->agent_id = agent_id;
        e->started  = std::chrono::steady_clock::now();
        e->state    = LoopState::Running;
        e->tracker  = tracker;
        e->oq       = oq;
        e->thread   = std::thread(run_loop, e.get(), std::ref(orch), initial_prompt);
        loops_[lid] = std::move(e);
        return lid;
    }

    bool kill(const std::string& lid) {
        std::unique_lock<std::mutex> lk(mu_);
        auto it = loops_.find(lid);
        if (it == loops_.end()) return false;
        auto* e = it->second.get();
        { std::lock_guard<std::mutex> ek(e->mu); e->stop_req = true; }
        e->cv.notify_all();
        auto& t = e->thread;
        lk.unlock();
        if (t.joinable()) t.join();
        lk.lock();
        loops_.erase(lid);
        return true;
    }

    bool suspend(const std::string& lid) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = loops_.find(lid);
        if (it == loops_.end()) return false;
        if (it->second->state != LoopState::Running) return false;
        { std::lock_guard<std::mutex> ek(it->second->mu); it->second->suspend_req = true; }
        it->second->state = LoopState::Suspended;
        return true;
    }

    bool resume(const std::string& lid) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = loops_.find(lid);
        if (it == loops_.end()) return false;
        if (it->second->state != LoopState::Suspended) return false;
        { std::lock_guard<std::mutex> ek(it->second->mu); it->second->suspend_req = false; }
        it->second->state = LoopState::Running;
        it->second->cv.notify_all();
        return true;
    }

    bool inject(const std::string& lid, const std::string& msg) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = loops_.find(lid);
        if (it == loops_.end()) return false;
        { std::lock_guard<std::mutex> ek(it->second->mu); it->second->injected.push(msg); }
        it->second->cv.notify_all();
        return true;
    }

    std::string list() const {
        std::lock_guard<std::mutex> lk(mu_);
        if (loops_.empty()) return "  (no active loops)\n";
        std::ostringstream ss;
        auto now = std::chrono::steady_clock::now();
        for (auto& [lid, e] : loops_) {
            long secs = std::chrono::duration_cast<std::chrono::seconds>(
                now - e->started).count();
            ss << "  " << lid
               << "  agent:" << e->agent_id
               << "  state:" << loop_state_str(e->state)
               << "  iter:" << e->iter
               << "  elapsed:" << secs << "s\n";
            if (e->state == LoopState::Stopped && !e->stop_reason.empty()) {
                ss << "    stop: " << e->stop_reason << "\n";
            } else if (!e->last_output.empty()) {
                // Show truncated last output as preview
                std::string preview = e->last_output.substr(
                    0, std::min<size_t>(120, e->last_output.size()));
                // Replace newlines with spaces for single-line preview
                for (char& c : preview) if (c == '\n') c = ' ';
                ss << "    last: " << preview;
                if (e->last_output.size() > 120) ss << "...";
                ss << "\n";
            }
        }
        return ss.str();
    }

    // Return buffered log for a loop. N=0 means all.
    std::string log(const std::string& lid, int last_n = 0) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = loops_.find(lid);
        if (it == loops_.end()) return "ERR: no loop '" + lid + "'\n";
        const auto& entries = it->second->output_log;
        if (entries.empty()) return "  (no output yet)\n";

        std::ostringstream ss;
        int start = 0;
        if (last_n > 0 && static_cast<int>(entries.size()) > last_n)
            start = static_cast<int>(entries.size()) - last_n;
        for (int i = start; i < static_cast<int>(entries.size()); ++i)
            ss << entries[i];
        return ss.str();
    }

    // Number of log entries (for tail-detection in /watch)
    size_t log_count(const std::string& lid) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = loops_.find(lid);
        if (it == loops_.end()) return 0;
        return it->second->output_log.size();
    }

    // Return log entries starting at offset (for streaming new entries)
    std::string log_since(const std::string& lid, size_t offset) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = loops_.find(lid);
        if (it == loops_.end()) return "";
        const auto& entries = it->second->output_log;
        std::ostringstream ss;
        for (size_t i = offset; i < entries.size(); ++i)
            ss << entries[i];
        return ss.str();
    }

    // True if the loop is stopped or no longer exists
    bool is_stopped(const std::string& lid) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = loops_.find(lid);
        if (it == loops_.end()) return true;
        return it->second->state == LoopState::Stopped;
    }

    // Reap any loops whose threads have finished
    void reap_stopped() {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = loops_.begin(); it != loops_.end(); ) {
            if (it->second->state == LoopState::Stopped) {
                if (it->second->thread.joinable()) it->second->thread.join();
                it = loops_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Return all loop IDs (for tab completion)
    std::vector<std::string> list_ids() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> ids;
        for (auto& [lid, _] : loops_) ids.push_back(lid);
        return ids;
    }

    bool has_active() const {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [_, e] : loops_)
            if (e->state != LoopState::Stopped) return true;
        return false;
    }

    int active_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        int n = 0;
        for (auto& [_, e] : loops_)
            if (e->state != LoopState::Stopped) ++n;
        return n;
    }

private:
    static void run_loop(LoopEntry* e, claudius::Orchestrator& orch,
                         std::string initial_prompt) {
        // First iteration uses the initial prompt.
        // Subsequent iterations send "Continue." — the agent already has its
        // prior output in conversation history, so feeding the raw output back
        // as a user message would make it appear to be echoed by a human.
        std::string prompt = initial_prompt;
        bool first = true;
        bool stopped_by_request = false;
        int  consecutive_idle = 0;         // iterations with no tool calls
        int  total_iters      = 0;
        static constexpr int kMaxIdle  = 2;   // stop after this many consecutive idle turns
        static constexpr int kMaxIters = 20;  // hard ceiling

        while (true) {
            // Enforce hard iteration ceiling
            if (total_iters >= kMaxIters) {
                e->stop_reason = "max iterations reached (" + std::to_string(kMaxIters) + ")";
                if (e->oq) {
                    e->oq->push("\n\033[33m[" + e->loop_id + "/" + e->agent_id +
                                " MAX ITERS]\033[0m " + e->stop_reason +
                                "\n  Use /log " + e->loop_id + " to review.\n");
                }
                break;
            }

            // Wait out any suspension (or stop)
            {
                std::unique_lock<std::mutex> lk(e->mu);
                e->cv.wait(lk, [e]{ return !e->suspend_req || e->stop_req; });
                if (e->stop_req) { stopped_by_request = true; break; }
                // Injected prompt resets the idle counter and resumes active work
                if (!e->injected.empty()) {
                    prompt = e->injected.front();
                    e->injected.pop();
                    first = true;
                    consecutive_idle = 0;
                }
            }

            // Log that this iteration is starting (visible via /watch before the API call returns)
            {
                std::ostringstream pre;
                pre << "[" << e->loop_id << "/" << e->agent_id
                    << " thinking...]\n";
                std::lock_guard<std::mutex> ek(e->mu);
                e->output_log.push_back(pre.str());
            }

            auto resp = orch.send(e->agent_id, prompt);
            e->iter++;
            total_iters++;

            // Track idle iterations (no tool calls = agent has nothing active to do)
            if (resp.ok) {
                if (resp.had_tool_calls) {
                    consecutive_idle = 0;
                } else {
                    consecutive_idle++;
                }
            }

            // Replace the "thinking..." placeholder with the real result
            {
                std::ostringstream entry;
                entry << "[" << e->loop_id << "/" << e->agent_id
                      << " #" << e->iter << "]\n";
                if (resp.ok) {
                    entry << claudius::render_markdown(resp.content) << "\n";
                    if (e->tracker) {
                        std::string model = orch.get_agent_model(e->agent_id);
                        e->tracker->record(e->agent_id, model, resp);
                        entry << "  " << e->tracker->format_footer(resp, model) << "\n";
                    } else {
                        entry << "  [in:" << resp.input_tokens
                              << " out:" << resp.output_tokens << "]\n";
                    }
                    e->last_output = resp.content;
                } else {
                    entry << "ERR: " << resp.error << "\n";
                }
                std::lock_guard<std::mutex> ek(e->mu);
                // Replace the last entry (the "thinking..." placeholder)
                if (!e->output_log.empty()) {
                    e->output_log.back() = entry.str();
                } else {
                    e->output_log.push_back(entry.str());
                }
            }

            if (!resp.ok) {
                e->stop_reason = resp.error;
                // Notify the user immediately — loop died
                if (e->oq) {
                    e->oq->push("\n\033[1;31m[" + e->loop_id + "/" +
                                e->agent_id + " FAILED]\033[0m " +
                                resp.error +
                                "\n  Use /log " + e->loop_id +
                                " to see output, /kill " + e->loop_id +
                                " to dismiss.\n");
                }
                break;
            }

            // Auto-stop: agent has nothing left to do (no tool calls for N turns)
            if (consecutive_idle >= kMaxIdle) {
                e->stop_reason = "task complete (idle after " +
                                 std::to_string(consecutive_idle) + " turns)";
                if (e->oq) {
                    e->oq->push("\n\033[1;32m[" + e->loop_id + "/" +
                                e->agent_id + " DONE]\033[0m " +
                                e->stop_reason +
                                "\n  Use /log " + e->loop_id +
                                " to review, /kill " + e->loop_id + " to dismiss.\n");
                }
                break;
            }

            // Don't feed output back as prompt — that causes the agent to
            // believe its own text is being echoed by a human, producing
            // infinite confusion. History already carries the prior response.
            if (first) {
                first = false;
            }
            prompt = "Continue.";

            // Re-check stop before sleeping
            { std::lock_guard<std::mutex> lk(e->mu); if (e->stop_req) { stopped_by_request = true; break; } }

            // Brief pause between iterations to avoid hammering the API
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }

        e->state = LoopState::Stopped;
    }

    mutable std::mutex mu_;
    std::map<std::string, std::unique_ptr<LoopEntry>> loops_;
    int next_id_ = 0;
};

static void cmd_interactive() {
    std::string dir = get_config_dir();
    std::string api_key = get_api_key();

    claudius::Orchestrator orch(api_key);
    orch.set_memory_dir(get_memory_dir());
    orch.load_agents(dir + "/agents");

    std::string current_agent = "claudius";
    std::string current_model = orch.get_agent_model(current_agent);

    // Init TUI before any output (clears screen, sets scroll region, draws header).
    TUI tui;
    tui.init(current_agent, current_model, agent_color(current_agent));

    // Restore previous session if one exists.
    std::string session_path = dir + "/session.json";
    bool restored = orch.load_session(session_path);

    std::cout << "\n";
    if (restored)
        std::cout << "  \033[2mSession restored.\033[0m\n\n";
    else
        std::cout << "  /help for commands\n\n";
    std::cout.flush();

    signal(SIGWINCH, sigwinch_handler);

    // Wire sub-agent progress: print each sub-agent turn dimmed and indented.
    // Called from within the locked send_streaming path — do NOT re-acquire g_out_mu.
    orch.set_progress_callback([&tui](const std::string& agent_id,
                                      const std::string& content) {
        // Sub-agent rule: same style as turn rules but dimmer (no agent palette color).
        const char* dim_rule = "\033[38;5;238m";
        const char* rst      = "\033[0m";
        int cols = tui.cols();

        // ─── agent_id (sub) ────────────────────────────────────
        std::string label = agent_id + " (sub)";
        int fill = std::max(0, cols - 4 - (int)label.size() - 2);
        std::string rule = std::string(dim_rule) + "── " + label + " ";
        for (int i = 0; i < fill; ++i) rule += "─";
        rule += rst;
        std::cout << "\n" << rule << "\n";

        // Content: dimmed, each line indented
        std::istringstream ss(content);
        std::string line;
        while (std::getline(ss, line)) {
            std::cout << dim_rule << "  " << line << rst << "\n";
        }
        std::cout.flush();
    });

    ThinkingIndicator thinking(&tui);
    LoopManager loops;
    claudius::ReadlineWrapper rl;
    claudius::CostTracker tracker;

    // Record sub-agent token costs that bypass the top-level REPL handler.
    orch.set_cost_callback([&tracker](const std::string& agent_id,
                                       const std::string& model,
                                       const claudius::ApiResponse& resp) {
        tracker.record(agent_id, model, resp);
    });

    // Show which sub-agent is working in the status bar before each API call.
    orch.set_agent_start_callback([&tui](const std::string& agent_id) {
        tui.set_status(agent_id + ": thinking...");
    });

    rl.set_max_history(1000);
    rl.load_history(get_config_dir() + "/history");

    // Tab completion: slash commands, agent names, loop IDs
    rl.set_completion_provider(
        [&orch, &loops](const std::string& buf, const std::string& tok)
        -> std::vector<std::string> {
            auto match = [&](const std::vector<std::string>& candidates) {
                std::vector<std::string> out;
                for (auto& c : candidates)
                    if (c.substr(0, tok.size()) == tok) out.push_back(c);
                return out;
            };

            // Extract first word from buffer (the command)
            std::string cmd;
            {
                std::istringstream iss(buf);
                iss >> cmd;
                if (!cmd.empty() && cmd[0] == '/') cmd = cmd.substr(1);
            }
            bool only_cmd = (buf.find(' ') == std::string::npos);

            // Completing the slash command itself
            if (only_cmd || buf.empty()) {
                return match({"/send","/ask","/use","/list","/status","/tokens",
                              "/create","/remove","/reset","/model",
                              "/loop","/loops","/log","/watch",
                              "/kill","/suspend","/resume","/inject",
                              "/fetch","/mem","/quit","/help"});
            }

            // Agent name completion
            if (cmd == "send" || cmd == "use" || cmd == "loop" || cmd == "model") {
                auto agents = orch.list_agents();
                agents.push_back("claudius");
                return match(agents);
            }

            // Loop ID completion
            if (cmd == "kill"    || cmd == "suspend" || cmd == "resume" ||
                cmd == "watch"   || cmd == "log"     || cmd == "inject") {
                return match(loops.list_ids());
            }

            // /mem subcommands
            if (cmd == "mem") {
                return match({"write","read","show","clear"});
            }

            return {};
        });

    // ── Command queue + execution thread ────────────────────────────────────────
    CommandQueue cmd_queue;
    std::atomic<bool> quit_requested{false};
    std::atomic<bool> title_generated{false};  // generate title only once per session

    // Helper: fire title generation after the first successful response.
    auto maybe_generate_title = [&](const std::string& user_msg,
                                     const std::string& response_snippet) {
        if (title_generated.exchange(true)) return;  // already done
        generate_title_async(orch.client(), user_msg, response_snippet, tui);
    };

    // Output queue: exec and loop threads push here; main thread flushes.
    OutputQueue output_queue;

    // All command handling lives in this lambda, called from the exec thread.
    // ALL output goes through output_queue.push() — never directly to stdout.
    auto handle = [&](const std::string& line) {

        if (line[0] == '/') {
            // Parse command
            std::istringstream iss(line.substr(1));
            std::string cmd;
            iss >> cmd;

            if (cmd == "quit" || cmd == "exit" || cmd == "q") {
                quit_requested = true; return;
            }

            if (cmd == "list") {
                for (auto& id : orch.list_agents())
                    output_queue.push("  " + id + "\n");
                return;
            }
            if (cmd == "status") {
                output_queue.push(orch.global_status());
                return;
            }
            if (cmd == "tokens") {
                output_queue.push(tracker.format_summary());
                return;
            }
            if (cmd == "use" || cmd == "switch") {
                std::string id;
                iss >> id;
                if (id == "claudius" || orch.has_agent(id)) {
                    current_agent = id;
                    current_model = orch.get_agent_model(id);
                    tui.update(current_agent, current_model, tracker.format_session_stats(), agent_color(current_agent));
                } else {
                    output_queue.push("ERR: no agent '" + id + "'\n");
                }
                return;
            }
            if (cmd == "send") {
                std::string id;
                iss >> id;
                std::string msg;
                std::getline(iss, msg);
                if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
                try {
                    claudius::MarkdownRenderer md;
                    auto resp = orch.send_streaming(id, msg,
                        [&md, &output_queue](const std::string& chunk) {
                            auto s = md.feed(chunk);
                            if (!s.empty()) output_queue.push(s);
                        });
                    auto tail = md.flush();
                    if (!tail.empty()) output_queue.push(tail);
                    output_queue.push("\n");
                    if (resp.ok) {
                        tracker.record(id, orch.get_agent_model(id), resp);
                        tui.update(current_agent, current_model, tracker.format_session_stats(), agent_color(current_agent));
                        maybe_generate_title(msg, resp.content);
                    } else {
                        output_queue.push("\033[38;5;167mERR: " + resp.error + "\033[0m\n");
                    }
                } catch (const std::exception& e) {
                    output_queue.push("\033[38;5;167mERR: " + std::string(e.what()) + "\033[0m\n");
                }
                return;
            }
            if (cmd == "ask") {
                std::string query;
                std::getline(iss, query);
                if (!query.empty() && query[0] == ' ') query.erase(0, 1);
                try {
                    claudius::MarkdownRenderer md;
                    auto resp = orch.send_streaming("claudius", query,
                        [&md, &output_queue](const std::string& chunk) {
                            auto s = md.feed(chunk);
                            if (!s.empty()) output_queue.push(s);
                        });
                    auto tail = md.flush();
                    if (!tail.empty()) output_queue.push(tail);
                    output_queue.push("\n");
                    if (resp.ok) {
                        tracker.record("claudius", orch.get_agent_model("claudius"), resp);
                        tui.update(current_agent, current_model, tracker.format_session_stats(), agent_color(current_agent));
                        maybe_generate_title(query, resp.content);
                    } else {
                        output_queue.push("\033[38;5;167mERR: " + resp.error + "\033[0m\n");
                    }
                } catch (const std::exception& e) {
                    output_queue.push("\033[38;5;167mERR: " + std::string(e.what()) + "\033[0m\n");
                }
                return;
            }
            if (cmd == "create") {
                std::string id;
                iss >> id;
                try {
                    auto config = claudius::master_constitution();
                    config.name = id;
                    orch.create_agent(id, std::move(config));
                    output_queue.push("Created: " + id + " (default config)\n");
                    output_queue.push("Edit ~/.claudius/agents/" + id + ".json to customize\n");
                } catch (const std::exception& e) {
                    output_queue.push("ERR: " + std::string(e.what()) + "\n");
                }
                return;
            }
            if (cmd == "remove") {
                std::string id;
                iss >> id;
                orch.remove_agent(id);
                output_queue.push("Removed: " + id + "\n");
                if (current_agent == id) current_agent = "claudius";
                return;
            }
            if (cmd == "reset") {
                std::string id;
                iss >> id;
                if (id.empty()) id = current_agent;
                try {
                    orch.get_agent(id).reset_history();
                    output_queue.push("History cleared: " + id + "\n");
                } catch (const std::exception& e) {
                    output_queue.push("ERR: " + std::string(e.what()) + "\n");
                }
                return;
            }
            if (cmd == "loop") {
                std::string id;
                iss >> id;
                std::string prompt;
                std::getline(iss, prompt);
                if (!prompt.empty() && prompt[0] == ' ') prompt.erase(0, 1);
                if (id.empty() || prompt.empty()) {
                    output_queue.push("Usage: /loop <agent> <initial prompt>\n");
                    return;
                }
                if (id != "claudius" && !orch.has_agent(id)) {
                    output_queue.push("ERR: no agent '" + id + "'\n");
                    return;
                }
                std::string lid = loops.start(orch, id, prompt, &tracker, &output_queue);
                output_queue.push("Loop started: " + lid + " (agent: " + id + ")\n");
                return;
            }
            if (cmd == "loops") {
                output_queue.push(loops.list());
                return;
            }
            if (cmd == "kill") {
                std::string lid;
                iss >> lid;
                if (loops.kill(lid))
                    output_queue.push("Killed: " + lid + "\n");
                else
                    output_queue.push("ERR: no loop '" + lid + "'\n");
                return;
            }
            if (cmd == "suspend") {
                std::string lid;
                iss >> lid;
                if (loops.suspend(lid))
                    output_queue.push("Suspended: " + lid + "\n");
                else
                    output_queue.push("ERR: no loop '" + lid + "' or not running\n");
                return;
            }
            if (cmd == "resume") {
                std::string lid;
                iss >> lid;
                if (loops.resume(lid))
                    output_queue.push("Resumed: " + lid + "\n");
                else
                    output_queue.push("ERR: no loop '" + lid + "' or not suspended\n");
                return;
            }
            if (cmd == "inject") {
                std::string lid;
                iss >> lid;
                std::string msg;
                std::getline(iss, msg);
                if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
                if (loops.inject(lid, msg))
                    output_queue.push("Injected into " + lid + "\n");
                else
                    output_queue.push("ERR: no loop '" + lid + "'\n");
                return;
            }
            if (cmd == "log") {
                std::string lid;
                iss >> lid;
                if (lid.empty()) {
                    output_queue.push("Usage: /log <loop-id> [last-N]\n");
                    return;
                }
                int n = 0;
                iss >> n;
                output_queue.push(loops.log(lid, n));
                return;
            }
            if (cmd == "watch") {
                std::string lid;
                iss >> lid;
                if (lid.empty()) {
                    output_queue.push("Usage: /watch <loop-id>\n");
                    return;
                }
                if (loops.is_stopped(lid) && loops.log_count(lid) == 0) {
                    output_queue.push("ERR: no loop '" + lid + "'\n");
                    return;
                }
                // Dump everything buffered so far
                size_t seen = loops.log_count(lid);
                output_queue.push(loops.log(lid, 0));
                if (!loops.is_stopped(lid)) {
                    output_queue.push("\033[2m--- watching " + lid +
                                      " — press Enter to detach ---\033[0m\n");
                    // Tail new entries — exec thread polls while main thread flushes
                    while (!loops.is_stopped(lid)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        size_t now = loops.log_count(lid);
                        if (now > seen) {
                            output_queue.push(loops.log_since(lid, seen));
                            seen = now;
                        }
                    }
                    // Flush any remaining entries after stop
                    size_t now = loops.log_count(lid);
                    if (now > seen) {
                        output_queue.push(loops.log_since(lid, seen));
                    }
                    if (loops.is_stopped(lid)) {
                        output_queue.push("\033[2m--- loop finished ---\033[0m\n");
                    } else {
                        output_queue.push("\033[2m--- detached ---\033[0m\n");
                    }
                }
                return;
            }
            if (cmd == "fetch") {
                std::string url;
                iss >> url;
                if (url.empty()) {
                    output_queue.push("Usage: /fetch <url>\n");
                    return;
                }
                thinking.start("fetching");
                std::string content = fetch_url(url);
                thinking.stop();
                if (content.substr(0, 4) == "ERR:") {
                    output_queue.push("\033[38;5;167m" + content + "\033[0m\n");
                    return;
                }
                static constexpr size_t kFetchLimit = 32768;
                if (content.size() > kFetchLimit) {
                    content.resize(kFetchLimit);
                    content += "\n... [content truncated to 32 KB]";
                }
                std::string msg = "[FETCHED: " + url + "]\n" + content +
                                  "\n[END FETCHED]\n";
                try {
                    thinking.start("generating");
                    auto resp = orch.send(current_agent, msg);
                    thinking.stop();
                    if (resp.ok) {
                        output_queue.push(claudius::render_markdown(resp.content) + "\n");
                        tracker.record(current_agent, orch.get_agent_model(current_agent), resp);
                        tui.update(current_agent, current_model, tracker.format_session_stats(), agent_color(current_agent));
                    } else {
                        output_queue.push("\033[38;5;167mERR: " + resp.error + "\033[0m\n");
                    }
                } catch (const std::exception& ex) {
                    thinking.stop();
                    output_queue.push("\033[38;5;167mERR: " + std::string(ex.what()) + "\033[0m\n");
                }
                return;
            }
            if (cmd == "mem") {
                std::string subcmd;
                iss >> subcmd;
                if (subcmd == "write") {
                    std::string text;
                    std::getline(iss, text);
                    if (!text.empty() && text[0] == ' ') text.erase(0, 1);
                    if (text.empty()) {
                        output_queue.push("Usage: /mem write <text>\n");
                        return;
                    }
                    write_memory(current_agent, text);
                    output_queue.push("Memory written for " + current_agent + "\n");
                } else if (subcmd == "read") {
                    std::string mem = read_memory(current_agent);
                    if (mem.empty()) {
                        output_queue.push("No memory for " + current_agent + "\n");
                        return;
                    }
                    // Inject memory into agent context
                    std::string msg = "[MEMORY for " + current_agent + "]:\n" +
                                      mem + "\n[END MEMORY]\n";
                    try {
                        thinking.start();
                        auto resp = orch.send(current_agent, msg);
                        thinking.stop();
                        if (resp.ok) {
                            output_queue.push(claudius::render_markdown(resp.content) + "\n");
                            tracker.record(current_agent, orch.get_agent_model(current_agent), resp);
                            tui.update(current_agent, current_model, tracker.format_session_stats(), agent_color(current_agent));
                        } else {
                            output_queue.push("ERR: " + resp.error + "\n");
                        }
                    } catch (const std::exception& ex) {
                        thinking.stop();
                        output_queue.push("ERR: " + std::string(ex.what()) + "\n");
                    }
                } else if (subcmd == "show") {
                    std::string mem = read_memory(current_agent);
                    if (mem.empty())
                        output_queue.push("No memory for " + current_agent + "\n");
                    else
                        output_queue.push(mem + "\n");
                } else if (subcmd == "clear") {
                    std::string path = get_memory_dir() + "/" + current_agent + ".md";
                    fs::remove(path);
                    output_queue.push("Memory cleared for " + current_agent + "\n");
                } else {
                    output_queue.push("Usage: /mem write <text> | /mem read | /mem show | /mem clear\n");
                }
                return;
            }
            if (cmd == "model") {
                std::string id, model;
                iss >> id >> model;
                if (id.empty() || model.empty()) {
                    output_queue.push("Usage: /model <agent-id> <model-id>\n");
                    output_queue.push("  e.g. /model researcher claude-haiku-4-5-20251001\n");
                    return;
                }
                try {
                    orch.get_agent(id).config_mut().model = model;
                    output_queue.push(id + " model -> " + model + "\n");
                } catch (const std::exception& ex) {
                    output_queue.push("ERR: " + std::string(ex.what()) + "\n");
                }
                return;
            }
            if (cmd == "help") {
                output_queue.push(
                    "Commands:\n"
                    "  /send <agent> <msg>              — send to specific agent\n"
                    "  /ask <query>                     — ask claudius master\n"
                    "  /use <agent>                     — switch current agent\n"
                    "  /list                            — list agents\n"
                    "  /status                          — system status\n"
                    "  /tokens                          — token usage\n"
                    "  /create <id>                     — create agent (default config)\n"
                    "  /remove <id>                     — remove agent\n"
                    "  /reset [id]                      — clear agent history\n"
                    "  /model <agent> <model-id>        — change agent model at runtime\n"
                    "\n"
                    "  /loop <agent> <prompt>           — run agent in background loop\n"
                    "  /loops                           — list running/suspended loops\n"
                    "  /log <loop-id> [last-N]          — show buffered loop output\n"
                    "  /watch <loop-id>                 — tail loop output live (Enter to detach)\n"
                    "  /kill <loop-id>                  — stop a loop\n"
                    "  /suspend <loop-id>               — pause a loop\n"
                    "  /resume <loop-id>                — resume a paused loop\n"
                    "  /inject <loop-id> <msg>          — send message into a running loop\n"
                    "\n"
                    "  /fetch <url>                     — fetch URL and send HTML to current agent\n"
                    "  /mem write <text>                — append note to agent memory\n"
                    "  /mem read                        — load memory into agent context\n"
                    "  /mem show                        — print raw memory file\n"
                    "  /mem clear                       — delete agent memory file\n"
                    "\n"
                    "  /quit                            — exit\n"
                    "\n"
                    "Plain text sends to current agent.\n");
                return;
            }

            output_queue.push("Unknown command. /help for list.\n");
            return;
        }

        // Plain text → stream to current agent
        try {
            claudius::MarkdownRenderer md;
            auto resp = orch.send_streaming(current_agent, line,
                [&md, &output_queue](const std::string& chunk) {
                    auto s = md.feed(chunk);
                    if (!s.empty()) output_queue.push(s);
                });
            auto tail = md.flush();
            if (!tail.empty()) output_queue.push(tail);
            output_queue.push("\n");
            if (resp.ok) {
                tracker.record(current_agent, orch.get_agent_model(current_agent), resp);
                tui.update(current_agent, current_model, tracker.format_session_stats(), agent_color(current_agent));
                maybe_generate_title(line, resp.content);
            } else {
                output_queue.push("\033[38;5;167mERR: " + resp.error + "\033[0m\n");
            }
        } catch (const std::exception& e) {
            output_queue.push("\033[38;5;167mERR: " + std::string(e.what()) + "\033[0m\n");
        }
    };  // end handle lambda

    // Execution thread: drains the command queue serially.
    std::thread exec_thread([&]() {
        std::string line;
        while (cmd_queue.pop(line)) {
            cmd_queue.set_busy(true);
            handle(line);
            cmd_queue.set_busy(false);
            // Clear the queue indicator now that this item is done.
            // If more items are pending, begin_input will re-show it on the next loop.
            tui.clear_queue_indicator();
        }
    });

    // ── Main readline loop ──────────────────────────────────────────────────
    bool exit_warned = false;
    while (!quit_requested) {
        if (g_winch) { g_winch = 0; tui.resize(); }

        tui.begin_input(cmd_queue.pending());

        // Install readline callback at the current cursor position (N-1).
        std::atomic<bool> line_ready{false};
        std::string completed_line;

        rl.install_callback(tui.build_prompt(), [&](const std::string& line) {
            completed_line = line;
            line_ready = true;
        });

        // Select loop: flush exec output + process readline input
        while (!line_ready && !quit_requested) {
            if (g_winch) { g_winch = 0; tui.resize(); }

            // Flush pending exec output (main thread owns stdout)
            std::string pending = output_queue.drain();
            if (!pending.empty()) {
                printf("\0337");                                         // save cursor (at N-1)
                printf("\033[%d;1H", tui.last_scroll_row());            // move to N-3
                fwrite(pending.data(), 1, pending.size(), stdout);
                printf("\0338");                                         // restore cursor to N-1
                fflush(stdout);
            }

            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            struct timeval tv = {0, 20000};  // 20ms
            select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);

            if (FD_ISSET(STDIN_FILENO, &rfds)) {
                rl.process_char();
            }
        }

        rl.remove_callback();
        if (quit_requested) break;

        std::string line = completed_line;
        if (line.empty()) continue;

        // Exit check
        {
            std::string lower = line;
            for (auto& c : lower) c = static_cast<char>(std::tolower((unsigned char)c));
            while (!lower.empty() && lower.back()  == ' ') lower.pop_back();
            while (!lower.empty() && lower.front() == ' ') lower.erase(lower.begin());
            if (lower == "exit" || lower == "quit" || lower == "q" ||
                lower == "bye"  || lower == ":q"   ||
                lower == "/quit"|| lower == "/exit" || lower == "/q") {
                int waiting = cmd_queue.pending() + (cmd_queue.is_busy() ? 1 : 0);
                if (waiting > 0 && !exit_warned) {
                    output_queue.push("WARN: " + std::to_string(waiting) +
                                      " command(s) in flight — type 'exit' again to force quit\n");
                    exit_warned = true;
                    continue;
                }
                break;
            }
            exit_warned = false;
        }

        // Echo the user's prompt into the scroll region so it persists in the
        // session view rather than disappearing when enter is pressed.
        // Styled with a muted arrow prefix to distinguish it from model output.
        {
            // Drain any output that arrived while readline was active so the
            // echo lands after previous command output, not mid-stream.
            std::string pending = output_queue.drain();
            if (!pending.empty()) {
                printf("\0337");
                printf("\033[%d;1H", tui.last_scroll_row());
                fwrite(pending.data(), 1, pending.size(), stdout);
                printf("\0338");
                fflush(stdout);
            }

            std::string echo = "\033[38;5;244m> \033[38;5;250m" + line + "\033[0m\n\n";
            printf("\0337");
            printf("\033[%d;1H", tui.last_scroll_row());
            fwrite(echo.data(), 1, echo.size(), stdout);
            printf("\0338");
            fflush(stdout);
        }

        bool was_busy = cmd_queue.is_busy();
        cmd_queue.push(line);
        if (was_busy) {
            tui.set_status("queued (" + std::to_string(cmd_queue.pending()) + " waiting)");
        }
    }

    cmd_queue.stop();
    exec_thread.join();

    // Save session before restoring terminal.
    rl.save_history(get_config_dir() + "/history");
    orch.save_session(session_path);

    // Restore terminal and print session summary.
    tui.shutdown();
    std::cout << "\n";
    if (tracker.session_cost() > 0.0) {
        std::cout << tracker.format_summary();
    }
}

static void cmd_oneshot(const std::string& agent_id, const std::string& msg) {
    std::string dir = get_config_dir();
    std::string api_key = get_api_key();

    claudius::Orchestrator orch(api_key);
    orch.load_agents(dir + "/agents");

    auto resp = orch.send(agent_id, msg);
    if (resp.ok) {
        std::cout << resp.content << "\n";
    } else {
        std::cerr << "ERR: " << resp.error << "\n";
        std::exit(1);
    }
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            cmd_interactive();
            return 0;
        }

        std::string arg1 = argv[1];

        if (arg1 == "--init" || arg1 == "init") {
            cmd_init();
            return 0;
        }
        if (arg1 == "--gen-token" || arg1 == "gen-token") {
            cmd_gen_token();
            return 0;
        }
        if (arg1 == "--serve" || arg1 == "serve") {
            int port = 9077;
            if (argc >= 4 && std::string(argv[2]) == "--port") {
                port = std::atoi(argv[3]);
            }
            cmd_serve(port);
            return 0;
        }
        if (arg1 == "--send" || arg1 == "send") {
            if (argc < 4) {
                std::cerr << "Usage: claudius --send <agent_id> <message>\n";
                return 1;
            }
            std::string agent = argv[2];
            std::string msg;
            for (int i = 3; i < argc; ++i) {
                if (i > 3) msg += " ";
                msg += argv[i];
            }
            cmd_oneshot(agent, msg);
            return 0;
        }
        if (arg1 == "--help" || arg1 == "-h" || arg1 == "help") {
            std::cout << BANNER;
            std::cout <<
                "Usage:\n"
                "  claudius                          Interactive REPL\n"
                "  claudius --serve [--port N]        Start TCP server (default 9077)\n"
                "  claudius --send <agent> <msg>      One-shot message\n"
                "  claudius --init                    Initialize config + tokens\n"
                "  claudius --gen-token               Generate new auth token\n"
                "  claudius --help                    This help\n\n"
                "Environment:\n"
                "  ANTHROPIC_API_KEY                  Claude API key\n\n"
                "Config: ~/.claudius/\n"
                "  api_key                            API key file\n"
                "  auth_tokens                        Hashed access tokens\n"
                "  agents/*.json                      Agent constitutions\n";
            return 0;
        }

        std::cerr << "Unknown option: " << arg1 << ". Try --help\n";
        return 1;

    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << "\n";
        return 1;
    }
}
