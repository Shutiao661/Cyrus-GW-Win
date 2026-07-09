// ============================================================================
// io_engine_iocp.hpp - Windows IOCP (I/O Completion Port) 实现
// ============================================================================
// IOCP 是 Windows 上最高性能的异步 I/O 机制。
// 核心概念:
//   1. 创建 IOCP 句柄 (一个内核对象, 充当完成通知队列)
//   2. 将 socket 关联到 IOCP (通过 CreateIoCompletionPort)
//   3. 投递异步操作 (AcceptEx, WSARecv, WSASend) 并附带 OVERLAPPED 结构
//   4. 工作线程调用 GetQueuedCompletionStatusEx 等待完成通知
//   5. 操作完成时, Windows 内核将结果放入 IOCP 队列
//
// 与 io_uring 的对应关系:
//   IOCP                         io_uring
//   ─────────────────────         ────────
//   CreateIoCompletionPort    →   io_uring_queue_init
//   AcceptEx + OVERLAPPED     →   io_uring_prep_accept + io_uring_submit
//   WSARecv + OVERLAPPED      →   io_uring_prep_recv + io_uring_submit
//   WSASend + OVERLAPPED      →   io_uring_prep_send + io_uring_submit
//   GetQueuedCompletionStatusEx→  io_uring_wait_cqe / io_uring_peek_batch_cqe
// ============================================================================

#pragma once

#include "io_engine.hpp"
#include "cyrus/logger.hpp"

#include <vector>
#include <memory>

namespace cyrus {
namespace gateway {

// ============================================================================
// IOCPContext - IOCP 特定的操作上下文
// ============================================================================
// 扩展基础 IOContext, 添加 IOCP 所需的 OVERLAPPED 结构。
// OVERLAPPED 必须作为第一个成员或通过 CONTAINING_RECORD 访问,
// 因为 GetQueuedCompletionStatusEx 返回的是 OVERLAPPED*。
//
// 设计: 使用多重继承, IOCPContext 同时继承 IOContext 并包含 OVERLAPPED。
// 在 Windows 上, AcceptEx 还需要额外的地址缓冲区。
struct IOCPContext : public IOContext {
    OVERLAPPED overlapped;          // 必须字段: IOCP 完成通知依赖此结构

    // AcceptEx 需要: 为本地和远程地址预留空间
    // 每个 sockaddr_in 是 16 字节, 加上 16 字节填充 (MSDN 推荐)
    alignas(8) uint8_t accept_addr_buffer[2 * (sizeof(sockaddr_in) + 16)];

    // AcceptEx 需要: 预创建的新 socket (在 post_accept 时创建)
    SOCKET accept_socket = INVALID_SOCKET;

    IOCPContext() {
        std::memset(&overlapped, 0, sizeof(overlapped));
        std::memset(accept_addr_buffer, 0, sizeof(accept_addr_buffer));
    }

    // 从 OVERLAPPED* 恢复 IOCPContext*
    // 使用 offsetof 宏确保正确性 (比 CONTAINING_RECORD 更安全)
    static IOCPContext* from_overlapped(OVERLAPPED* ov) {
        // IOCPContext 中的 overlapped 字段偏移量
        // 注意: overlapped 是 IOContext 之后的第一个字段
        return reinterpret_cast<IOCPContext*>(
            reinterpret_cast<char*>(ov) - offsetof(IOCPContext, overlapped));
    }
};

// ============================================================================
// IOEngineIocp - Windows IOCP 引擎实现
// ============================================================================
class IOEngineIocp : public IOEngine {
public:
    IOEngineIocp() = default;
    ~IOEngineIocp() override;

    // --- IOEngine 接口实现 ---
    bool init() override;
    void shutdown() override;
    bool register_socket(socket_t fd, void* user_data) override;

    int post_accept(socket_t listen_fd, IOContext* ctx) override;
    int post_recv(socket_t fd, IOContext* ctx) override;
    int post_send(socket_t fd, IOContext* ctx) override;

    int wait_completions(IOContext** contexts, int max_events,
                          int timeout_ms) override;

    void post_wakeup() override;

    // --- 扩展: IOCPContext 池管理 ---

    // 从池中获取一个 IOCPContext (避免频繁 new/delete)
    IOCPContext* acquire_context();

    // 将 IOCPContext 归还池
    void release_context(IOCPContext* ctx);

private:
    HANDLE m_iocp = nullptr;  // IOCP 句柄 (内核完成队列对象)

    // AcceptEx 函数指针 (需要通过 WSAIoctl 动态加载, 不是标准 Winsock2 API)
    LPFN_ACCEPTEX m_lpfn_accept_ex = nullptr;

    // GetAcceptExSockaddrs 函数指针 (从 AcceptEx 的结果中提取地址)
    LPFN_GETACCEPTEXSOCKADDRS m_lpfn_get_accept_ex_sockaddrs = nullptr;

    // IOCPContext 对象池 (用 vector 模拟, 简单实现)
    std::vector<std::unique_ptr<IOCPContext>> m_context_pool;
    std::vector<IOCPContext*> m_free_contexts;  // 空闲链表
    std::mutex m_pool_mutex;

    // 是否正在关闭
    std::atomic<bool> m_shutting_down{false};
};

} // namespace gateway
} // namespace cyrus
