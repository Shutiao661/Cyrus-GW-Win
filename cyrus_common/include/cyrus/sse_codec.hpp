// ============================================================================
// sse_codec.hpp - Server-Sent Events (SSE) 格式工具
// ============================================================================
// SSE (Server-Sent Events) 是 W3C 标准的服务器推送协议。
// 用于从服务器向客户端单向流式推送事件, 如 LLM token 流。
//
// SSE 消息格式:
//   event: <事件类型>\n        ← 可选, 默认为 "message"
//   id: <事件ID>\n             ← 可选, 用于断线重连
//   data: <数据行>\n           ← 必需, 可以有多个 data: 行
//   data: <更多数据>\n
//   \n                         ← 空行表示消息结束
//
// 示例 (OpenAI 兼容的 chat completion 流):
//   data: {"choices":[{"delta":{"content":"Hello"}}]}\n\n
//   data: {"choices":[{"delta":{"content":" world"}}]}\n\n
//   data: [DONE]\n\n
//
// 使用方法:
//   SSEFormatter fmt;
//   std::string msg = fmt.data("Hello");          // "data: Hello\n\n"
//   std::string event = fmt.event("token", data); // "event: token\ndata: ...\n\n"
//   std::string heartbeat = fmt.comment("ping");  // ": ping\n"
// ============================================================================

#pragma once

#include <string>
#include <string_view>
#include <sstream>

namespace cyrus {

class SSEFormatter {
public:
    // --- 格式化单个 data 字段 ---
    // 生成: "data: <data>\n\n"
    // 注意: data 中的换行符会被展开为多行 "data: ..."
    static std::string data(std::string_view content) {
        std::ostringstream oss;
        write_data_field(oss, content);
        oss << "\n";   // 空行 = 消息结束
        return oss.str();
    }

    // --- 格式化带事件类型的 SSE 消息 ---
    // 生成: "event: <event_type>\ndata: <data>\n\n"
    static std::string event(std::string_view event_type, std::string_view content) {
        std::ostringstream oss;
        if (!event_type.empty()) {
            oss << "event: " << event_type << "\n";
        }
        write_data_field(oss, content);
        oss << "\n";
        return oss.str();
    }

    // --- 格式化 SSE 注释 (用于 keep-alive 心跳) ---
    // 生成: ": <comment>\n"
    // SSE 客户端会忽略注释行 (不触发 message 事件),
    // 但 TCP 连接上的数据流会重置代理/负载均衡器的空闲超时计时器。
    // 推荐每 15 秒发送一次 keep-alive。
    static std::string comment(std::string_view text) {
        std::ostringstream oss;
        oss << ": " << text << "\n\n";
        return oss.str();
    }

    // --- 格式化 OpenAI 兼容的完成块 ---
    // 生成: "data: <json>\n\n"
    static std::string completion_chunk(std::string_view json) {
        return data(json);
    }

    // --- 格式化流结束标记 (OpenAI 兼容) ---
    // 生成: "data: [DONE]\n\n"
    static std::string stream_done() {
        return data("[DONE]");
    }

private:
    // 写入 data: 字段, 处理多行数据
    // "line1\nline2" → "data: line1\ndata: line2\n"
    static void write_data_field(std::ostringstream& oss, std::string_view content) {
        if (content.empty()) {
            oss << "data: \n";
            return;
        }

        size_t start = 0;
        while (start < content.size()) {
            auto end = content.find('\n', start);
            if (end == std::string_view::npos) {
                // 最后一行
                oss << "data: " << content.substr(start) << "\n";
                break;
            } else {
                // 中间行
                oss << "data: " << content.substr(start, end - start) << "\n";
                start = end + 1;
            }
        }
    }
};

} // namespace cyrus
