// ============================================================================
// agent_client.hpp - Agent TCP 客户端
// ============================================================================
// 管理与后端 Cyrus Agent 的 TCP 连接。
// 使用二进制协议帧 (ProtocolCodec/ProtocolDecoder) 进行通信。
//
// 工作流程:
//   1. 连接到 Agent (TCP, 127.0.0.1:9999)
//   2. 发送编码的请求帧
//   3. 接收响应帧 (通过回调通知路由器)
//
// 连接复用: 一个 AgentClient 可以被多个请求复用 (串行)
// 连接池: Router 维护多个 AgentClient 实例 (round-robin 负载均衡)
// ============================================================================

#pragma once

#include "cyrus/types.hpp"
#include "cyrus/protocol.hpp"

#include <string>
#include <functional>

namespace cyrus {
namespace gateway {

class Router;  // 前向声明

class AgentClient {
public:
    AgentClient() = default;
    ~AgentClient();

    // 禁止拷贝
    AgentClient(const AgentClient&) = delete;
    AgentClient& operator=(const AgentClient&) = delete;

    // --- 连接管理 ---

    // 连接到 Agent 服务器
    // host: 服务器地址 (如 "127.0.0.1")
    // port: 服务器端口 (如 9999)
    // 返回 true 表示连接成功
    bool connect(const std::string& host, int port);

    // 断开连接
    void disconnect();

    // 是否已连接
    bool is_connected() const noexcept { return fd_ != INVALID_SOCKET_VAL; }

    // --- 数据收发 ---

    // 发送二进制协议帧 (阻塞发送)
    // data: 编码后的帧数据 (含长度前缀)
    // 返回 true 表示发送成功
    bool send_packet(const std::vector<uint8_t>& data);

    // 接收响应 (阻塞, 带超时)
    // out_frame: 输出解码后的帧 (payload only)
    // timeout_ms: 超时时间 (毫秒)
    // 返回 true 表示接收成功
    bool recv_packet(std::vector<uint8_t>& out_frame, int timeout_ms = 5000);

    // --- 回调设置 ---

    // 设置路由器引用 (用于接收 Agent 响应后回调)
    void set_router(Router* router) { router_ = router; }

    // 获取 socket fd
    socket_t fd() const noexcept { return fd_; }

private:
    socket_t fd_ = INVALID_SOCKET_VAL;
    Router* router_ = nullptr;
    std::string host_;
    int port_ = 0;
};

} // namespace gateway
} // namespace cyrus
