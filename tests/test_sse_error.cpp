// ============================================================================
// test_sse_error.cpp - SSE 错误处理与超时单元测试
// ============================================================================
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

#include "../cyrus_common/include/cyrus/sse_codec.hpp"
#include "../cyrus_gateway/include/cyrus/gateway/sse_handler.hpp"

using namespace cyrus;
using namespace cyrus::gateway;

static int tests_passed = 0, tests_failed = 0;

#define TEST(name) printf("  TEST: %s ... ", name);
#define PASS() do { printf("PASSED\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAILED: %s\n", msg); tests_failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

// 1. SSEFormatter::data 格式正确
void test_sse_data_format() {
    TEST("SSEFormatter::data format");
    std::string result = SSEFormatter::data("Hello");
    CHECK(result == "data: Hello\n\n", "Basic data format");
    PASS();
}

// 2. SSEFormatter::event 格式正确
void test_sse_event_format() {
    TEST("SSEFormatter::event format");
    std::string result = SSEFormatter::event("token", "Hello");
    CHECK(result.find("event: token\n") != std::string::npos, "Should have event line");
    CHECK(result.find("data: Hello\n") != std::string::npos, "Should have data line");
    PASS();
}

// 3. SSEFormatter::stream_done 格式
void test_sse_stream_done() {
    TEST("SSEFormatter::stream_done format");
    std::string result = SSEFormatter::stream_done();
    CHECK(result == "data: [DONE]\n\n", "OpenAI-compatible done marker");
    PASS();
}

// 4. SSEFormatter::comment (心跳)
void test_sse_comment() {
    TEST("SSEFormatter::comment format");
    std::string result = SSEFormatter::comment("ping");
    CHECK(result == ": ping\n\n", "Comment format");
    PASS();
}

// 5. SSERelayHandler 初始状态
void test_relay_initial_state() {
    TEST("SSERelayHandler initial state");
    SSERelayHandler relay;
    CHECK(relay.state() == SSERelayState::INIT, "Should start in INIT");
    CHECK(relay.is_active() == false, "Should not be active yet");
    PASS();
}

// 6. SSERelayHandler build SSE header
void test_relay_build_header() {
    TEST("SSERelayHandler build SSE header");
    SSERelayHandler relay;
    std::string header = relay.build_sse_header();
    CHECK(header.find("HTTP/1.1 200 OK") != std::string::npos, "Should have status line");
    CHECK(header.find("Content-Type: text/event-stream") != std::string::npos, "Should have SSE content type");
    CHECK(header.find("Cache-Control: no-cache") != std::string::npos, "Should disable cache");
    CHECK(header.find("X-Accel-Buffering: no") != std::string::npos, "Should disable nginx buffering");
    CHECK(relay.state() == SSERelayState::WAITING_FIRST_BYTE, "Should be waiting for first byte");
    PASS();
}

// 7. SSERelayHandler 正常流式完成
void test_relay_streaming_complete() {
    TEST("SSERelayHandler streaming completion");
    SSERelayHandler relay;
    relay.build_sse_header();

    std::string received;
    bool closed = false;
    relay.on_send = [&](std::string_view data, bool is_last) {
        received.append(data);
        if (is_last) closed = true;
    };

    relay.on_agent_data("data: token1\n\n");
    relay.on_agent_data("data: token2\n\n");
    relay.on_stream_complete();

    CHECK(received.find("data: token1") != std::string::npos, "Should contain token1");
    CHECK(received.find("data: token2") != std::string::npos, "Should contain token2");
    CHECK(received.find("[DONE]") != std::string::npos, "Should contain done marker");
    CHECK(closed, "Should be marked as completed");
    CHECK(relay.state() == SSERelayState::DONE, "Should be in DONE state");
    PASS();
}

// 8. SSERelayHandler Agent 错误传播
void test_relay_agent_error() {
    TEST("SSERelayHandler error event propagation");
    SSERelayHandler relay;
    relay.build_sse_header();

    std::string received;
    relay.on_send = [&](std::string_view data, bool) {
        received.append(data);
    };

    relay.on_agent_error("LLM call failed", 502);

    CHECK(received.find("event: error") != std::string::npos, "Should contain event: error");
    CHECK(received.find("upstream_error") != std::string::npos, "Should contain error type");
    CHECK(received.find("LLM call failed") != std::string::npos, "Should contain error message");
    CHECK(received.find("502") != std::string::npos, "Should contain HTTP status code");
    CHECK(relay.state() == SSERelayState::ERROR_SENT, "Should be in ERROR_SENT state");
    PASS();
}

// 9. 首包超时检测
void test_relay_first_byte_timeout() {
    TEST("SSERelayHandler first byte timeout");
    SSERelayTimeout timeout;
    timeout.first_byte_timeout_ms = 50;  // 50ms 快速超时
    SSERelayHandler relay(timeout);
    relay.build_sse_header();

    std::string received;
    relay.on_send = [&](std::string_view data, bool) {
        received.append(data);
    };

    // 等待超时
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool active = relay.check_timeout();

    CHECK(!active, "Should not be active after timeout");
    CHECK(received.find("upstream_first_byte_timeout") != std::string::npos, "Should contain timeout type");
    CHECK(received.find("504") != std::string::npos, "Should have 504 status");
    PASS();
}

// 10. 重复错误不双重发送
void test_relay_no_double_error() {
    TEST("SSERelayHandler no double error");
    SSERelayHandler relay;
    relay.build_sse_header();

    int call_count = 0;
    relay.on_send = [&](std::string_view, bool) { call_count++; };

    relay.on_agent_error("first error", 502);
    relay.on_agent_error("second error", 502);  // 应被忽略
    relay.on_agent_error("third error", 503);   // 应被忽略

    CHECK(call_count == 1, "Should only send error once");
    PASS();
}

int main() {
    printf("=== SSE Error Handling Tests ===\n\n");
    test_sse_data_format();
    test_sse_event_format();
    test_sse_stream_done();
    test_sse_comment();
    test_relay_initial_state();
    test_relay_build_header();
    test_relay_streaming_complete();
    test_relay_agent_error();
    test_relay_first_byte_timeout();
    test_relay_no_double_error();
    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
