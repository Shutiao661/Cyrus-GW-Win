// ============================================================================
// platform_win32.cpp - Windows 平台特定实现
// ============================================================================
// 实现 platform.hpp 中声明的跨平台函数在 Windows 上的版本:
//   1. register_signal_handler(): 使用 SetConsoleCtrlHandler 捕获 Ctrl+C
//   2. g_running: 全局原子标志, 信号处理器将其设为 false 通知主循环退出
// ============================================================================

#include "cyrus/platform.hpp"
#include "cyrus/types.hpp"

namespace cyrus {

// ============================================================================
// 全局运行标志
// ============================================================================
// 所有主事件循环都检查此标志, 当为 false 时退出
// 信号处理器负责将其设置为 false
// atomic 保证信号处理器和主线程之间的可见性
std::atomic<bool> g_running{true};

// ============================================================================
// 信号处理器 (Windows Console Control Handler)
// ============================================================================

// 存储用户注册的回调函数 (全局静态变量)
static SignalCallback g_signal_callback = nullptr;

// ---------------------------------------------------------------------------
// console_ctrl_handler - Windows 控制台事件处理函数
// 当用户按下 Ctrl+C、关闭控制台窗口、系统关机时, Windows 调用此函数。
// 返回 TRUE 表示已处理事件 (阻止默认行为, 如立即终止进程)。
// ---------------------------------------------------------------------------
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:          // 用户按下 Ctrl+C
        case CTRL_CLOSE_EVENT:      // 用户关闭控制台窗口
        case CTRL_SHUTDOWN_EVENT:   // 系统关机
        case CTRL_LOGOFF_EVENT:     // 用户注销
            // 设置运行标志为 false, 触发所有主循环退出
            g_running.store(false, std::memory_order_release);

            // 如果用户注册了回调, 调用它
            if (g_signal_callback) {
                g_signal_callback();
            }
            return TRUE;  // 已处理, 阻止默认行为
        default:
            return FALSE; // 未处理
    }
}

// ---------------------------------------------------------------------------
// register_signal_handler - 注册信号处理器
// 在 Windows 上使用 SetConsoleCtrlHandler
// callback: 收到信号后调用的回调 (在 g_running 设置为 false 之后)
// ---------------------------------------------------------------------------
void register_signal_handler(SignalCallback callback) {
    g_signal_callback = std::move(callback);

#if CYRUS_PLATFORM_WINDOWS
    // 注册控制台事件处理器
    // 如果注册失败 (极端情况), 忽略 — 用户仍可通过关闭窗口退出
    ::SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    // Linux: 使用 sigaction 注册 SIGINT 和 SIGTERM 处理器
    // (此部分在 .cpp 中实现, 如果将来需要 Linux 支持)
#endif
}

} // namespace cyrus
