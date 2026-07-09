// ============================================================================
// test_token_bucket.cpp - Token Bucket 限流器单元测试
// ============================================================================
#include <cstdio>
#include <thread>
#include <vector>
#include <chrono>

#include "../cyrus_common/include/cyrus/token_bucket.hpp"

using namespace cyrus;

static int tests_passed = 0, tests_failed = 0;

#define TEST(name) printf("  TEST: %s ... ", name);
#define PASS() do { printf("PASSED\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAILED: %s\n", msg); tests_failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

// 1. 初始满桶
void test_initial_full() {
    TEST("Initial bucket is full");
    TokenBucket bucket(100, 200);  // 100 tokens/s, capacity 200
    CHECK(bucket.try_consume(1), "Should allow first consumption");
    PASS();
}

// 2. 突发容量
void test_burst_capacity() {
    TEST("Burst capacity respected");
    TokenBucket bucket(100, 50);  // 100 tokens/s, but max 50 tokens stored
    int consumed = 0;
    for (int i = 0; i < 100; ++i) {
        if (bucket.try_consume(1)) consumed++;
    }
    CHECK(consumed <= 55, "Should not exceed burst capacity significantly");
    CHECK(consumed >= 45, "Should consume at least burst capacity");
    printf("(consumed: %d) ", consumed);
    PASS();
}

// 3. 速率限制
void test_rate_limit() {
    TEST("Rate limit enforced");
    TokenBucket bucket(10, 10);  // 10 tokens/s, capacity 10
    // 先消耗所有 token
    for (int i = 0; i < 10; ++i) {
        CHECK(bucket.try_consume(1), "Should consume initial tokens");
    }
    // 立即再消费应失败
    CHECK(!bucket.try_consume(1), "Should be rate limited");
    PASS();
}

// 4. Refill 机制
void test_refill() {
    TEST("Token refill over time");
    TokenBucket bucket(100, 100);  // 100 tokens/s
    // 消耗所有 token
    for (int i = 0; i < 100; ++i) {
        bucket.try_consume(1);
    }
    CHECK(!bucket.try_consume(1), "Should be empty");
    // 等待 100ms → 应补充 ~10 tokens
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int consumed = 0;
    for (int i = 0; i < 20; ++i) {
        if (bucket.try_consume(1)) consumed++;
    }
    CHECK(consumed >= 5, "Should refill some tokens after wait");
    CHECK(consumed <= 15, "Should not over-refill");
    printf("(refilled: %d) ", consumed);
    PASS();
}

// 5. 多线程并发
void test_concurrent() {
    TEST("Concurrent access correctness");
    TokenBucket bucket(1000, 2000);
    std::atomic<int> total{0};
    std::atomic<int> failed{0};

    auto worker = [&]() {
        for (int i = 0; i < 500; ++i) {
            if (bucket.try_consume(1)) {
                total.fetch_add(1, std::memory_order_relaxed);
            } else {
                failed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) t.join();

    int consumed = total.load();
    CHECK(consumed > 0, "Should consume some tokens concurrently");
    CHECK(consumed <= 2000 + 500, "Should not exceed capacity + refill");
    printf("(4 threads, consumed: %d) ", consumed);
    PASS();
}

// 6. RateLimiter 两级限流
void test_rate_limiter() {
    TEST("Two-level rate limiter");
    RateLimiter limiter(1000, 2000, 50, 100);  // global 1K/s, per-IP 50/s

    // 全局检查
    CHECK(limiter.check(""), "Global should pass");
    // 同 IP 连续请求应在 per-IP 限制内
    int passed = 0;
    for (int i = 0; i < 200; ++i) {
        if (limiter.check("192.168.1.1")) passed++;
    }
    CHECK(passed <= 105, "Per-IP rate limit should cap at ~burst+1");
    CHECK(passed >= 90, "Should allow at least burst capacity");
    printf("(passed: %d/200) ", passed);
    PASS();
}

// 7. 动态速率调整
void test_dynamic_rate() {
    TEST("Dynamic rate adjustment");
    TokenBucket bucket(10, 10);
    for (int i = 0; i < 10; ++i) bucket.try_consume(1);
    CHECK(!bucket.try_consume(1), "Should be empty at rate=10");
    bucket.set_rate(1000);  // 提高到 1000 tokens/s
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(bucket.try_consume(5), "Should allow consumption after rate increase");
    PASS();
}

int main() {
    printf("=== Token Bucket Tests ===\n\n");
    test_initial_full();
    test_burst_capacity();
    test_rate_limit();
    test_refill();
    test_concurrent();
    test_rate_limiter();
    test_dynamic_rate();
    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
