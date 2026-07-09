// ============================================================================
// sse_handler.cpp - SSE 流处理器实现
// ============================================================================

#include "cyrus/gateway/sse_handler.hpp"

namespace cyrus {
namespace gateway {

// ============================================================================
// feed() - 喂入数据
// ============================================================================
// 将新数据累积到内部缓冲区, 按行分割, 调用 process_line()
size_t SSEHandler::feed(const uint8_t* data, size_t len) {
    if (len == 0) return 0;

    // 追加到累积器
    accumulator_.append(reinterpret_cast<const char*>(data), len);

    size_t consumed = len;

    // 查找换行符并处理完整的行
    while (true) {
        auto lf_pos = accumulator_.find('\n');
        if (lf_pos == std::string::npos) {
            break;  // 没有完整行
        }

        // 提取行 (去除尾部 \r)
        std::string line = accumulator_.substr(0, lf_pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // 从累积器移除已处理的行
        accumulator_.erase(0, lf_pos + 1);

        // 处理此行
        process_line(line);
    }

    return consumed;
}

// ============================================================================
// process_line() - 处理一行 SSE 数据
// ============================================================================
// SSE 行格式:
//   field:value     (冒号前是字段名, 冒号后可能带空格)
//   :comment        (冒号开头是注释)
//   (空行)           → 事件结束, 触发回调
void SSEHandler::process_line(const std::string& line) {
    // 空行 → 事件边界 (dispatch)
    if (line.empty()) {
        dispatch_event();
        return;
    }

    // 注释行 (: 开头) → 忽略
    if (line[0] == ':') {
        return;
    }

    // 解析 field: value
    auto colon_pos = line.find(':');
    if (colon_pos == std::string::npos) {
        // 没有冒号 → 整个行视为字段名, 值为空
        // (不符合规范, 但容忍处理)
        return;
    }

    std::string field = line.substr(0, colon_pos);
    std::string value;

    // 跳过冒号后的一个空格 (SSE 规范)
    size_t value_start = colon_pos + 1;
    if (value_start < line.size() && line[value_start] == ' ') {
        value_start++;
    }
    value = line.substr(value_start);

    // --- 按字段名处理 ---
    if (field == "data") {
        // data 字段: 可以多行, 用 \n 连接
        if (!data_buffer_.empty()) {
            data_buffer_ += '\n';
        }
        data_buffer_ += value;
    } else if (field == "event") {
        current_event_.event = value;
    } else if (field == "id") {
        current_event_.id = value;
    } else if (field == "retry") {
        try {
            current_event_.retry = std::stoll(value);
        } catch (...) {
            current_event_.retry = 0;
        }
    }
    // 未知字段: 按照 SSE 规范忽略
}

// ============================================================================
// dispatch_event() - 分发构建完成的事件
// ============================================================================
void SSEHandler::dispatch_event() {
    // 设置 data 字段
    current_event_.data = data_buffer_;

    // 触发回调
    if (on_event && !current_event_.is_empty()) {
        on_event(current_event_);
    }

    // 重置状态 (准备下一个事件)
    current_event_ = SSEEvent{};
    data_buffer_.clear();
}

// ============================================================================
// reset() - 重置处理器
// ============================================================================
void SSEHandler::reset() {
    accumulator_.clear();
    data_buffer_.clear();
    current_event_ = SSEEvent{};
}

} // namespace gateway
} // namespace cyrus
