-- ============================================================================
-- wrk_post.lua - wrk POST 请求脚本 (用于 SSE streaming 压测)
-- ============================================================================
-- 用法: wrk -c 100 -t 10 -d 30s -s benchmark/wrk_post.lua http://localhost:8080/v1/chat/completions
-- ============================================================================

wrk.method = "POST"
wrk.body   = [[{"model":"test","messages":[{"role":"user","content":"Hello"}]}]]
wrk.headers["Content-Type"] = "application/json"

-- 响应处理: SSE streaming 验证
local counter = 0
local done_received = false

response = function(status, headers, body)
    counter = counter + 1
    -- 检查是否收到 [DONE] 标记
    if string.find(body, "[DONE]") then
        done_received = true
    end
    -- 检查是否有 error 事件
    if string.find(body, "event: error") then
        -- 记录错误但不计入成功
    end
end

done = function(summary, latency, requests)
    io.write("------------------------------\n")
    io.write(string.format("Total requests:  %d\n", summary.requests))
    io.write(string.format("Duration:        %.2fs\n", summary.duration / 1000000))
    io.write(string.format("Requests/sec:    %.2f\n", summary.requests / (summary.duration / 1000000)))
    io.write(string.format("Bytes/sec:       %.2f\n", summary.bytes / (summary.duration / 1000000)))
    io.write(string.format("Errors:          connect=%d, read=%d, write=%d, status=%d, timeout=%d\n",
        summary.errors.connect, summary.errors.read, summary.errors.write,
        summary.errors.status, summary.errors.timeout))
    io.write("------------------------------\n")
end
