// claudius/src/api_client.cpp — Claude API client implementation
#include "api_client.h"

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <algorithm>

#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h>

static constexpr const char* API_HOST = "api.anthropic.com";
static constexpr int         API_PORT = 443;
static constexpr const char* API_PATH = "/v1/messages";
static constexpr const char* API_VERSION = "2023-06-01";

namespace claudius {

ApiClient::ApiClient(const std::string& api_key) : api_key_(api_key) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    ssl_ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx_) throw std::runtime_error("Failed to create SSL context");

    SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(ssl_ctx_);
}

ApiClient::~ApiClient() {
    close_connection();
    if (ssl_ctx_) SSL_CTX_free(ssl_ctx_);
}

bool ApiClient::ensure_connection() {
    if (connected_) return true;

    // Resolve host
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(API_HOST, "443", &hints, &res) != 0)
        return false;

    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_ < 0) { freeaddrinfo(res); return false; }

    // TCP_NODELAY for low latency
    int flag = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    if (connect(sock_, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        close(sock_);
        sock_ = -1;
        return false;
    }
    freeaddrinfo(res);

    // TLS handshake
    ssl_ = SSL_new(ssl_ctx_);
    SSL_set_fd(ssl_, sock_);
    SSL_set_tlsext_host_name(ssl_, API_HOST);

    if (SSL_connect(ssl_) != 1) {
        SSL_free(ssl_);
        ssl_ = nullptr;
        close(sock_);
        sock_ = -1;
        return false;
    }

    connected_ = true;
    return true;
}

void ApiClient::close_connection() {
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
    connected_ = false;
}

std::string ApiClient::build_request_body(const ApiRequest& req, bool streaming) {
    auto obj = jobj();
    auto& m = obj->as_object_mut();
    m["model"] = jstr(req.model);
    m["max_tokens"] = jnum(static_cast<double>(req.max_tokens));
    m["temperature"] = jnum(req.temperature);

    if (!req.system_prompt.empty()) {
        m["system"] = jstr(req.system_prompt);
    }

    auto msgs = jarr();
    for (auto& msg : req.messages) {
        auto mo = jobj();
        mo->as_object_mut()["role"] = jstr(msg.role);
        mo->as_object_mut()["content"] = jstr(msg.content);
        msgs->as_array_mut().push_back(mo);
    }
    m["messages"] = msgs;

    if (streaming) {
        m["stream"] = jbool(true);
    }

    return json_serialize(*obj);
}

std::string ApiClient::send_request(const std::string& body, bool streaming) {
    std::ostringstream http;
    http << "POST " << API_PATH << " HTTP/1.1\r\n";
    http << "Host: " << API_HOST << "\r\n";
    http << "Content-Type: application/json\r\n";
    http << "x-api-key: " << api_key_ << "\r\n";
    http << "anthropic-version: " << API_VERSION << "\r\n";
    http << "Content-Length: " << body.size() << "\r\n";
    if (streaming) http << "Accept: text/event-stream\r\n";
    http << "Connection: keep-alive\r\n";
    http << "\r\n";
    http << body;

    std::string raw = http.str();
    int total = static_cast<int>(raw.size());
    int sent = 0;
    while (sent < total) {
        int n = SSL_write(ssl_, raw.data() + sent, total - sent);
        if (n <= 0) {
            close_connection();
            throw std::runtime_error("SSL write failed");
        }
        sent += n;
    }

    return {};  // response read separately
}

std::string ApiClient::read_response(bool streaming, StreamCallback cb) {
    std::string raw;
    raw.reserve(8192);
    char buf[4096];

    // Read headers first
    std::string headers;
    while (true) {
        int n = SSL_read(ssl_, buf, 1);
        if (n <= 0) break;
        headers += buf[0];
        if (headers.size() >= 4 &&
            headers.substr(headers.size() - 4) == "\r\n\r\n") break;
    }

    // Parse content-length or chunked
    bool chunked = headers.find("Transfer-Encoding: chunked") != std::string::npos;
    int content_length = -1;
    auto cl_pos = headers.find("Content-Length: ");
    if (cl_pos != std::string::npos) {
        content_length = std::atoi(headers.c_str() + cl_pos + 16);
    }

    if (chunked) {
        // Read chunked encoding
        while (true) {
            // Read chunk size line
            std::string size_line;
            while (true) {
                int n = SSL_read(ssl_, buf, 1);
                if (n <= 0) goto done;
                if (buf[0] == '\n') break;
                if (buf[0] != '\r') size_line += buf[0];
            }
            int chunk_size = static_cast<int>(std::strtol(size_line.c_str(), nullptr, 16));
            if (chunk_size == 0) break;

            // Read chunk data
            int read_so_far = 0;
            while (read_so_far < chunk_size) {
                int to_read = std::min(chunk_size - read_so_far, (int)sizeof(buf));
                int n = SSL_read(ssl_, buf, to_read);
                if (n <= 0) goto done;
                raw.append(buf, n);

                if (streaming && cb) {
                    // Parse SSE events for streaming
                    // (simplified: extract text deltas)
                    std::string chunk_str(buf, n);
                    // Look for content_block_delta events
                    size_t dpos = 0;
                    while ((dpos = chunk_str.find("\"text\":\"", dpos)) != std::string::npos) {
                        dpos += 8;
                        size_t end = chunk_str.find("\"", dpos);
                        if (end != std::string::npos) {
                            cb(chunk_str.substr(dpos, end - dpos));
                            dpos = end;
                        }
                    }
                }

                read_so_far += n;
            }
            // Read trailing \r\n
            SSL_read(ssl_, buf, 2);
        }
        // Read trailing headers/final \r\n
        SSL_read(ssl_, buf, 2);
    } else if (content_length > 0) {
        int read_so_far = 0;
        while (read_so_far < content_length) {
            int to_read = std::min(content_length - read_so_far, (int)sizeof(buf));
            int n = SSL_read(ssl_, buf, to_read);
            if (n <= 0) break;
            raw.append(buf, n);
            read_so_far += n;
        }
    }

done:
    return raw;
}

ApiResponse ApiClient::parse_response(const std::string& body) {
    ApiResponse resp;
    resp.raw_body = body;

    try {
        auto root = json_parse(body);

        // Check for error
        auto err = root->get("error");
        if (err && err->is_object()) {
            resp.ok = false;
            resp.error = err->get_string("message", "Unknown API error");
            return resp;
        }

        // Extract content
        auto content = root->get("content");
        if (content && content->is_array()) {
            for (auto& block : content->as_array()) {
                if (block && block->get_string("type") == "text") {
                    resp.content += block->get_string("text");
                }
            }
        }

        resp.stop_reason = root->get_string("stop_reason");

        // Token usage
        auto usage = root->get("usage");
        if (usage && usage->is_object()) {
            resp.input_tokens  = usage->get_int("input_tokens");
            resp.output_tokens = usage->get_int("output_tokens");
        }

        resp.ok = true;
    } catch (const std::exception& e) {
        resp.ok = false;
        resp.error = std::string("Parse error: ") + e.what();
    }

    return resp;
}

ApiResponse ApiClient::complete(const ApiRequest& req) {
    std::lock_guard<std::mutex> lock(conn_mutex_);

    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            if (!ensure_connection()) {
                return ApiResponse{false, "", 0, 0, "Connection failed"};
            }

            std::string body = build_request_body(req, false);
            send_request(body, false);
            std::string raw = read_response(false, nullptr);

            auto resp = parse_response(raw);
            total_in_  += resp.input_tokens;
            total_out_ += resp.output_tokens;
            return resp;

        } catch (...) {
            close_connection();
            if (attempt == 1) {
                return ApiResponse{false, "", 0, 0, "Request failed after retry"};
            }
        }
    }

    return ApiResponse{false, "", 0, 0, "Unreachable"};
}

ApiResponse ApiClient::stream(const ApiRequest& req, StreamCallback cb) {
    std::lock_guard<std::mutex> lock(conn_mutex_);

    try {
        if (!ensure_connection()) {
            return ApiResponse{false, "", 0, 0, "Connection failed"};
        }

        std::string body = build_request_body(req, true);
        send_request(body, true);
        std::string raw = read_response(true, cb);

        // For streaming, parse the accumulated SSE data to get final usage
        // Find the last message_stop or message_delta with usage
        ApiResponse resp;
        resp.ok = true;
        resp.raw_body = raw;

        // Extract accumulated text from SSE events
        std::istringstream stream_data(raw);
        std::string line;
        while (std::getline(stream_data, line)) {
            if (line.find("\"type\":\"message_delta\"") != std::string::npos) {
                try {
                    // Find the JSON object in this SSE event
                    auto pos = line.find("{");
                    if (pos != std::string::npos) {
                        // Try to find usage in the delta
                        auto upos = line.find("\"output_tokens\":");
                        if (upos != std::string::npos) {
                            resp.output_tokens = std::atoi(line.c_str() + upos + 16);
                        }
                    }
                } catch (...) {}
            }
        }

        total_in_  += resp.input_tokens;
        total_out_ += resp.output_tokens;
        return resp;

    } catch (const std::exception& e) {
        close_connection();
        return ApiResponse{false, "", 0, 0, std::string("Stream error: ") + e.what()};
    }
}

} // namespace claudius
