// ============================================================================
// bench_main.cpp - IOCP vs Select 基准测试 (Windows)
// ============================================================================
// 三种测试路径:
//   1. PURE_ERROR:  仅返回静态错误响应 (测纯 I/O 性能)
//   2. AGENT_SINGLE: 经过 Agent echo handler (测 IPC 开销)
//   3. FULL_CHAIN:   完整 Gateway→Agent→Mock LLM→SSE 流式 (测全链路)
//
// 并发参数: C = 100 / 300 / 500
// 指标: RPS, 超时率, 完成请求数
//
// 用法: bench_main.exe --clients=100 --path=pure_error --duration=30
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <mutex>
#include <algorithm>

// 独立声明 (bench_main 不链接 cyrus_common 中的 g_running)
static std::atomic<bool> g_running{false};

#include "../cyrus_common/include/cyrus/platform.hpp"

using namespace std::chrono;

// ============================================================================
// 配置
// ============================================================================
struct BenchConfig {
    int test_port       = 19999;
    int duration_sec    = 30;
    int clients         = 100;
    int msg_size        = 256;
    const char* path    = "pure_error";
};

static BenchConfig g_config;
static std::atomic<int64_t> g_total_requests{0};
static std::atomic<int64_t> g_total_errors{0};

// ============================================================================
// 简单 select() echo 服务器 (对照基线)
// ============================================================================
class SelectEchoServer {
public:
    bool start() {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_fd_ == INVALID_SOCKET) return false;

        int reuse = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        u_long mode = 1;
        ioctlsocket(listen_fd_, FIONBIO, &mode);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(g_config.test_port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return false;
        if (listen(listen_fd_, SOMAXCONN) == SOCKET_ERROR) return false;

        thread_ = std::thread(&SelectEchoServer::run, this);
        return true;
    }

    void stop() {
        g_running.store(false);
        if (thread_.joinable()) {
            // 等待线程退出 (最多 2 秒)
            auto start = steady_clock::now();
            while (thread_.joinable() &&
                   duration_cast<seconds>(steady_clock::now() - start).count() < 2) {
                std::this_thread::sleep_for(milliseconds(100));
            }
            if (thread_.joinable()) {
                thread_.detach();  // 强制放弃
            }
        }
        if (listen_fd_ != INVALID_SOCKET) {
            closesocket(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
        }
    }

private:
    void run() {
        while (g_running.load()) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listen_fd_, &read_fds);
            int max_fd = static_cast<int>(listen_fd_);

            for (SOCKET c : clients_) {
                FD_SET(c, &read_fds);
                if (static_cast<int>(c) > max_fd) max_fd = static_cast<int>(c);
            }

            timeval timeout{0, 100000};  // 100ms
            int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &timeout);
            if (ready <= 0) continue;

            // Accept
            if (FD_ISSET(listen_fd_, &read_fds)) {
                SOCKET client = accept(listen_fd_, nullptr, nullptr);
                if (client != INVALID_SOCKET) {
                    u_long mode = 1;
                    ioctlsocket(client, FIONBIO, &mode);
                    clients_.push_back(client);
                }
            }

            // Echo
            char buf[4096];
            for (size_t i = 0; i < clients_.size(); ) {
                if (FD_ISSET(clients_[i], &read_fds)) {
                    int n = recv(clients_[i], buf, sizeof(buf), 0);
                    if (n > 0) {
                        send(clients_[i], buf, n, 0);
                        i++;
                    } else {
                        closesocket(clients_[i]);
                        clients_.erase(clients_.begin() + i);
                    }
                } else {
                    i++;
                }
            }
        }

        for (SOCKET c : clients_) closesocket(c);
        clients_.clear();
    }

    SOCKET listen_fd_ = INVALID_SOCKET;
    std::vector<SOCKET> clients_;
    std::thread thread_;
};

// ============================================================================
// 客户端 worker
// ============================================================================
void client_worker(int id) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_config.test_port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return;
    }

    // 设置 recv 超时 5 秒
    int timeout = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    char send_buf[256];
    char recv_buf[4096];
    memset(send_buf, 'A', g_config.msg_size);

    while (g_running.load()) {
        int sent = send(sock, send_buf, g_config.msg_size, 0);
        if (sent <= 0) { g_total_errors++; break; }

        int total_recv = 0;
        while (total_recv < g_config.msg_size) {
            int recvd = recv(sock, recv_buf + total_recv,
                             sizeof(recv_buf) - total_recv, 0);
            if (recvd <= 0) {
                g_total_errors++;
                goto done;
            }
            total_recv += recvd;
        }
        g_total_requests++;
    }

done:
    closesocket(sock);
}

// ============================================================================
// 统计输出
// ============================================================================
void print_stats(double elapsed_sec, const char* engine_name) {
    double rps = g_total_requests.load() / elapsed_sec;
    int64_t total = g_total_requests.load();
    int64_t errors = g_total_errors.load();

    printf("\n--- %s ---\n", engine_name);
    printf("  Requests:  %lld\n", (long long)total);
    printf("  Errors:    %lld (%.2f%%)\n",
           (long long)errors, total > 0 ? 100.0 * errors / total : 0);
    printf("  Throughput: %.1f req/s\n", rps);
    printf("  Success:   %lld (%.2f%%)\n",
           (long long)(total - errors),
           total > 0 ? 100.0 * (total - errors) / total : 0);
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char* argv[]) {
    // 解析参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.find("--clients=") == 0) {
            g_config.clients = std::stoi(arg.substr(10));
        } else if (arg.find("--duration=") == 0) {
            g_config.duration_sec = std::stoi(arg.substr(11));
        } else if (arg.find("--path=") == 0) {
            g_config.path = argv[i] + 7;  // 简化: 直接用指针
        } else if (arg.find("--port=") == 0) {
            g_config.test_port = std::stoi(arg.substr(7));
        }
    }

    printf("========================================\n");
    printf("  IOCP vs Select Benchmark (Windows)\n");
    printf("  Clients: %d | Duration: %d sec | Path: %s\n",
           g_config.clients, g_config.duration_sec, g_config.path);
    printf("========================================\n");

    // 初始化 WSA
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // --- select() 服务器测试 ---
    printf("\n[1/2] Testing select() echo server...\n");

    g_running.store(true);
    g_total_requests.store(0);
    g_total_errors.store(0);

    SelectEchoServer sel_server;
    if (!sel_server.start()) {
        fprintf(stderr, "Failed to start select server\n");
        WSACleanup();
        return 1;
    }

    std::this_thread::sleep_for(milliseconds(500));

    std::vector<std::thread> client_threads;
    for (int i = 0; i < g_config.clients; ++i) {
        client_threads.emplace_back(client_worker, i);
    }

    auto bench_start = steady_clock::now();
    std::this_thread::sleep_for(seconds(g_config.duration_sec));
    g_running.store(false);
    auto bench_end = steady_clock::now();

    double elapsed = duration_cast<milliseconds>(bench_end - bench_start).count() / 1000.0;

    for (auto& t : client_threads) t.join();
    sel_server.stop();

    int64_t select_total = g_total_requests.load();
    double select_rps = select_total / elapsed;
    print_stats(elapsed, "select() Server");

    // --- IOCP 服务器测试 ---
    // 注意: IOCP 测试需要完整的 Gateway 进程
    // 这里简化为输出建议命令
    printf("\n[2/2] IOCP Gateway benchmark:\n");
    printf("  Start in separate terminals:\n");
    printf("    Terminal 1: .\\cyrus_agent.exe --port 9999\n");
    printf("    Terminal 2: .\\cyrus_gateway.exe config\\gateway.conf\n");
    printf("  Then run:\n");
    printf("    .\\benchmark\\run_bench.ps1 -Clients %d -Duration %d\n\n",
           g_config.clients, g_config.duration_sec);

    // 输出对比
    printf("========================================\n");
    printf("  Results Summary\n");
    printf("========================================\n");
    printf("  select():  %lld requests, %.1f req/s\n",
           (long long)select_total, select_rps);
    printf("  IOCP:      Run run_bench.ps1 for results\n");
    printf("========================================\n");

    WSACleanup();
    return 0;
}
