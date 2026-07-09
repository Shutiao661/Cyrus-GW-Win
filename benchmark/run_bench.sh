#!/bin/bash
# ============================================================================
# run_bench.sh - 多引擎多并发基准测试脚本 (Linux)
# ============================================================================
# 测试矩阵:
#   - 引擎: epoll (Reactor) / io_uring (C++20 协程)
#   - 并发: C=100 / 300 / 500
#   - 路径: pure_error / agent_single / full_chain
#
# 输出: bench_results.csv + 终端摘要
# ============================================================================

set -e

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULT_FILE="$BENCH_DIR/bench_results.csv"
ENGINE="${1:-both}"  # epoll | uring | both
DURATION=30

echo "============================================"
echo "  Cyrus-GW Benchmark Suite"
echo "  Engine: $ENGINE | Duration: ${DURATION}s"
echo "============================================"

# CSV header
echo "engine,path,clients,rps,errors,timeouts,success_rate,p50_us,p90_us,p99_us" > "$RESULT_FILE"

# 测试函数
run_bench() {
    local engine=$1
    local path=$2
    local clients=$3
    local binary=""

    if [ "$engine" = "epoll" ]; then
        binary="$BENCH_DIR/bench_epoll"
    elif [ "$engine" = "uring" ]; then
        binary="$BENCH_DIR/bench_uring"
    else
        echo "Unknown engine: $engine"
        return
    fi

    if [ ! -f "$binary" ]; then
        echo "  [SKIP] $binary not found"
        echo "$engine,$path,$clients,0,0,0,0,0,0,0" >> "$RESULT_FILE"
        return
    fi

    echo ""
    echo "--- Running: $engine | $path | C=$clients ---"
    "$binary" --clients="$clients" --path="$path" --duration="$DURATION" 2>&1 | tee /tmp/bench_output.txt

    # 提取指标 (简单 grep)
    local rps=$(grep "RPS:" /tmp/bench_output.txt | awk '{print $2}')
    local errors=$(grep "Errors:" /tmp/bench_output.txt | awk '{print $2}')
    local timeouts=$(grep "Timeouts:" /tmp/bench_output.txt | awk '{print $2}')
    local p50=$(grep "P50:" /tmp/bench_output.txt | awk '{print $3}')
    local p90=$(grep "P90:" /tmp/bench_output.txt | awk '{print $3}')
    local p99=$(grep "P99:" /tmp/bench_output.txt | awk '{print $3}')
    local success=$(grep "Success:" /tmp/bench_output.txt | awk '{print $2}')

    echo "$engine,$path,$clients,${rps:-0},${errors:-0},${timeouts:-0},${success:-0},${p50:-0},${p90:-0},${p99:-0}" >> "$RESULT_FILE"
}

# 执行测试矩阵
PATHS=("pure_error" "agent_single" "full_chain")
CLIENTS=(100 300 500)

for path in "${PATHS[@]}"; do
    for clients in "${CLIENTS[@]}"; do
        if [ "$ENGINE" = "both" ] || [ "$ENGINE" = "epoll" ]; then
            run_bench "epoll" "$path" "$clients"
        fi
        if [ "$ENGINE" = "both" ] || [ "$ENGINE" = "uring" ]; then
            run_bench "uring" "$path" "$clients"
        fi
    done
done

echo ""
echo "============================================"
echo "  Benchmark Complete"
echo "  Results: $RESULT_FILE"
echo "============================================"

# 生成摘要
echo ""
echo "Summary (RPS comparison):"
echo "-------------------------"
column -t -s',' "$RESULT_FILE"
