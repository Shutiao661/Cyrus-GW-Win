# Cyrus-GW — High-Performance Async HTTP Gateway

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Windows](https://img.shields.io/badge/Platform-Windows-blue.svg)](https://www.microsoft.com/windows)
[![IOCP](https://img.shields.io/badge/Engine-IOCP-green.svg)](https://learn.microsoft.com/windows/win32/fileio/i-o-completion-ports)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Cyrus-GW** 是一个用 C++20 编写的高性能异步 HTTP 网关，支持 Server-Sent Events (SSE) 流式响应。

### 架构

```
                      ┌─────────────────────┐
   HTTP Clients ─────→│   Cyrus-GW Gateway  │
                      │   (IOCP / io_uring) │
                      └──────┬──────────────┘
                             │ TCP (Binary Protocol)
                      ┌──────▼──────────────┐
                      │   Cyrus Agent       │
                      │   (Business Logic)  │
                      └─────────────────────┘
```

### 特性

- **高性能异步 I/O**: Windows IOCP / Linux io_uring
- **HTTP/1.1 解析**: 逐字节状态机，支持 keep-alive、chunked encoding
- **SSE 流式推送**: OpenAI 兼容的聊天补全流
- **二进制协议**: Gateway–Agent 间的高效通信协议
- **缓冲池**: 预分配内存，零 malloc 开销
- **C++20**: 使用 `std::format`、`std::source_location` 等现代特性

---

## 构建 (Build)

### 前置要求

- **Visual Studio 2022** (v17.0+) with C++20 support
- **CMake 3.21+** (推荐 3.25+)
- **Windows 10/11** x64

### 步骤

```bash
# 1. 克隆项目
cd E:\Demo\Cyrus-GW-Win

# 2. 打开 Visual Studio 2022
#    File → Open → Folder → 选择此目录

# 3. VS 自动检测 CMakeLists.txt
#    在 Solution Explorer 的 "Switch between solutions" 下拉中选择:
#    "CMake Targets View"

# 4. 选择配置: x64-Debug
#    Build → Build All (Ctrl+Shift+B)

# 5. 或者使用命令行:
cmake --preset vs2022-debug-x64
cmake --build out/build/x64-Debug
```

### 目标

| 目标 | 类型 | 说明 |
|------|------|------|
| `cyrus_common` | 静态库 | 基础类型、日志、配置、缓冲池、协议 |
| `cyrus_gateway` | 可执行文件 | HTTP 网关 (IOCP 引擎) |
| `cyrus_agent` | 可执行文件 | 后端 Agent 服务 |
| `test_http_parser` | 测试 | HTTP 解析器测试 (9 cases) |
| `test_protocol` | 测试 | 协议编解码测试 (6 cases) |
| `test_sse` | 测试 | SSE 编解码测试 (6 cases) |
| `test_buffer` | 测试 | 缓冲池测试 (5 cases) |
| `cyrus_benchmark` | 基准测试 | IOCP vs Select 吞吐量对比 |

---

## 运行 (Run)

### 1. 启动 Agent

```powershell
# 终端 1: 启动 Agent
.\out\build\x64-Debug\cyrus_agent\cyrus_agent.exe --port 9999

# 输出:
# ============================================
#   Cyrus Agent v1.0.0
# ============================================
# Agent server listening on 127.0.0.1:9999
# Agent ready. Port: 9999, Handlers: echo, chat
```

### 2. 启动 Gateway

```powershell
# 终端 2: 启动 Gateway
.\out\build\x64-Debug\cyrus_gateway\cyrus_gateway.exe config\gateway.conf

# 输出:
# ============================================
#   Cyrus-GW Gateway v1.0.0
# ============================================
# IOEngineIocp initialized successfully
# Cyrus-GW Gateway listening on 0.0.0.0:8080
```

### 3. 测试

```powershell
# 终端 3: 发送请求

# 健康检查
curl http://localhost:8080/health
# → {"status":"ok","version":"1.0.0","platform":"Windows"}

# 欢迎页面
curl http://localhost:8080/
# → <!DOCTYPE html>...

# 聊天补全 (SSE 流式)
curl -N -X POST http://localhost:8080/v1/chat/completions `
  -H "Content-Type: application/json" `
  -d '{"model":"cyrus","messages":[{"role":"user","content":"Hello"}]}'
# → SSE 事件流...
```

---

## 运行测试

```bash
# 使用 CTest
ctest --test-dir out/build/x64-Debug --output-on-failure

# 或直接在 VS 中:
# Test → Test Explorer → Run All
```

---

## 项目结构

```
Cyrus-GW-Win/
├── CMakeLists.txt              # 根 CMake
├── CMakePresets.json           # VS2022 预设
├── README.md
├── config/gateway.conf         # 网关配置
├── cmake/CompilerWarnings.cmake
├── cyrus_common/               # 静态库 (types/log/config/buffer/protocol)
├── cyrus_gateway/              # Gateway 可执行文件 (IOCP engine + HTTP parser)
├── cyrus_agent/                # Agent 可执行文件 (echo + chat handlers)
├── tests/                      # 单元测试 (4 test exes)
└── benchmark/                  # 基准测试
```

---

## 配置

编辑 `config/gateway.conf`:

```ini
[server]
listen_address = 0.0.0.0
listen_port = 8080
worker_threads = 0          # 0 = auto (CPU core count)

[agent]
host = 127.0.0.1
port = 9999
pool_size = 4

[logging]
level = DEBUG               # DEBUG | INFO | WARN | ERROR
```

---

## 技术栈

| 技术 | 用途 |
|------|------|
| C++20 | 核心语言 (`std::format`, `std::source_location`) |
| Windows IOCP | 异步 I/O 引擎 (AcceptEx, WSARecv, WSASend) |
| CMake | 跨平台构建系统 |
| SSE | 流式推送协议 |
| TCP | Gateway ↔ Agent 通信 |

---

## License

MIT License. See [LICENSE](LICENSE) for details.
