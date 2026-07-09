// ============================================================================
// io_engine_uring.cpp - Linux io_uring I/O 引擎实现
// ============================================================================
// 仅在 __linux__ 平台提供完整实现。
// 在 Windows 上, 方法返回错误 (存根), 实际使用 IOEngineIocp。
//
// io_uring 操作流程:
//   1. 获取 SQE: io_uring_get_sqe(&ring_)
//   2. 准备操作: io_uring_prep_xxx(sqe, fd, ...)
//   3. 设置 user_data: sqe->user_data = encode(op, ctx)
//   4. 提交: io_uring_submit(&ring_)
//   5. 等待完成: io_uring_wait_cqe(&ring_, &cqe) / io_uring_peek_batch_cqe
//   6. 处理结果: 从 cqe->user_data 恢复 ctx, 设置 bytes/error
// ============================================================================

#include "cyrus/gateway/io_engine_uring.hpp"

#ifdef __linux__
#include <liburing.h>
#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>
#endif

namespace cyrus {
namespace gateway {

// ============================================================================
// 析构
// ============================================================================
IOEngineUring::~IOEngineUring() {
    shutdown();
}

// ============================================================================
// init() - 初始化 io_uring
// ============================================================================
bool IOEngineUring::init() {
#ifdef __linux__
    // io_uring_queue_init 参数:
    //   entries: SQ 环大小 (必须是 2 的幂)
    //   ring: 输出, io_uring 实例
    //   flags: 0 = 默认, IORING_SETUP_SQPOLL = 内核轮询 SQ
    constexpr int SQ_ENTRIES = 256;
    int ret = io_uring_queue_init(SQ_ENTRIES, &ring_, 0);
    if (ret < 0) {
        LOG_ERROR("io_uring_queue_init failed: {} ({})", -ret, strerror(-ret));
        return false;
    }

    initialized_ = true;
    LOG_INFO("IOEngineUring initialized ({} SQ entries, flags=0)", SQ_ENTRIES);
    return true;
#else
    LOG_ERROR("IOEngineUring is Linux-only. Use IOEngineIocp on Windows.");
    return false;
#endif
}

// ============================================================================
// shutdown() - 关闭 io_uring
// ============================================================================
void IOEngineUring::shutdown() {
#ifdef __linux__
    if (!initialized_) return;

    shutting_down_.store(true, std::memory_order_release);

    // 投递多个 nop 操作以唤醒阻塞在 wait_completions 上的线程
    for (int i = 0; i < 64; ++i) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (sqe) {
            io_uring_prep_nop(sqe);
            sqe->user_data = 0;  // 标记为唤醒事件
            io_uring_submit(&ring_);
        }
    }

    io_uring_queue_exit(&ring_);
    initialized_ = false;
    LOG_INFO("IOEngineUring shutdown complete");
#else
    (void)shutting_down_;
#endif
}

// ============================================================================
// register_socket() - 注册 fd 到 io_uring
// ============================================================================
bool IOEngineUring::register_socket(socket_t fd, void* user_data) {
#ifdef __linux__
    // io_uring 通过 SQE 的 fd 字段直接引用 socket, 无需显式注册
    // user_data 由调用者通过 IOContext::user_data 管理
    (void)fd;
    (void)user_data;
    return true;
#else
    (void)fd;
    (void)user_data;
    LOG_ERROR("IOEngineUring: register_socket not available on this platform");
    return false;
#endif
}

// ============================================================================
// post_accept() - 投递异步 Accept
// ============================================================================
int IOEngineUring::post_accept(socket_t listen_fd, IOContext* base_ctx) {
#ifdef __linux__
    UringContext* ctx = static_cast<UringContext*>(base_ctx);

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        LOG_ERROR("io_uring_get_sqe failed (SQ full)");
        return -1;
    }

    // 准备 accept 操作
    sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);

    io_uring_prep_accept(sqe, static_cast<int>(listen_fd),
                         reinterpret_cast<sockaddr*>(&client_addr),
                         &addr_len, 0);

    ctx->op = IOOperation::ACCEPT;
    ctx->fd = listen_fd;
    sqe->user_data = UringContext::encode(IOOperation::ACCEPT, ctx);

    // 提交到内核
    io_uring_submit(&ring_);
    return 0;
#else
    (void)listen_fd;
    (void)base_ctx;
    return -1;
#endif
}

// ============================================================================
// post_recv() - 投递异步 Recv
// ============================================================================
int IOEngineUring::post_recv(socket_t fd, IOContext* base_ctx) {
#ifdef __linux__
    UringContext* ctx = static_cast<UringContext*>(base_ctx);

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        LOG_ERROR("io_uring_get_sqe failed (SQ full)");
        return -1;
    }

    ctx->op = IOOperation::RECV;
    ctx->fd = fd;
    ctx->bytes_transferred = 0;
    ctx->error = 0;

    io_uring_prep_recv(sqe, static_cast<int>(fd),
                       ctx->buffer, ctx->buffer_len, 0);
    sqe->user_data = UringContext::encode(IOOperation::RECV, ctx);

    io_uring_submit(&ring_);
    return 0;
#else
    (void)fd;
    (void)base_ctx;
    return -1;
#endif
}

// ============================================================================
// post_send() - 投递异步 Send
// ============================================================================
int IOEngineUring::post_send(socket_t fd, IOContext* base_ctx) {
#ifdef __linux__
    UringContext* ctx = static_cast<UringContext*>(base_ctx);

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        LOG_ERROR("io_uring_get_sqe failed (SQ full)");
        return -1;
    }

    ctx->op = IOOperation::SEND;
    ctx->fd = fd;

    // bytes_transferred 在此处承载待发送长度
    io_uring_prep_send(sqe, static_cast<int>(fd),
                       ctx->buffer, ctx->bytes_transferred, 0);
    sqe->user_data = UringContext::encode(IOOperation::SEND, ctx);

    io_uring_submit(&ring_);
    return 0;
#else
    (void)fd;
    (void)base_ctx;
    return -1;
#endif
}

// ============================================================================
// wait_completions() - 等待完成事件 (批量收割 CQE)
// ============================================================================
int IOEngineUring::wait_completions(IOContext** contexts, int max_events,
                                     int timeout_ms) {
#ifdef __linux__
    if (shutting_down_.load(std::memory_order_acquire)) {
        return 0;
    }

    // 批量收割 CQE
    struct io_uring_cqe* cqes[128];
    int to_get = (max_events < 128) ? max_events : 128;
    int ret;

    if (timeout_ms < 0) {
        // 无限等待
        ret = io_uring_wait_cqe(&ring_, &cqes[0]);
        if (ret == 0) {
            to_get = 1;
        } else {
            return 0;
        }
    } else if (timeout_ms == 0) {
        // 非阻塞 peek
        ret = io_uring_peek_batch_cqe(&ring_, cqes, static_cast<unsigned>(to_get));
        if (ret < 0) return 0;
        to_get = ret;
    } else {
        // 带超时: 先尝试 peek, 如果为空则 wait
        ret = io_uring_peek_batch_cqe(&ring_, cqes, static_cast<unsigned>(to_get));
        if (ret > 0) {
            to_get = ret;
        } else {
            // 提交超时操作
            struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (sqe) {
                struct __kernel_timespec ts;
                ts.tv_sec = timeout_ms / 1000;
                ts.tv_nsec = (timeout_ms % 1000) * 1000000;
                io_uring_prep_timeout(sqe, &ts, 0, 0);
                sqe->user_data = 0;
                io_uring_submit(&ring_);
            }

            // 等待单个 CQE (可能是超时事件或 I/O 完成)
            ret = io_uring_wait_cqe(&ring_, &cqes[0]);
            if (ret < 0) return 0;

            // 检查是否是超时事件
            if (cqes[0]->user_data == 0) {
                io_uring_cqe_seen(&ring_, cqes[0]);
                return 0;  // 超时
            }
            to_get = 1;
        }
    }

    // 处理完成事件
    int count = 0;
    for (int i = 0; i < to_get && i < 128; ++i) {
        struct io_uring_cqe* cqe = cqes[i];

        // 跳过唤醒/超时事件
        if (cqe->user_data == 0) {
            io_uring_cqe_seen(&ring_, cqe);
            continue;
        }

        UringContext* ctx = UringContext::decode_ctx(cqe->user_data);

        // 填充结果
        if (cqe->res >= 0) {
            ctx->bytes_transferred = static_cast<size_t>(cqe->res);
            ctx->error = 0;
        } else {
            ctx->bytes_transferred = 0;
            ctx->error = -cqe->res;  // errno
        }

        contexts[count++] = ctx;

        // 标记 CQE 已处理 (归还到 CQ 环)
        io_uring_cqe_seen(&ring_, cqe);
    }

    return count;
#else
    (void)contexts;
    (void)max_events;
    (void)timeout_ms;
    return 0;
#endif
}

// ============================================================================
// post_wakeup() - 唤醒阻塞线程
// ============================================================================
void IOEngineUring::post_wakeup() {
#ifdef __linux__
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe) {
        io_uring_prep_nop(sqe);
        sqe->user_data = 0;  // 标记为唤醒
        io_uring_submit(&ring_);
    }
#endif
}

} // namespace gateway
} // namespace cyrus
