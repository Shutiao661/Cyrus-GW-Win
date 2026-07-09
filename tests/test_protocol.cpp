// ============================================================================
// test_protocol.cpp - 二进制协议编解码器单元测试
// ============================================================================
#include <cstdio>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

#include "../cyrus_common/include/cyrus/protocol.hpp"
#include "../cyrus_common/include/cyrus/types.hpp"

using namespace cyrus;

static int tests_passed = 0, tests_failed = 0;

#define TEST(name) printf("  TEST: %s ... ", name);
#define PASS() do { printf("PASSED\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAILED: %s\n", msg); tests_failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

// 1. 编码请求往返测试
void test_encode_decode_request() {
    TEST("Encode/Decode MSG_REQUEST round-trip");

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Host", "localhost"},
        {"Content-Type", "application/json"}
    };
    std::string body = R"({"message":"hello"})";

    auto frame = ProtocolCodec::encode_request(42, "POST", "/api/chat", headers, body);
    CHECK(!frame.empty(), "Frame should not be empty");

    // 验证长度前缀
    uint32_t payload_len;
    std::memcpy(&payload_len, frame.data(), sizeof(uint32_t));
    payload_len = ntohl(payload_len);
    CHECK(payload_len == frame.size() - sizeof(uint32_t), "Payload length should match");

    // 解码测试
    ProtocolDecoder decoder;
    bool frame_received = false;
    decoder.on_frame = [&](const MessageHeader& h, const uint8_t* p, size_t len) {
        CHECK(h.request_id == 42, "Request ID should be 42");
        CHECK(h.msg_type == MessageType::MSG_REQUEST, "Type should be MSG_REQUEST");
        frame_received = true;
    };
    decoder.feed(frame.data(), frame.size());
    CHECK(frame_received, "Should have decoded a frame");

    PASS();
}

// 2. 编码响应头测试
void test_encode_response_headers() {
    TEST("Encode MSG_RESPONSE_HEADERS");

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "text/event-stream"}
    };
    auto frame = ProtocolCodec::encode_response_headers(42, 200, headers);
    CHECK(!frame.empty(), "Frame should not be empty");

    uint32_t payload_len;
    std::memcpy(&payload_len, frame.data(), sizeof(uint32_t));
    payload_len = ntohl(payload_len);
    CHECK(payload_len > 0, "Payload should have data");

    PASS();
}

// 3. 编码错误帧测试
void test_encode_error() {
    TEST("Encode MSG_ERROR");

    auto frame = ProtocolCodec::encode_error(42, ErrorCode::E_AGENT_UNREACHABLE, "Agent down");
    CHECK(!frame.empty(), "Frame should not be empty");

    uint32_t payload_len;
    std::memcpy(&payload_len, frame.data(), sizeof(uint32_t));
    payload_len = ntohl(payload_len);

    // 检查 flags
    MessageHeader header;
    std::memcpy(&header, frame.data() + sizeof(uint32_t), sizeof(MessageHeader));
    CHECK(header.flags & FrameFlags::FLAG_ERROR, "Should have FLAG_ERROR flag");
    CHECK(header.msg_type == MessageType::MSG_ERROR, "Should be MSG_ERROR");

    PASS();
}

// 4. 解码器: 部分帧测试
void test_decoder_partial_frames() {
    TEST("Decoder: partial frames");

    ProtocolDecoder decoder;
    int frame_count = 0;
    decoder.on_frame = [&](const MessageHeader&, const uint8_t*, size_t) {
        frame_count++;
    };

    auto frame = ProtocolCodec::encode_response_end(42);

    // 只喂入一半
    decoder.feed(frame.data(), frame.size() / 2);
    CHECK(frame_count == 0, "No complete frame yet");

    // 喂入另一半
    decoder.feed(frame.data() + frame.size() / 2, frame.size() - frame.size() / 2);
    CHECK(frame_count == 1, "Should have 1 complete frame");

    PASS();
}

// 5. 解码器: 多帧粘包测试
void test_decoder_multiple_frames() {
    TEST("Decoder: multiple frames in one feed");

    auto frame1 = ProtocolCodec::encode_response_end(1);
    auto frame2 = ProtocolCodec::encode_response_end(2);

    // 拼接两帧
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), frame1.begin(), frame1.end());
    combined.insert(combined.end(), frame2.begin(), frame2.end());

    ProtocolDecoder decoder;
    int frame_count = 0;
    decoder.on_frame = [&](const MessageHeader&, const uint8_t*, size_t) {
        frame_count++;
    };
    decoder.feed(combined.data(), combined.size());
    CHECK(frame_count == 2, "Should decode 2 frames");

    PASS();
}

// 6. 解码器 reset 测试
void test_decoder_reset() {
    TEST("Decoder reset");

    ProtocolDecoder decoder;
    int frame_count = 0;
    decoder.on_frame = [&](const MessageHeader&, const uint8_t*, size_t) {
        frame_count++;
    };

    // 喂入半帧
    auto frame = ProtocolCodec::encode_response_end(42);
    decoder.feed(frame.data(), frame.size() / 2);
    CHECK(frame_count == 0, "No frame yet");

    // 重置后应丢弃累积数据
    decoder.reset();
    CHECK(decoder.buffered_bytes() == 0, "Buffer should be empty after reset");

    PASS();
}

int main() {
    printf("========================================\n");
    printf("  Protocol Codec Unit Tests\n");
    printf("========================================\n\n");

    test_encode_decode_request();
    test_encode_response_headers();
    test_encode_error();
    test_decoder_partial_frames();
    test_decoder_multiple_frames();
    test_decoder_reset();

    printf("\n========================================\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
