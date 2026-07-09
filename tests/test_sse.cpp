// ============================================================================
// test_sse.cpp - SSE 编解码器单元测试
// ============================================================================
#include <cstdio>
#include <cstring>
#include <string>

#include "../cyrus_common/include/cyrus/sse_codec.hpp"
#include "../cyrus_gateway/include/cyrus/gateway/sse_handler.hpp"

using namespace cyrus;
using namespace cyrus::gateway;

static int tests_passed = 0, tests_failed = 0;

#define TEST(name) printf("  TEST: %s ... ", name);
#define PASS() do { printf("PASSED\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAILED: %s\n", msg); tests_failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

// 1. SSE 数据格式化
void test_sse_data() {
    TEST("SSE data formatting");
    std::string result = SSEFormatter::data("hello");
    CHECK(result == "data: hello\n\n", "Simple data event");
    PASS();
}

// 2. SSE 事件类型
void test_sse_event() {
    TEST("SSE event type");
    std::string result = SSEFormatter::event("update", "world");
    CHECK(result == "event: update\ndata: world\n\n", "Event with type");
    PASS();
}

// 3. SSE 注释 (keep-alive)
void test_sse_comment() {
    TEST("SSE comment (keep-alive)");
    std::string result = SSEFormatter::comment("ping");
    CHECK(result == ": ping\n\n", "Comment format");
    PASS();
}

// 4. SSE 流结束
void test_sse_done() {
    TEST("SSE stream done");
    std::string result = SSEFormatter::stream_done();
    CHECK(result == "data: [DONE]\n\n", "Stream done marker");
    PASS();
}

// 5. SSEHandler 解析
void test_sse_handler_parse() {
    TEST("SSEHandler parse");

    SSEHandler handler;
    int event_count = 0;
    handler.on_event = [&](const SSEEvent& ev) {
        event_count++;
    };

    const char* sse_data = "data: hello\n\n";
    handler.feed(reinterpret_cast<const uint8_t*>(sse_data), strlen(sse_data));
    CHECK(event_count == 1, "Should parse 1 event");

    PASS();
}

// 6. SSEHandler 多行 data
void test_sse_handler_multiline() {
    TEST("SSEHandler multiline data");

    SSEHandler handler;
    std::string event_data;
    handler.on_event = [&](const SSEEvent& ev) {
        event_data = ev.data;
    };

    const char* sse_data = "data: line1\ndata: line2\n\n";
    handler.feed(reinterpret_cast<const uint8_t*>(sse_data), strlen(sse_data));
    CHECK(event_data == "line1\nline2", "Multiline data should join with newline");

    PASS();
}

int main() {
    printf("========================================\n");
    printf("  SSE Codec Unit Tests\n");
    printf("========================================\n\n");

    test_sse_data();
    test_sse_event();
    test_sse_comment();
    test_sse_done();
    test_sse_handler_parse();
    test_sse_handler_multiline();

    printf("\n========================================\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
