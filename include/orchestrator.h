#pragma once
// claudius/include/orchestrator.h — Multi-agent orchestrator

#include "agent.h"
#include "api_client.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>

namespace claudius {

class Orchestrator {
public:
    explicit Orchestrator(const std::string& api_key);

    // Agent management
    Agent& create_agent(const std::string& id, Constitution config);
    Agent& get_agent(const std::string& id);
    bool   has_agent(const std::string& id) const;
    void   remove_agent(const std::string& id);
    std::vector<std::string> list_agents() const;

    // Load agent definitions from directory
    void load_agents(const std::string& dir);

    // Send message to specific agent
    ApiResponse send(const std::string& agent_id, const std::string& message);

    // Ask Claudius (master) about system state
    ApiResponse ask_claudius(const std::string& query);

    // Global stats
    std::string global_status() const;

    // Token tracking
    int total_input_tokens()  const { return client_.total_input_tokens(); }
    int total_output_tokens() const { return client_.total_output_tokens(); }

    ApiClient& client() { return client_; }

private:
    ApiClient client_;
    std::unordered_map<std::string, std::unique_ptr<Agent>> agents_;
    mutable std::mutex agents_mutex_;

    // Master Claudius agent for meta-queries
    std::unique_ptr<Agent> claudius_master_;
};

} // namespace claudius
