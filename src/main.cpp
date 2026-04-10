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
#include "server.h"
#include "auth.h"
#include "constitution.h"

#include <iostream>
#include <string>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static const char* BANNER = R"(
   _____ _                 _ _
  / ____| |               | (_)
 | |    | | __ _ _   _  __| |_ _   _ ___
 | |    | |/ _` | | | |/ _` | | | | / __|
 | |____| | (_| | |_| | (_| | | |_| \__ \
  \_____|_|\__,_|\__,_|\__,_|_|\__,_|___/
                                    v0.1.0
  Agent orchestrator. Few tokens. Full accuracy.
)";

static std::string get_config_dir() {
    const char* home = std::getenv("HOME");
    if (!home) home = ".";
    std::string dir = std::string(home) + "/.claudius";
    fs::create_directories(dir);
    return dir;
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
        c.name = "researcher";
        c.role = "research-analyst";
        c.brevity = claudius::Brevity::Lite;
        c.max_tokens = 2048;
        c.temperature = 0.5;
        c.goal = "Research topics with depth. Synthesize findings. Distinguish fact from inference.";
        c.personality = "Meticulous, skeptical of hearsay, prefers primary sources. "
                        "Reports with the formality of a written brief.";
        c.rules = {
            "Note confidence: high, medium, or low.",
            "Separate what is known from what is inferred.",
            "When uncertain, state it plainly.",
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

    std::cout << "Example agents created in " << agents_dir << "/\n";
    std::cout << "  reviewer.json   — code review (ultra)\n";
    std::cout << "  researcher.json — research analyst (lite)\n";
    std::cout << "  devops.json     — infrastructure (full)\n\n";
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

static void cmd_interactive() {
    std::string dir = get_config_dir();
    std::string api_key = get_api_key();

    claudius::Orchestrator orch(api_key);
    orch.load_agents(dir + "/agents");

    std::cout << BANNER;
    std::cout << "Agents: ";
    for (auto& id : orch.list_agents()) std::cout << id << " ";
    std::cout << "\n";
    std::cout << "Commands: /send <agent> <msg> | /ask <query> | /list | /status\n";
    std::cout << "          /create <id> | /remove <id> | /reset <id> | /tokens | /quit\n";
    std::cout << "Default: messages go to claudius master.\n\n";

    std::string current_agent = "claudius";

    while (true) {
        std::cout << "[" << current_agent << "] > ";
        std::cout.flush();

        std::string line;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        if (line[0] == '/') {
            // Parse command
            std::istringstream iss(line.substr(1));
            std::string cmd;
            iss >> cmd;

            if (cmd == "quit" || cmd == "exit" || cmd == "q") break;

            if (cmd == "list") {
                for (auto& id : orch.list_agents())
                    std::cout << "  " << id << "\n";
                continue;
            }
            if (cmd == "status") {
                std::cout << orch.global_status();
                continue;
            }
            if (cmd == "tokens") {
                std::cout << "in:" << orch.total_input_tokens()
                          << " out:" << orch.total_output_tokens() << "\n";
                continue;
            }
            if (cmd == "use" || cmd == "switch") {
                std::string id;
                iss >> id;
                if (id == "claudius" || orch.has_agent(id)) {
                    current_agent = id;
                    std::cout << "Switched to: " << id << "\n";
                } else {
                    std::cout << "ERR: no agent '" << id << "'\n";
                }
                continue;
            }
            if (cmd == "send") {
                std::string id;
                iss >> id;
                std::string msg;
                std::getline(iss, msg);
                if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
                try {
                    auto resp = orch.send(id, msg);
                    if (resp.ok) {
                        std::cout << resp.content << "\n";
                        std::cout << "  [in:" << resp.input_tokens
                                  << " out:" << resp.output_tokens << "]\n";
                    } else {
                        std::cout << "ERR: " << resp.error << "\n";
                    }
                } catch (const std::exception& e) {
                    std::cout << "ERR: " << e.what() << "\n";
                }
                continue;
            }
            if (cmd == "ask") {
                std::string query;
                std::getline(iss, query);
                if (!query.empty() && query[0] == ' ') query.erase(0, 1);
                try {
                    auto resp = orch.ask_claudius(query);
                    if (resp.ok) {
                        std::cout << resp.content << "\n";
                        std::cout << "  [in:" << resp.input_tokens
                                  << " out:" << resp.output_tokens << "]\n";
                    } else {
                        std::cout << "ERR: " << resp.error << "\n";
                    }
                } catch (const std::exception& e) {
                    std::cout << "ERR: " << e.what() << "\n";
                }
                continue;
            }
            if (cmd == "create") {
                std::string id;
                iss >> id;
                try {
                    auto config = claudius::master_constitution();
                    config.name = id;
                    orch.create_agent(id, std::move(config));
                    std::cout << "Created: " << id << " (default config)\n";
                    std::cout << "Edit ~/.claudius/agents/" << id << ".json to customize\n";
                } catch (const std::exception& e) {
                    std::cout << "ERR: " << e.what() << "\n";
                }
                continue;
            }
            if (cmd == "remove") {
                std::string id;
                iss >> id;
                orch.remove_agent(id);
                std::cout << "Removed: " << id << "\n";
                if (current_agent == id) current_agent = "claudius";
                continue;
            }
            if (cmd == "reset") {
                std::string id;
                iss >> id;
                if (id.empty()) id = current_agent;
                try {
                    orch.get_agent(id).reset_history();
                    std::cout << "History cleared: " << id << "\n";
                } catch (const std::exception& e) {
                    std::cout << "ERR: " << e.what() << "\n";
                }
                continue;
            }
            if (cmd == "help") {
                std::cout << "Commands:\n"
                    "  /send <agent> <msg>  — send to specific agent\n"
                    "  /ask <query>         — ask claudius master\n"
                    "  /use <agent>         — switch current agent\n"
                    "  /list                — list agents\n"
                    "  /status              — system status\n"
                    "  /tokens              — token usage\n"
                    "  /create <id>         — create agent (default config)\n"
                    "  /remove <id>         — remove agent\n"
                    "  /reset [id]          — clear agent history\n"
                    "  /quit                — exit\n"
                    "\n"
                    "Plain text sends to current agent.\n";
                continue;
            }

            std::cout << "Unknown command. /help for list.\n";
            continue;
        }

        // Plain text → send to current agent
        try {
            auto resp = orch.send(current_agent, line);
            if (resp.ok) {
                std::cout << resp.content << "\n";
                std::cout << "  [in:" << resp.input_tokens
                          << " out:" << resp.output_tokens << "]\n";
            } else {
                std::cout << "ERR: " << resp.error << "\n";
            }
        } catch (const std::exception& e) {
            std::cout << "ERR: " << e.what() << "\n";
        }
    }

    std::cout << "\n" << orch.global_status();
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
