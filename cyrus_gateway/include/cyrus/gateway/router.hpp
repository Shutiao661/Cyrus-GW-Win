// ============================================================================
// router.hpp - URL 路由器 + Agent 连接池
// ============================================================================
// Router 负责:
//   1. URL 路由匹配: 根据 HTTP 方法和 URI 找到对应的处理函数
//   2. Agent 连接池: 管理与后端 Agent 服务器的 TCP 连接 (round-robin)
//   3. 请求代理: 将客户端请求转发给 Agent, 并回传响应
//
// 路由模式:
//   GET  /                         → 欢迎页面
//   GET  /health                   → 健康检查
//   POST /v1/chat/completions      → 代理到 Agent (SSE 流式)
//   其他                            → 404 Not Found
// ============================================================================

#pragma once

#include "cyrus/types.hpp"
#include "cyrus/config.hpp"
#include "http_parser.hpp"
#include "agent_client.hpp"

#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <mutex>

namespace cyrus {
namespace gateway {

// 前向声明
class Connection;

// ============================================================================
// RouteHandler - 路由处理函数类型
// ============================================================================
// conn: 当前连接 (用于发送响应)
// request: 解析完成的 HTTP 请求
using RouteHandler = std::function<void(Connection* conn, const ParsedRequest& request)>;

// ============================================================================
// Router - 路由器
// ============================================================================
class Router {
public:
    explicit Router(const Config& config);
    ~Router();

    // --- 初始化 (连接到 Agent 后端) ---
    bool init();

    // --- 路由请求 ---
    // 根据请求的 method + URI 找到处理函数并执行
    // 返回 true 表示路由成功 (找到匹配的处理函数)
    // 返回 false 表示 404 (没有匹配的路由)
    bool route(Connection* conn, const ParsedRequest& request);

    // --- Agent 连接池 ---

    // 获取一个可用的 Agent 客户端 (round-robin)
    AgentClient* acquire_agent_client();

    // 归还 Agent 客户端 (不关闭连接)
    void release_agent_client(AgentClient* client);

private:
    // --- 路由表 ---
    struct Route {
        HttpMethod method;
        std::string path;           // 精确匹配模式
        RouteHandler handler;
    };
    std::vector<Route> routes_;

    // --- Agent 连接池 ---
    std::vector<std::unique_ptr<AgentClient>> agent_pool_;
    std::mutex pool_mutex_;
    size_t next_agent_index_ = 0;   // round-robin 索引

    // --- Agent 配置 ---
    Config config_;                      // 完整配置 (保留用于 SSE timeout 等)
    std::string agent_host_;
    int agent_port_;
    int agent_pool_size_;

    // --- 连接池管理 ---
    void health_check();  // 后台健康检查 + 自动重连

    // --- 默认处理函数 ---
    void handle_404(Connection* conn, const ParsedRequest& request);
    void handle_welcome(Connection* conn, const ParsedRequest& request);
    void handle_health(Connection* conn, const ParsedRequest& request);
    void handle_chat_completion(Connection* conn, const ParsedRequest& request);
};

} // namespace gateway
} // namespace cyrus
