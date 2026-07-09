// ============================================================================
// io_engine_iocp.cpp - Windows IOCP 引擎完整实现
// ============================================================================
// 这是整个 Windows 移植中最关键的文件 (~450 行)
//
// 实现 IOCP (I/O Completion Port) 异步 I/O 引擎
//
// IOCP 工作流程:
//   1. 创建 IOCP 句柄 (内核完成队列)
//   2. 将 socket 关联到 IOCP (CreateIoCompletionPort)
//   3. 投递异步操作:
//       - Accept: 使用 AcceptEx (需要预创建 socket 和地址缓冲区)
//       - Recv:   使用 WSARecv (需要预分配接收缓冲区)
//       - Send:   使用 WSASend
//   4. 工作线程调用 GetQueuedCompletionStatusEx 等待完成通知
//   5. 操作完成 → 内核将结果放入 IOCP 队列 → GetQueuedCompletionStatusEx 返回
//
// 关键注意事项:
//   - AcceptEx 不是标准 Winsock2 的一部分, 需要通过 WSAIoctl 加载函数指针
//   - OVERLAPPED 结构必须在操作完成前保持有效
//   - AcceptEx 需要预创建 accept socket (不能用监听 socket 的 fd)
//   - 完成后的 socket 需要调用 setsockopt(SO_UPDATE_ACCEPT_CONTEXT)
//   - 线程安全: 多个线程可以并发调用 GetQueuedCompletionStatusEx
// ============================================================================

#include "cyrus/gateway/io_engine_iocp.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

namespace cyrus {
namespace gateway {

// ============================================================================
// IOEngineIocp 析构
// ============================================================================
IOEngineIocp::~IOEngineIocp() {
    shutdown();
}

// ============================================================================
// init() - 初始化 IOCP 引擎
// ============================================================================
// 步骤:
//   1. 创建 IOCP 句柄 (内核完成队列)
//   2. 加载 AcceptEx 和 GetAcceptExSockaddrs 函数指针
//   3. 预分配 IOCPContext 对象池
// ============================================================================
bool IOEngineIocp::init() {
    // --- 第 1 步: 创建 IOCP ---
    // 参数:
    //   INVALID_HANDLE_VALUE  → 创建新的 IOCP (而非关联到已有文件)
    //   NULL                  → 不关联已有句柄
    //   0                     → 完成键 (暂时为 0)
    //   0                     → 并发线程数 (0 = 使用系统 CPU 核心数)
    m_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (m_iocp == nullptr) {
        LOG_ERROR("CreateIoCompletionPort failed: {}", GetLastError());
        return false;
    }

    // --- 第 2 步: 加载 AcceptEx 函数指针 ---
    // AcceptEx 是 Microsoft 扩展, 不在标准 Winsock2 导出表中
    // 需要通过 WSAIoctl 获取函数指针
    // 使用临时 socket 作为查询目标 (任何 socket 都可以)
    SOCKET tmp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tmp_socket == INVALID_SOCKET) {
        LOG_ERROR("Failed to create temporary socket for loading AcceptEx: {}",
                  WSAGetLastError());
        CloseHandle(m_iocp);
        m_iocp = nullptr;
        return false;
    }

    // 加载 AcceptEx
    GUID guid_accept_ex = WSAID_ACCEPTEX;
    DWORD bytes_returned = 0;
    int result = WSAIoctl(
        tmp_socket,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guid_accept_ex, sizeof(guid_accept_ex),
        &m_lpfn_accept_ex, sizeof(m_lpfn_accept_ex),
        &bytes_returned, nullptr, nullptr);

    if (result == SOCKET_ERROR) {
        LOG_ERROR("Failed to load AcceptEx: {}", WSAGetLastError());
        closesocket(tmp_socket);
        CloseHandle(m_iocp);
        m_iocp = nullptr;
        return false;
    }

    // 加载 GetAcceptExSockaddrs
    GUID guid_get_accept_ex_sockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
    result = WSAIoctl(
        tmp_socket,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guid_get_accept_ex_sockaddrs, sizeof(guid_get_accept_ex_sockaddrs),
        &m_lpfn_get_accept_ex_sockaddrs, sizeof(m_lpfn_get_accept_ex_sockaddrs),
        &bytes_returned, nullptr, nullptr);

    if (result == SOCKET_ERROR) {
        LOG_ERROR("Failed to load GetAcceptExSockaddrs: {}", WSAGetLastError());
        closesocket(tmp_socket);
        CloseHandle(m_iocp);
        m_iocp = nullptr;
        return false;
    }

    closesocket(tmp_socket);

    // --- 第 3 步: 预分配 IOCPContext 对象池 ---
    // 高并发时避免频繁 new/delete, 预分配 256 个上下文
    constexpr int INITIAL_POOL_SIZE = 256;
    m_context_pool.reserve(INITIAL_POOL_SIZE);
    m_free_contexts.reserve(INITIAL_POOL_SIZE);
    for (int i = 0; i < INITIAL_POOL_SIZE; ++i) {
        auto ctx = std::make_unique<IOCPContext>();
        m_free_contexts.push_back(ctx.get());
        m_context_pool.push_back(std::move(ctx));
    }

    LOG_INFO("IOEngineIocp initialized successfully (pool: {} contexts)", INITIAL_POOL_SIZE);
    return true;
}

// ============================================================================
// shutdown() - 关闭引擎
// ============================================================================
// 设置关闭标志, 向 IOCP 投递唤醒事件以释放阻塞的线程, 关闭 IOCP 句柄
void IOEngineIocp::shutdown() {
    if (m_iocp == nullptr) return;

    m_shutting_down.store(true, std::memory_order_release);

    // 投递多个唤醒事件 (每个可能阻塞的工作线程一个)
    for (int i = 0; i < 64; ++i) {
        PostQueuedCompletionStatus(m_iocp, 0, 0, nullptr);
    }

    // 短暂等待线程处理唤醒事件
    Sleep(100);

    CloseHandle(m_iocp);
    m_iocp = nullptr;

    // 清理对象池
    m_free_contexts.clear();
    m_context_pool.clear();

    LOG_INFO("IOEngineIocp shutdown complete");
}

// ============================================================================
// register_socket() - 将 socket 关联到 IOCP
// ============================================================================
// 将 socket 与 IOCP 关联, 并附加 user_data 作为 CompletionKey
// 当此 socket 上的操作完成时, GetQueuedCompletionStatusEx 会返回此 user_data
bool IOEngineIocp::register_socket(socket_t fd, void* user_data) {
    HANDLE result = CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(fd),  // 要关联的 socket
        m_iocp,                         // 目标 IOCP
        reinterpret_cast<ULONG_PTR>(user_data),  // CompletionKey (完成时返回)
        0);                             // 并发数 (0 = 继承)

    if (result == nullptr) {
        LOG_ERROR("Failed to register socket {} with IOCP: {}", static_cast<int>(fd), GetLastError());
        return false;
    }
    return true;
}

// ============================================================================
// post_accept() - 投递异步 Accept 操作
// ============================================================================
// 使用 AcceptEx 投递异步 accept。
// AcceptEx 要求:
//   1. 预创建 accept socket (不能用监听 socket)
//   2. 预分配地址缓冲区 (为本地和远程地址预留空间)
//   3. 最初零字节接收 (接收的数据在后续 recv 中处理)
//
// AcceptEx 与标准 accept() 的区别:
//   - AcceptEx 是异步的, accept() 是阻塞的
//   - AcceptEx 需要预创建 socket, accept() 自动创建
//   - AcceptEx 返回后需要调用 SO_UPDATE_ACCEPT_CONTEXT
int IOEngineIocp::post_accept(socket_t listen_fd, IOContext* base_ctx) {
    // 转换为 IOCP 特定上下文
    IOCPContext* ctx = static_cast<IOCPContext*>(base_ctx);

    // --- 第 1 步: 创建 accept socket ---
    // 使用 WSA_FLAG_OVERLAPPED 标志创建支持重叠 I/O 的 socket
    SOCKET accept_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (accept_sock == INVALID_SOCKET) {
        LOG_ERROR("post_accept: socket() failed: {}", WSAGetLastError());
        return -1;
    }

    // 保存到上下文中 (完成时使用)
    ctx->accept_socket = accept_sock;

    // 重置 OVERLAPPED 结构
    std::memset(&ctx->overlapped, 0, sizeof(OVERLAPPED));
    ctx->op = IOOperation::ACCEPT;
    ctx->fd = listen_fd;

    // --- 第 2 步: 调用 AcceptEx ---
    // AcceptEx 参数:
    //   listen_fd         → 监听 socket
    //   accept_sock       → 预创建的 socket (将成为新连接)
    //   accept_addr_buffer→ 地址缓冲区 (接收本地和远程地址)
    //   0                 → 地址缓冲区中留给本地地址的空间
    //   addr_len          → 每个地址预留的空间
    //   addr_len          → 每个地址预留的空间
    //   &bytes_received   → 接收的字节数 (总是 0, 因为 dwReceiveDataLength = 0)
    //   &ctx->overlapped  → OVERLAPPED 结构
    constexpr DWORD addr_len = sizeof(sockaddr_in) + 16;

    BOOL success = m_lpfn_accept_ex(
        listen_fd,
        accept_sock,
        ctx->accept_addr_buffer,
        0,                  // 不接收数据 (dwReceiveDataLength = 0)
        addr_len,
        addr_len,
        nullptr,            // 不需要 bytes_received 输出 (用完成通知获取)
        &ctx->overlapped);

    if (!success) {
        int err = WSAGetLastError();
        // ERROR_IO_PENDING 是正常情况 (操作已加入队列, 等待完成)
        if (err != ERROR_IO_PENDING) {
            LOG_ERROR("post_accept: AcceptEx failed: {}", err);
            closesocket(accept_sock);
            ctx->accept_socket = INVALID_SOCKET;
            return -1;
        }
    }

    return 0;  // 成功投递
}

// ============================================================================
// post_recv() - 投递异步 Recv 操作
// ============================================================================
int IOEngineIocp::post_recv(socket_t fd, IOContext* base_ctx) {
    IOCPContext* ctx = static_cast<IOCPContext*>(base_ctx);

    std::memset(&ctx->overlapped, 0, sizeof(OVERLAPPED));
    ctx->op = IOOperation::RECV;
    ctx->fd = fd;
    ctx->bytes_transferred = 0;
    ctx->error = 0;

    // 准备 WSABUF (Winsock 缓冲区描述符)
    WSABUF wsa_buf;
    wsa_buf.buf = reinterpret_cast<CHAR*>(ctx->buffer);
    wsa_buf.len = static_cast<ULONG>(ctx->buffer_len);

    DWORD flags = 0;
    int result = WSARecv(
        fd,
        &wsa_buf, 1,            // 1 个缓冲区
        nullptr,                 // 不需要 bytes_received 输出
        &flags,
        &ctx->overlapped,
        nullptr);               // 不需要完成回调

    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            LOG_ERROR("post_recv: WSARecv failed on fd {}: {}", static_cast<int>(fd), err);
            return -1;
        }
    }

    return 0;
}

// ============================================================================
// post_send() - 投递异步 Send 操作
// ============================================================================
int IOEngineIocp::post_send(socket_t fd, IOContext* base_ctx) {
    IOCPContext* ctx = static_cast<IOCPContext*>(base_ctx);

    std::memset(&ctx->overlapped, 0, sizeof(OVERLAPPED));
    ctx->op = IOOperation::SEND;
    ctx->fd = fd;

    // 准备发送缓冲区 (使用 bytes_transferred 作为待发送长度)
    WSABUF wsa_buf;
    wsa_buf.buf = reinterpret_cast<CHAR*>(ctx->buffer);
    wsa_buf.len = static_cast<ULONG>(ctx->bytes_transferred);

    int result = WSASend(
        fd,
        &wsa_buf, 1,
        nullptr,                 // 不需要 bytes_sent 输出
        0,                        // 无特殊标志
        &ctx->overlapped,
        nullptr);

    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            LOG_ERROR("post_send: WSASend failed on fd {}: {}", static_cast<int>(fd), err);
            return -1;
        }
    }

    return 0;
}

// ============================================================================
// wait_completions() - 等待完成事件 (核心事件循环)
// ============================================================================
// 调用 GetQueuedCompletionStatusEx 批量获取完成事件。
// 这是工作线程的主循环的核心 — 每个工作线程阻塞在此函数上,
// 等待 I/O 完成事件, 然后分发处理。
//
// 参考: io_uring 的 io_uring_peek_batch_cqe() + io_uring_submit_and_wait()
int IOEngineIocp::wait_completions(IOContext** contexts, int max_events,
                                    int timeout_ms) {
    if (m_shutting_down.load(std::memory_order_acquire)) {
        return 0;
    }

    // 准备 OVERLAPPED_ENTRY 数组 (批量接收完成事件)
    constexpr int MAX_ENTRIES = 128;
    OVERLAPPED_ENTRY entries[MAX_ENTRIES];
    ULONG count = 0;

    // 限制单次获取数量
    int to_get = (max_events < MAX_ENTRIES) ? max_events : MAX_ENTRIES;

    // --- 调用 GetQueuedCompletionStatusEx ---
    // 参数:
    //   m_iocp      → IOCP 句柄
    //   entries     → 输出: 完成事件数组
    //   to_get      → 最多获取的事件数
    //   &count      → 输出: 实际获取的事件数
    //   timeout_ms  → 超时 (毫秒)
    //   FALSE       → 不要求 alertable wait
    BOOL result = GetQueuedCompletionStatusEx(
        m_iocp, entries, to_get, &count, static_cast<DWORD>(timeout_ms), FALSE);

    if (!result) {
        // 可能是超时 (count=0) 或真正的错误
        DWORD err = GetLastError();
        if (err == WAIT_TIMEOUT) {
            return 0;  // 超时, 无完成事件
        }
        if (err == ERROR_ABANDONED_WAIT_0) {
            // IOCP 已关闭 (shutdown 调用)
            return 0;
        }
        LOG_WARN("GetQueuedCompletionStatusEx error: {}", err);
        return 0;
    }

    // --- 处理完成事件 ---
    for (ULONG i = 0; i < count; ++i) {
        OVERLAPPED_ENTRY& entry = entries[i];

        // 检查是否是唤醒事件 (PostQueuedCompletionStatus 发送的)
        if (entry.lpOverlapped == nullptr) {
            // 唤醒事件 (shutdown 信号), 跳过
            contexts[i] = nullptr;
            continue;
        }

        // 从 OVERLAPPED* 恢复到 IOCPContext*
        IOCPContext* ctx = IOCPContext::from_overlapped(entry.lpOverlapped);

        // 填充完成结果
        ctx->bytes_transferred = entry.dwNumberOfBytesTransferred;
        ctx->error = (entry.lpOverlapped != nullptr) ? 0 : -1;

        contexts[i] = ctx;  // 返回给调用者
    }

    return static_cast<int>(count);
}

// ============================================================================
// post_wakeup() - 投递唤醒事件
// ============================================================================
// 向 IOCP 投递一个特殊的完成事件 (OVERLAPPED=NULL)
// 用于在 shutdown 时唤醒工作线程
void IOEngineIocp::post_wakeup() {
    if (m_iocp) {
        PostQueuedCompletionStatus(m_iocp, 0, 0, nullptr);
    }
}

// ============================================================================
// IOCPContext 池管理
// ============================================================================

IOCPContext* IOEngineIocp::acquire_context() {
    std::lock_guard<std::mutex> lock(m_pool_mutex);

    if (m_free_contexts.empty()) {
        // 池耗尽: 动态分配新的
        auto ctx = std::make_unique<IOCPContext>();
        IOCPContext* ptr = ctx.get();
        m_context_pool.push_back(std::move(ctx));
        LOG_DEBUG("IOCPContext pool expanded ({} total)", m_context_pool.size());
        return ptr;
    }

    IOCPContext* ctx = m_free_contexts.back();
    m_free_contexts.pop_back();
    ctx->reset();  // 清空旧数据
    return ctx;
}

void IOEngineIocp::release_context(IOCPContext* ctx) {
    std::lock_guard<std::mutex> lock(m_pool_mutex);

    // 清理接收缓冲区引用 (不释放 buffer — 由 BufferHandle 管理)
    ctx->buffer = nullptr;
    ctx->buffer_len = 0;
    ctx->bytes_transferred = 0;

    m_free_contexts.push_back(ctx);
}

// ============================================================================
// 工厂函数
// ============================================================================
std::unique_ptr<IOEngine> create_io_engine() {
#if CYRUS_PLATFORM_WINDOWS
    auto engine = std::make_unique<IOEngineIocp>();
    LOG_INFO("Creating IOEngine: IOCP (Windows)");
    return engine;
#elif CYRUS_PLATFORM_LINUX
    // Linux: 尝试 io_uring, 如果不可用则回退
    auto engine = std::make_unique<IOEngineUring>();
    LOG_INFO("Creating IOEngine: io_uring (Linux)");
    return engine;
#else
    LOG_ERROR("No IOEngine available for this platform!");
    return nullptr;
#endif
}

} // namespace gateway
} // namespace cyrus
