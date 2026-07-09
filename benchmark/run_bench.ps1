# ============================================================================
# run_bench.ps1 - 多引擎多并发基准测试脚本 (Windows)
# ============================================================================
# 在 Windows 上使用 wrk 对 Cyrus-GW Gateway 进行压测。
#
# 前置条件:
#   1. 启动 Agent:  .\cyrus_agent.exe --port 9999
#   2. 启动 Gateway: .\cyrus_gateway.exe config\gateway.conf
#   3. 安装 wrk:     winget install wrk
#
# 测试矩阵:
#   - 并发: C=100 / 300 / 500
#   - 路径: /health (纯错误/静态), /v1/chat/completions (SSE流式)
#
# 输出: bench_results_win.csv
# ============================================================================

param(
    [string]$Host = "localhost",
    [int]$Port = 8080,
    [int]$Duration = 30,
    [string]$WrkPath = "wrk"
)

$ResultFile = Join-Path $PSScriptRoot "bench_results_win.csv"
$BaseUrl = "http://${Host}:${Port}"

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Cyrus-GW Windows Benchmark" -ForegroundColor Cyan
Write-Host "  Target: $BaseUrl" -ForegroundColor Cyan
Write-Host "  Duration: ${Duration}s per test" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan

# CSV Header
"path,method,clients,rps,latency_avg,latency_p99,timeout_rate" | Out-File -FilePath $ResultFile -Encoding utf8

# 测试函数
function Run-WrkTest {
    param([string]$Path, [string]$Method, [int]$Clients)

    $url = "$BaseUrl$Path"
    $bodyFile = $null

    if ($Method -eq "POST") {
        $body = '{"model":"test","messages":[{"role":"user","content":"Hello"}]}'
        $bodyFile = Join-Path $env:TEMP "cyrus_bench_body.json"
        $body | Out-File -FilePath $bodyFile -Encoding utf8 -NoNewline
    }

    Write-Host ""
    Write-Host "--- $Method $Path | C=$Clients ---" -ForegroundColor Yellow

    $args = @(
        "-c", $Clients,
        "-t", [Math]::Min($Clients, 16),
        "-d", "${Duration}s",
        "--latency"
    )

    if ($Method -eq "POST" -and $bodyFile) {
        $args += "-s", (Join-Path $PSScriptRoot "wrk_post.lua")
    }

    $args += $url

    try {
        $output = & $WrkPath @args 2>&1 | Out-String

        # 提取指标
        $rpsMatch = [regex]::Match($output, 'Requests/sec:\s+([\d.]+)')
        $latAvgMatch = [regex]::Match($output, 'Latency\s+([\d.]+)\s*(\w+)')
        $latP99Match = [regex]::Match($output, '99%\s+([\d.]+)\s*(\w+)')
        $timeoutMatch = [regex]::Match($output, 'Socket errors:.*timeout\s+(\d+)')

        $rps = if ($rpsMatch.Success) { $rpsMatch.Groups[1].Value } else { "0" }
        $latAvg = if ($latAvgMatch.Success) { $latAvgMatch.Groups[1].Value } else { "0" }
        $latP99 = if ($latP99Match.Success) { $latP99Match.Groups[1].Value } else { "0" }
        $timeouts = if ($timeoutMatch.Success) { $timeoutMatch.Groups[1].Value } else { "0" }

        "$Path,$Method,$Clients,$rps,$latAvg,$latP99,$timeouts" | Out-File -FilePath $ResultFile -Append -Encoding utf8

        Write-Host "  RPS: $rps | Latency(avg): ${latAvg}ms | P99: ${latP99}ms | Timeouts: $timeouts" -ForegroundColor Green

    } catch {
        Write-Host "  [ERROR] wrk failed: $_" -ForegroundColor Red
        "$Path,$Method,$Clients,0,0,0,0" | Out-File -FilePath $ResultFile -Append -Encoding utf8
    }

    if ($bodyFile -and (Test-Path $bodyFile)) {
        Remove-Item $bodyFile
    }
}

# 快速健康检查
Write-Host ""
Write-Host "Checking Gateway health..." -ForegroundColor Cyan
try {
    $health = Invoke-RestMethod -Uri "$BaseUrl/health" -TimeoutSec 5
    Write-Host "  Gateway: $($health | ConvertTo-Json -Compress)" -ForegroundColor Green
} catch {
    Write-Host "  [FATAL] Gateway not reachable at $BaseUrl. Start gateway first." -ForegroundColor Red
    exit 1
}

# 测试矩阵
$Clients = @(100, 300, 500)

# 1. Pure error / static response
foreach ($c in $Clients) {
    Run-WrkTest -Path "/health" -Method "GET" -Clients $c
}

# 2. Full chain SSE streaming
foreach ($c in $Clients) {
    Run-WrkTest -Path "/v1/chat/completions" -Method "POST" -Clients $c
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Benchmark Complete" -ForegroundColor Cyan
Write-Host "  Results: $ResultFile" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan

# 摘要表格
Write-Host ""
Write-Host "Summary:" -ForegroundColor Yellow
Import-Csv $ResultFile | Format-Table -AutoSize
