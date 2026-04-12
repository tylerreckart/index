// claudius/src/orchestrator.cpp
#include "orchestrator.h"
#include "commands.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace claudius {

Orchestrator::Orchestrator(const std::string& api_key)
    : client_(api_key)
{
    // Default memory directory: ~/.claudius/memory
    const char* home = std::getenv("HOME");
    memory_dir_ = (home ? std::string(home) : std::string(".")) + "/.claudius/memory";

    // Create master Claudius agent
    auto master = master_constitution();
    claudius_master_ = std::make_unique<Agent>("claudius", master, client_);
}

Agent& Orchestrator::create_agent(const std::string& id, Constitution config) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    if (agents_.count(id)) {
        throw std::runtime_error("Agent already exists: " + id);
    }
    auto agent = std::make_unique<Agent>(id, std::move(config), client_);
    auto& ref = *agent;
    agents_[id] = std::move(agent);
    return ref;
}

Agent& Orchestrator::get_agent(const std::string& id) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) throw std::runtime_error("No agent: " + id);
    return *it->second;
}

bool Orchestrator::has_agent(const std::string& id) const {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    return agents_.count(id) > 0;
}

void Orchestrator::remove_agent(const std::string& id) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    agents_.erase(id);
}

std::vector<std::string> Orchestrator::list_agents() const {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    std::vector<std::string> ids;
    ids.reserve(agents_.size());
    for (auto& [id, _] : agents_) ids.push_back(id);
    return ids;
}

void Orchestrator::load_agents(const std::string& dir) {
    if (!fs::exists(dir)) return;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".json") {
            try {
                auto config = Constitution::from_file(entry.path().string());
                std::string id = config.name.empty()
                    ? entry.path().stem().string()
                    : config.name;
                create_agent(id, std::move(config));
            } catch (const std::exception& e) {
                // Skip malformed agent files, log to stderr
                fprintf(stderr, "WARN: skip %s: %s\n",
                    entry.path().c_str(), e.what());
            }
        }
    }
}

// Build an AgentInvoker that runs a sub-agent through the full dispatch loop.
// Sub-agents receive their own tool access (/fetch, /exec, /write, /agent).
// depth prevents runaway delegation chains (max 2 levels: claudius → agent → sub-agent).
AgentInvoker Orchestrator::make_invoker(const std::string& caller_id, int depth) {
    if (depth >= 2) {
        return [](const std::string&, const std::string&) -> std::string {
            return "ERR: delegation depth limit reached (max 2 levels)";
        };
    }
    return [this, caller_id, depth](const std::string& sub_id, const std::string& sub_msg) -> std::string {
        if (sub_id == caller_id) return "ERR: agent cannot invoke itself";
        if (sub_id == "claudius") return "ERR: claudius cannot be delegated to";
        {
            std::lock_guard<std::mutex> lk(agents_mutex_);
            if (!agents_.count(sub_id))
                return "ERR: no agent '" + sub_id + "'";
        }
        // Run the full agentic dispatch loop for the sub-agent so it has
        // access to its own tools (/fetch, /exec, /write, /agent, /mem).
        auto resp = send_internal(sub_id, sub_msg, depth + 1);
        return resp.ok ? resp.content : "ERR: " + resp.error;
    };
}

ApiResponse Orchestrator::ask_claudius(const std::string& query) {
    return send("claudius", query);
}

void Orchestrator::set_progress_callback(ProgressCallback cb) {
    progress_cb_ = std::move(cb);
}

void Orchestrator::set_cost_callback(CostCallback cb) {
    cost_cb_ = std::move(cb);
}

void Orchestrator::set_agent_start_callback(AgentStartCallback cb) {
    start_cb_ = std::move(cb);
}

// Core agentic dispatch loop — used by both send() and sub-agent invocations.
ApiResponse Orchestrator::send_internal(const std::string& agent_id,
                                        const std::string& message,
                                        int depth) {
    Agent* agent_ptr;
    std::string current_msg;

    if (agent_id == "claudius") {
        agent_ptr   = claudius_master_.get();
        current_msg = global_status() + "\n\nQUERY: " + message;
    } else {
        agent_ptr   = &get_agent(agent_id);
        current_msg = message;
    }

    auto invoker = make_invoker(agent_id, depth);

    ApiResponse resp;
    static constexpr int kMaxTurns = 6;
    for (int i = 0; i < kMaxTurns; ++i) {
        // Notify UI that a sub-agent is about to make an API call.
        if (depth > 0 && start_cb_) start_cb_(agent_id);

        resp = agent_ptr->send(current_msg);
        if (!resp.ok) return resp;

        if (depth > 0 && resp.ok) {
            // Notify the UI (progress) and record cost for sub-agent turns.
            // Top-level cost is recorded by the REPL after send() returns.
            if (progress_cb_) progress_cb_(agent_id, resp.content);
            if (cost_cb_)     cost_cb_(agent_id, agent_ptr->config().model, resp);
        }

        auto cmds = parse_agent_commands(resp.content);
        if (cmds.empty()) break;

        resp.had_tool_calls = true;
        current_msg = execute_agent_commands(cmds, agent_id, memory_dir_, invoker);
    }

    return resp;
}

ApiResponse Orchestrator::send(const std::string& agent_id, const std::string& message) {
    return send_internal(agent_id, message, 0);
}

ApiResponse Orchestrator::send_streaming(const std::string& agent_id,
                                         const std::string& message,
                                         StreamCallback cb) {
    Agent* agent_ptr;
    std::string current_msg;

    if (agent_id == "claudius") {
        agent_ptr   = claudius_master_.get();
        current_msg = global_status() + "\n\nQUERY: " + message;
    } else {
        agent_ptr   = &get_agent(agent_id);
        current_msg = message;
    }

    // First turn: stream to caller
    ApiResponse resp = agent_ptr->stream(current_msg, cb);
    if (!resp.ok) return resp;

    auto cmds = parse_agent_commands(resp.content);
    if (cmds.empty()) return resp;

    auto invoker = make_invoker(agent_id, 0);

    // Tool-call re-entry turns: stream each so the user can follow progress
    resp.had_tool_calls = true;
    static constexpr int kMaxReentryTurns = 5;
    for (int i = 0; i < kMaxReentryTurns; ++i) {
        cb("\n");
        current_msg = execute_agent_commands(cmds, agent_id, memory_dir_, invoker);
        resp = agent_ptr->stream(current_msg, cb);
        if (!resp.ok) return resp;
        cmds = parse_agent_commands(resp.content);
        if (cmds.empty()) break;
        resp.had_tool_calls = true;
    }

    return resp;
}


std::string Orchestrator::get_agent_model(const std::string& id) const {
    if (id == "claudius") return claudius_master_->config().model;
    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) return "";
    return it->second->config().model;
}

std::string Orchestrator::global_status() const {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    std::ostringstream ss;

    // Agent roster — this is the primary signal Claudius uses for routing.
    // Format emphasizes capability over stats; stats follow in parentheses.
    if (agents_.empty()) {
        ss << "AVAILABLE AGENTS: none loaded\n";
    } else {
        ss << "AVAILABLE AGENTS — delegate with /agent <id> <task>:\n";
        for (auto& [id, agent] : agents_) {
            const auto& cfg = agent->config();
            ss << "  " << id;
            if (!cfg.role.empty())
                ss << "  [" << cfg.role << "]";
            if (!cfg.goal.empty())
                ss << "  — " << cfg.goal;
            ss << "\n";
            // Stats on second line, indented, so they don't crowd the capability description
            const auto& st = agent->stats();
            ss << "    reqs:" << st.total_requests
               << " in:" << st.total_input_tokens
               << " out:" << st.total_output_tokens;
            if (!cfg.advisor_model.empty())
                ss << " advisor:" << cfg.advisor_model;
            ss << "\n";
        }
    }

    ss << "SESSION: in:" << client_.total_input_tokens()
       << " out:" << client_.total_output_tokens() << "\n";

    return ss.str();
}

// ─── Session persistence ──────────────────────────────────────────────────────

static std::shared_ptr<JsonValue> messages_to_json(const std::vector<Message>& msgs) {
    auto arr = jarr();
    for (auto& m : msgs) {
        auto obj = jobj();
        obj->as_object_mut()["role"]    = jstr(m.role);
        obj->as_object_mut()["content"] = jstr(m.content);
        arr->as_array_mut().push_back(obj);
    }
    return arr;
}

static std::vector<Message> messages_from_json(const JsonValue* arr) {
    std::vector<Message> out;
    if (!arr || !arr->is_array()) return out;
    for (auto& v : arr->as_array()) {
        if (!v) continue;
        out.push_back({v->get_string("role"), v->get_string("content")});
    }
    return out;
}

void Orchestrator::save_session(const std::string& path) const {
    auto root = jobj();
    auto& m = root->as_object_mut();
    m["version"] = jnum(1);

    // Claudius master history
    m["claudius"] = messages_to_json(claudius_master_->history());

    // All loaded agents
    auto agents_obj = jobj();
    {
        std::lock_guard<std::mutex> lk(agents_mutex_);
        for (auto& [id, agent] : agents_) {
            if (!agent->history().empty())
                agents_obj->as_object_mut()[id] = messages_to_json(agent->history());
        }
    }
    m["agents"] = agents_obj;

    std::ofstream f(path);
    if (f.is_open()) f << json_serialize(*root);
}

bool Orchestrator::load_session(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string raw = ss.str();
    if (raw.empty()) return false;

    try {
        auto root = json_parse(raw);

        // Restore claudius master
        auto cval = root->get("claudius");
        auto cmsgs = messages_from_json(cval.get());
        if (!cmsgs.empty()) claudius_master_->set_history(std::move(cmsgs));

        // Restore loaded agents
        auto aval = root->get("agents");
        if (aval && aval->is_object()) {
            std::lock_guard<std::mutex> lk(agents_mutex_);
            for (auto& [id, vptr] : aval->as_object()) {
                auto it = agents_.find(id);
                if (it == agents_.end()) continue;
                auto msgs = messages_from_json(vptr.get());
                if (!msgs.empty()) it->second->set_history(std::move(msgs));
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace claudius
