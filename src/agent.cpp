// claudius/src/agent.cpp
#include "agent.h"
#include "json.h"
#include <sstream>

namespace claudius {

Agent::Agent(const std::string& id, Constitution config, ApiClient& client)
    : id_(id), config_(std::move(config)), client_(client)
{
    stats_.created = std::chrono::steady_clock::now();
}

ApiResponse Agent::send(const std::string& user_message) {
    // Add user message to history
    history_.push_back(Message{"user", user_message});

    // Build request
    ApiRequest req;
    req.model = config_.model;
    req.system_prompt = config_.build_system_prompt();
    req.max_tokens = config_.max_tokens;
    req.temperature = config_.temperature;
    req.messages = history_;

    auto resp = client_.complete(req);

    if (resp.ok) {
        // Add assistant response to history
        history_.push_back(Message{"assistant", resp.content});
        stats_.total_input_tokens  += resp.input_tokens;
        stats_.total_output_tokens += resp.output_tokens;
        stats_.total_requests++;
    }

    // Auto-trim if history grows large
    if (history_.size() > 40) {
        trim_history(20);
    }

    return resp;
}

void Agent::reset_history() {
    history_.clear();
}

void Agent::trim_history(int keep_last_n) {
    if (static_cast<int>(history_.size()) > keep_last_n) {
        history_.erase(
            history_.begin(),
            history_.begin() + (history_.size() - keep_last_n)
        );
        // Ensure first message is user (API requirement)
        while (!history_.empty() && history_.front().role != "user") {
            history_.erase(history_.begin());
        }
    }
}

std::string Agent::status_summary() const {
    std::ostringstream ss;
    ss << id_ << " | " << config_.role
       << " | msgs:" << history_.size()
       << " | in:" << stats_.total_input_tokens
       << " out:" << stats_.total_output_tokens
       << " | reqs:" << stats_.total_requests;
    return ss.str();
}

std::string Agent::to_json() const {
    auto obj = jobj();
    auto& m = obj->as_object_mut();
    m["id"] = jstr(id_);
    m["config"] = json_parse(config_.to_json());

    auto hist = jarr();
    for (auto& msg : history_) {
        auto mo = jobj();
        mo->as_object_mut()["role"] = jstr(msg.role);
        mo->as_object_mut()["content"] = jstr(msg.content);
        hist->as_array_mut().push_back(mo);
    }
    m["history"] = hist;

    auto st = jobj();
    st->as_object_mut()["input_tokens"]  = jnum(static_cast<double>(stats_.total_input_tokens));
    st->as_object_mut()["output_tokens"] = jnum(static_cast<double>(stats_.total_output_tokens));
    st->as_object_mut()["requests"]      = jnum(static_cast<double>(stats_.total_requests));
    m["stats"] = st;

    return json_serialize(*obj);
}

} // namespace claudius
