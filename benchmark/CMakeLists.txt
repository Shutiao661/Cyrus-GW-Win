# ============================================================================
# benchmark/CMakeLists.txt - 基准测试
# ============================================================================

# IOCP/Select 基准 (Windows/Linux 通用)
add_executable(cyrus_benchmark bench_main.cpp)
target_link_libraries(cyrus_benchmark PRIVATE cyrus_common)
target_include_directories(cyrus_benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../cyrus_gateway/include
)
if(WIN32)
    target_link_libraries(cyrus_benchmark PRIVATE ${WINSOCK_LIBRARIES})
endif()
set_compiler_warnings(cyrus_benchmark)

# epoll Edge-Triggered 基准 (Linux only)
if(UNIX AND NOT APPLE)
    add_executable(cyrus_bench_epoll bench_epoll.cpp)
    target_link_libraries(cyrus_bench_epoll PRIVATE cyrus_common pthread)
    target_include_directories(cyrus_bench_epoll PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../cyrus_gateway/include
    )
    set_compiler_warnings(cyrus_bench_epoll)

    # io_uring + C++20 协程基准 (Linux only, 需要 liburing)
    find_library(LIBURING liburing.a)
    find_library(LIBURING_SHARED liburing.so)
    if(LIBURING OR LIBURING_SHARED)
        add_executable(cyrus_bench_uring bench_uring.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../cyrus_gateway/src/io_engine_uring.cpp)
        target_link_libraries(cyrus_bench_uring PRIVATE cyrus_common pthread uring)
        target_include_directories(cyrus_bench_uring PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../cyrus_gateway/include
        )
        target_compile_features(cyrus_bench_uring PRIVATE cxx_std_20)
        set_compiler_warnings(cyrus_bench_uring)
    else()
        message(STATUS "liburing not found — skipping bench_uring")
    endif()
endif()
