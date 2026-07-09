// ============================================================================
// test_buffer.cpp - 缓冲池单元测试
// ============================================================================
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../cyrus_common/include/cyrus/buffer_pool.hpp"

using namespace cyrus;

static int tests_passed = 0, tests_failed = 0;

#define TEST(name) printf("  TEST: %s ... ", name);
#define PASS() do { printf("PASSED\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAILED: %s\n", msg); tests_failed++; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

// 1. 基本获取/释放
void test_acquire_release() {
    TEST("Acquire and release");

    BufferPool pool(10, 256);
    CHECK(pool.available_count() == 10, "Initially all buffers available");

    {
        auto buf = pool.acquire();
        CHECK(buf.valid(), "Should get valid buffer");
        CHECK(pool.available_count() == 9, "One buffer should be in use");
        buf.set_length(5);
        CHECK(buf.length() == 5, "Length should be set");
    }
    // buf 离开作用域 → 自动归还
    CHECK(pool.available_count() == 10, "Buffer should be returned to pool");

    PASS();
}

// 2. 池耗尽
void test_exhaust_pool() {
    TEST("Exhaust pool");

    BufferPool pool(3, 128);
    auto b1 = pool.acquire();
    auto b2 = pool.acquire();
    auto b3 = pool.acquire();
    CHECK(b1.valid() && b2.valid() && b3.valid(), "All 3 should be valid");
    CHECK(pool.available_count() == 0, "Pool should be exhausted");

    auto b4 = pool.acquire();
    CHECK(!b4.valid(), "4th acquire should return invalid handle");

    PASS();
}

// 3. 缓冲区读写
void test_read_write() {
    TEST("Write and read buffer content");

    BufferPool pool(5, 256);
    auto buf = pool.acquire();
    CHECK(buf.valid(), "Should have buffer");

    const char* test_data = "Hello, Buffer!";
    std::memcpy(buf.data(), test_data, strlen(test_data) + 1);
    buf.set_length(strlen(test_data));

    CHECK(buf.length() == strlen(test_data), "Length should match");
    CHECK(std::strcmp(reinterpret_cast<const char*>(buf.data()), test_data) == 0,
          "Data should match");

    PASS();
}

// 4. 线程安全 (基本并发)
void test_thread_safety() {
    TEST("Thread safety (concurrent acquire/release)");

    BufferPool pool(100, 64);
    std::atomic<int> error_count{0};

    auto worker = [&pool, &error_count](int id) {
        for (int i = 0; i < 50; ++i) {
            auto buf = pool.acquire();
            if (!buf.valid()) {
                error_count++;
                continue;
            }
            // 模拟一些工作
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // buf 自动归还
        }
    };

    std::thread t1(worker, 1);
    std::thread t2(worker, 2);
    std::thread t3(worker, 3);
    std::thread t4(worker, 4);

    t1.join(); t2.join(); t3.join(); t4.join();

    CHECK(error_count == 0, "No acquire failures");
    CHECK(pool.available_count() == 100, "All buffers should be returned");

    PASS();
}

// 5. BufferHandle 移动语义
void test_buffer_move() {
    TEST("BufferHandle move semantics");

    BufferPool pool(5, 128);
    auto buf1 = pool.acquire();
    CHECK(buf1.valid(), "First acquire");

    void* data_ptr = buf1.data();
    auto buf2 = std::move(buf1);
    CHECK(!buf1.valid(), "After move, source should be invalid");
    CHECK(buf2.valid(), "After move, target should be valid");
    CHECK(buf2.data() == data_ptr, "Data pointer should be preserved");

    PASS();
}

int main() {
    printf("========================================\n");
    printf("  Buffer Pool Unit Tests\n");
    printf("========================================\n\n");

    test_acquire_release();
    test_exhaust_pool();
    test_read_write();
    test_thread_safety();
    test_buffer_move();

    printf("\n========================================\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
