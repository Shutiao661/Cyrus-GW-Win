// ============================================================================
// test_chunked_decoder.cpp - ChunkedDecoder 单元测试
// ============================================================================
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../cyrus_common/include/cyrus/chunked_decoder.hpp"

using namespace cyrus;

static int tests_passed = 0, tests_failed = 0;

#define TEST(name) printf("  TEST: %s ... ", name);
#define PASS() do { printf("PASSED\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAILED: %s\n", msg); tests_failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

// 1. 单个 chunk
void test_single_chunk() {
    TEST("Single chunk decode");
    ChunkedDecoder dec;
    std::string output;
    dec.on_data = [&](const uint8_t* data, size_t len) {
        output.append(reinterpret_cast<const char*>(data), len);
    };

    // "5\r\nHello\r\n0\r\n\r\n"
    const char* input = "5\r\nHello\r\n0\r\n\r\n";
    dec.feed(reinterpret_cast<const uint8_t*>(input), strlen(input));
    CHECK(dec.is_done(), "Should be done");
    CHECK(output == "Hello", "Should decode 'Hello'");
    PASS();
}

// 2. 多个 chunk
void test_multiple_chunks() {
    TEST("Multiple chunks decode");
    ChunkedDecoder dec;
    std::string output;
    dec.on_data = [&](const uint8_t* data, size_t len) {
        output.append(reinterpret_cast<const char*>(data), len);
    };

    // "3\r\nABC\r\n4\r\nDEFG\r\n0\r\n\r\n"
    const char* input = "3\r\nABC\r\n4\r\nDEFG\r\n0\r\n\r\n";
    dec.feed(reinterpret_cast<const uint8_t*>(input), strlen(input));
    CHECK(dec.is_done(), "Should be done");
    CHECK(output == "ABCDEFG", "Should decode 'ABCDEFG'");
    PASS();
}

// 3. 分片到达 (半包)
void test_fragmented_arrival() {
    TEST("Fragmented data arrival");
    ChunkedDecoder dec;
    std::string output;
    dec.on_data = [&](const uint8_t* data, size_t len) {
        output.append(reinterpret_cast<const char*>(data), len);
    };

    // 分三次喂入: "5\r" → "\nHe" → "llo\r\n0\r\n\r\n"
    dec.feed(reinterpret_cast<const uint8_t*>("5\r"), 2);
    CHECK(!dec.is_done(), "Should not be done yet");
    CHECK(dec.state() == ChunkedDecoder::State::READ_SIZE, "Should be in READ_SIZE");

    dec.feed(reinterpret_cast<const uint8_t*>("\nHe"), 3);
    CHECK(output == "He", "Should decode partial chunk data");

    dec.feed(reinterpret_cast<const uint8_t*>("llo\r\n0\r\n\r\n"), 9);
    CHECK(dec.is_done(), "Should be done");
    CHECK(output == "Hello", "Should decode 'Hello'");
    PASS();
}

// 4. 空 chunk 流
void test_empty_stream() {
    TEST("Empty chunked stream");
    ChunkedDecoder dec;
    std::string output;
    dec.on_data = [&](const uint8_t* data, size_t len) {
        output.append(reinterpret_cast<const char*>(data), len);
    };

    // "0\r\n\r\n"
    dec.feed(reinterpret_cast<const uint8_t*>("0\r\n\r\n"), 5);
    CHECK(dec.is_done(), "Should be done");
    CHECK(output.empty(), "Should be empty string");
    PASS();
}

// 5. Chunk 扩展
void test_chunk_extension() {
    TEST("Chunk extension handling");
    ChunkedDecoder dec;
    std::string output;
    dec.on_data = [&](const uint8_t* data, size_t len) {
        output.append(reinterpret_cast<const char*>(data), len);
    };

    // "5;some=extension\r\nHello\r\n0\r\n\r\n"
    const char* input = "5;some=extension\r\nHello\r\n0\r\n\r\n";
    dec.feed(reinterpret_cast<const uint8_t*>(input), strlen(input));
    CHECK(dec.is_done(), "Should be done");
    CHECK(output == "Hello", "Should decode 'Hello' (extension ignored)");
    PASS();
}

// 6. 大写十六进制
void test_uppercase_hex() {
    TEST("Uppercase hex chunk size");
    ChunkedDecoder dec;
    std::string output;
    dec.on_data = [&](const uint8_t* data, size_t len) {
        output.append(reinterpret_cast<const char*>(data), len);
    };

    // "A\r\n0123456789\r\n0\r\n\r\n"
    const char* input = "A\r\n0123456789\r\n0\r\n\r\n";
    dec.feed(reinterpret_cast<const uint8_t*>(input), strlen(input));
    CHECK(dec.is_done(), "Should be done");
    CHECK(output == "0123456789", "Should decode 10 chars");
    PASS();
}

// 7. 无效十六进制 → 错误
void test_invalid_hex() {
    TEST("Invalid hex digit detection");
    ChunkedDecoder dec;
    dec.feed(reinterpret_cast<const uint8_t*>("GG\r\n"), 4);
    CHECK(dec.is_error(), "Should be in error state");
    PASS();
}

// 8. 重置
void test_reset() {
    TEST("Reset and reuse");
    ChunkedDecoder dec;
    std::string output;
    dec.on_data = [&](const uint8_t* data, size_t len) {
        output.append(reinterpret_cast<const char*>(data), len);
    };

    dec.feed(reinterpret_cast<const uint8_t*>("3\r\nABC\r\n0\r\n\r\n"), 12);
    CHECK(output == "ABC", "");

    dec.reset();
    output.clear();
    dec.feed(reinterpret_cast<const uint8_t*>("4\r\nDEFG\r\n0\r\n\r\n"), 14);
    CHECK(output == "DEFG", "Should work after reset");
    PASS();
}

int main() {
    printf("=== ChunkedDecoder Tests ===\n\n");
    test_single_chunk();
    test_multiple_chunks();
    test_fragmented_arrival();
    test_empty_stream();
    test_chunk_extension();
    test_uppercase_hex();
    test_invalid_hex();
    test_reset();
    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
