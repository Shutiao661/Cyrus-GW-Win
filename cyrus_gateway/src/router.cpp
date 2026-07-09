// ============================================================================
// router.cpp - 路由器实现
// ============================================================================

#include "cyrus/gateway/router.hpp"
#include "cyrus/gateway/connection.hpp"
#include "cyrus/gateway/sse_handler.hpp"
#include "cyrus/logger.hpp"

#include <sstream>
#include <cstring>

namespace cyrus {
namespace gateway {

// ============================================================================
// 构造/析构
// ============================================================================
Router::Router(const Config& config) : config_(config) {
    agent_host_      = config_.get("agent", "host", "127.0.0.1");
    agent_port_      = config_.get_int("agent", "port", 9999);
    agent_pool_size_ = config_.get_int("agent", "pool_size", 4);
}

Router::~Router() {
    // Agent 连接通过 unique_ptr 自动关闭
}

// ============================================================================
// init() - 初始化路由表和 Agent 连接池
// ============================================================================
bool Router::init() {
    // --- 注册路由 ---
    routes_ = {
        { HttpMethod::GET,  "/",                   [this](auto* c, auto& r) { handle_welcome(c, r); } },
        { HttpMethod::GET,  "/health",             [this](auto* c, auto& r) { handle_health(c, r); } },
        { HttpMethod::POST, "/v1/chat/completions", [this](auto* c, auto& r) { handle_chat_completion(c, r); } },
        { HttpMethod::POST, "/v1/chat",             [this](auto* c, auto& r) { handle_chat_completion(c, r); } },
    };

    // --- 创建 Agent 连接池 ---
    LOG_INFO("Connecting to Agent backend at {}:{} (pool: {})",
             agent_host_, agent_port_, agent_pool_size_);

    agent_pool_.reserve(agent_pool_size_);
    for (int i = 0; i < agent_pool_size_; ++i) {
        auto client = std::make_unique<AgentClient>();
        if (client->connect(agent_host_, agent_port_)) {
            LOG_DEBUG("Agent client #{} connected", i);
            agent_pool_.push_back(std::move(client));
        } else {
            LOG_WARN("Agent client #{} failed to connect", i);
        }
    }

    if (agent_pool_.empty()) {
        LOG_WARN("No agent connections available (agent may not be running)");
        // 不致命: 网关仍可处理 /health 等不依赖 Agent 的路由
    }

    LOG_INFO("Router initialized: {} routes, {} agent connections",
             routes_.size(), agent_pool_.size());
    return true;
}

// ============================================================================
// route() - 路由请求
// ============================================================================
bool Router::route(Connection* conn, const ParsedRequest& request) {
    for (const auto& route : routes_) {
        if (route.method == request.method && route.path == request.uri) {
            route.handler(conn, request);
            return true;
        }
    }
    // 未匹配 → 404
    handle_404(conn, request);
    return false;
}

// ============================================================================
// Agent 连接池管理
// ============================================================================
AgentClient* Router::acquire_agent_client() {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    if (agent_pool_.empty()) {
        return nullptr;
    }

    // Round-robin 轮询 + 健康检查: 跳过已断开或标记为不健康的连接
    size_t pool_size = agent_pool_.size();
    for (size_t attempt = 0; attempt < pool_size; ++attempt) {
        size_t index = next_agent_index_ % pool_size;
        next_agent_index_++;

        auto& client = agent_pool_[index];
        if (client->is_connected()) {
            return client.get();
        }

        // 连接已断开, 尝试重连
        LOG_WARN("Agent client #{} disconnected, attempting reconnect to {}:{}",
                 index, agent_host_, agent_port_);
        if (client->connect(agent_host_, agent_port_)) {
            LOG_INFO("Agent client #{} reconnected successfully", index);
            return client.get();
        }
        LOG_WARN("Agent client #{} reconnect failed", index);
    }

    // 所有连接都不健康
    LOG_ERROR("All {} agent connections are unhealthy", pool_size);
    return nullptr;
}

void Router::release_agent_client(AgentClient* client) {
    // 连接保持打开供复用 (长连接池模式)
    // 如果连接已断开, 在下次 acquire 时自动重连
    (void)client;

    // 定期健康检查: 每 100 次 release 触发一轮心跳
    static int release_counter = 0;
    if (++release_counter % 100 == 0) {
        health_check();
    }
}

// 后台健康检查: 向所有 Agent 连接发送心跳, 标记不健康连接
void Router::health_check() {
    std::lock_guard<std::mutex> lock(pool_mutex_);

    int healthy = 0;
    int unhealthy = 0;

    for (size_t i = 0; i < agent_pool_.size(); ++i) {
        auto& client = agent_pool_[i];
        if (client->is_connected()) {
            healthy++;
        } else {
            unhealthy++;
            // 尝试后台重连
            if (client->connect(agent_host_, agent_port_)) {
                healthy++;
                unhealthy--;
                LOG_INFO("Agent client #{} reconnected during health check", i);
            }
        }
    }

    if (unhealthy > 0) {
        LOG_WARN("Agent pool health: {} healthy, {} unhealthy (of {} total)",
                 healthy, unhealthy, agent_pool_.size());
    }
}

// ============================================================================
// 路由处理函数
// ============================================================================

// --- 404 Not Found ---
void Router::handle_404(Connection* conn, const ParsedRequest& request) {
    std::string body = std::format("404 Not Found: {} {}\n",
                                    http_method_to_sv(request.method),
                                    request.uri);
    conn->send_error(HttpStatus::NOT_FOUND, body.c_str());
    LOG_DEBUG("404: {} {}", http_method_to_sv(request.method), request.uri);
}

// --- 欢迎页面 ---
void Router::handle_welcome(Connection* conn, const ParsedRequest& request) {
    const char* html = R"(<!DOCTYPE html>
<html>
<head><title>Cyrus-GW</title><meta charset="utf-8">
<style>body{font-family:sans-serif;max-width:700px;margin:40px auto;padding:0 20px;line-height:1.6}
h1{color:#333}code{background:#f4f4f4;padding:2px 6px;border-radius:3px}
.endpoint{background:#f8f8f8;padding:8px 12px;margin:4px 0;border-left:3px solid #4CAF50}
</style></head>
<body>
<h1> Cyrus-GW Gateway</h1>
<p>High-performance async HTTP gateway powered by C++20 + Windows IOCP</p>
<h2>Endpoints</h2>
<div class="endpoint"><code>GET /health</code> — Health check</div>
<div class="endpoint"><code>POST /v1/chat/completions</code> — Chat completion (SSE streaming)</div>
<p><small>Platform: Windows | Engine: IOCP | Version: 1.0.0</small></p>
</body>
</html>)";
    conn->send_response(HttpStatus::OK, "text/html; charset=utf-8", html);
}

// --- 健康检查 ---
void Router::handle_health(Connection* conn, const ParsedRequest& request) {
    std::string json = std::format(
        R"({{"status":"ok","version":"2.0.0","platform":"Windows","engine":"IOCP","uptime":"ok"}})");
    conn->send_response(HttpStatus::OK, "application/json", json);
}

// --- Chat Completion (代理到 Agent + SSE 流式透传) ---
void Router::handle_chat_completion(Connection* conn, const ParsedRequest& request) {
    // 获取 Agent 客户端连接
    AgentClient* agent = acquire_agent_client();
    if (!agent) {
        LOG_ERROR("No agent available for chat completion");
        conn->send_error(HttpStatus::SERVICE_UNAVAILABLE,
            R"({"error":{"message":"No agent backend available","type":"gateway_error","code":503}})");
        return;
    }

    // 编码请求为二进制协议帧
    std::string body_str(request.body.begin(), request.body.end());
    std::vector<std::pair<std::string, std::string>> headers;
    for (const auto& [name, value] : request.headers) {
        headers.emplace_back(name, value);
    }

    auto frame = ProtocolCodec::encode_request(
        1,  // request_id
        http_method_to_sv(request.method),
        request.uri,
        headers,
        body_str);

    // 发送到 Agent (阻塞, localhost)
    if (!agent->send_packet(frame)) {
        LOG_ERROR("Failed to send request to agent");
        conn->send_error(HttpStatus::BAD_GATEWAY,
            R"({"error":{"message":"Failed to communicate with agent","type":"gateway_error","code":502}})");
        release_agent_client(agent);
        return;
    }

    // --- SSE 流式透传: 接收 Agent → 累积 SSE → 一次性 send_response ---
    // 简化版本: 先收集所有 chunk, 再批量发送
    // TODO: 生产版本需要非阻塞 Agent recv + 异步逐帧 send
    SSERelayTimeout sse_timeout = SSERelayTimeout::from_config(config_);
    SSERelayHandler relay(sse_timeout);

    std::string accumulated_sse;
    accumulated_sse.reserve(8192);

    relay.on_send = [&accumulated_sse](std::string_view sse_data, bool /*is_last*/) {
        accumulated_sse.append(sse_data);
    };

    // 接收 Agent 流式响应
    std::vector<uint8_t> frame_data;
    bool stream_ok = false;

    while (agent->recv_packet(frame_data, 30000)) {
        if (frame_data.size() < sizeof(MessageHeader)) {
            frame_data.clear();
            continue;
        }

        MessageHeader resp_header;
        std::memcpy(&resp_header, frame_data.data(), sizeof(MessageHeader));
        size_t payload_offset = sizeof(MessageHeader);

        switch (resp_header.msg_type) {
            case MessageType::MSG_RESPONSE_HEADERS:
                // Agent 返回的响应头: 验证状态码, 提取元数据
                // 非 200 响应 → 转为错误事件
                if (resp_header.flags & FrameFlags::FLAG_ERROR) {
                    relay.on_agent_error("Agent returned error status", 502);
                    goto done_receiving;
                }
                break;

            case MessageType::MSG_RESPONSE_DATA: {
                std::string_view chunk(
                    reinterpret_cast<const char*>(frame_data.data() + payload_offset),
                    frame_data.size() - payload_offset);
                relay.on_agent_data(chunk);
                break;
            }
            case MessageType::MSG_RESPONSE_END:
                relay.on_stream_complete();
                stream_ok = true;
                goto done_receiving;

            case MessageType::MSG_ERROR: {
                std::string_view err(
                    reinterpret_cast<const char*>(frame_data.data() + payload_offset),
                    frame_data.size() - payload_offset);
                relay.on_agent_error(err, 502);
                goto done_receiving;
            }

            default:
                break;
        }

        frame_data.clear();

        if (!relay.check_timeout()) {
            LOG_WARN("SSE relay timeout");
            break;
        }
    }

    // Agent 连接断开或超时
    if (!stream_ok && relay.is_active()) {
        relay.on_agent_error("Agent connection lost", 504);
    }

done_receiving:
    release_agent_client(agent);

    // 构建完整 SSE HTTP 响应并发送
    std::string full_response;
    full_response.reserve(256 + accumulated_sse.size());
    full_response += relay.build_sse_header();
    full_response += accumulated_sse;

    conn->send_sse_response(full_response);

    LOG_INFO("Chat completion SSE relay ended (stream_ok={}, sse_bytes={})",
             stream_ok, accumulated_sse.size());
}

} // namespace gateway
} // namespace cyrus
