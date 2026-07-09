# ============================================================================
# CompilerWarnings.cmake - 编译器警告配置
# 为每个目标设置合理的警告级别和编译选项
# ============================================================================

# ---------------------------------------------------------------------------
# set_compiler_warnings(target_name)
#   为目标设置高警告级别和其他编译选项
#   - MSVC: /W4 (高警告级别), /utf-8 (源码 UTF-8), /permissive- (标准一致性)
#   - GCC/Clang: -Wall -Wextra -Wpedantic 等
# ---------------------------------------------------------------------------
function(set_compiler_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4              # 警告级别 4 (最高实用性级别, /Wall 会产生大量系统头文件警告)
            /utf-8           # 源文件和执行字符集均为 UTF-8 (支持中文注释)
            /permissive-     # 严格标准一致性模式
            /Zc:__cplusplus  # 正确设置 __cplusplus 宏 (否则 MSVC 始终报告 199711L)
            /Zc:preprocessor # 符合标准的预处理器 (C++20 __VA_OPT__ 等需要)
            /wd4996          # 禁用 POSIX 弃用警告 (strerror 等在 MSVC 上被视为不安全)
        )
        # 将警告视为错误 (可选，确保代码质量)
        # target_compile_options(${target_name} PRIVATE /WX)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wcast-align
            -Woverloaded-virtual
            -Wconversion
            -Wsign-conversion
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
            -Wimplicit-fallthrough
            -Wno-c++20-compat
        )
    endif()
endfunction()
