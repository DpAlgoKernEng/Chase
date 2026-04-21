#!/bin/bash
# scripts/diagnose_multiprocess_performance.sh
# 多进程性能诊断脚本

echo "=== Chase 多进程性能诊断 ==="
echo ""

# 1. 检查 CPU 核数
echo ">>> CPU 信息"
if [[ "$OSTYPE" == "darwin"* ]]; then
    CPU_CORES=$(sysctl -n hw.ncpu)
    echo "CPU 核数: $CPU_CORES (macOS)"
else
    CPU_CORES=$(nproc)
    echo "CPU 核数: $CPU_CORES (Linux)"
fi

# 2. 检查当前进程数
echo ""
echo ">>> 进程信息"
SERVER_PIDS=$(pgrep -f "production_server|minimal_server" 2>/dev/null)
if [ -n "$SERVER_PIDS" ]; then
    PROCESS_COUNT=$(echo "$SERVER_PIDS" | wc -l | tr -d ' ')
    echo "服务器进程数: $PROCESS_COUNT"
    for pid in $SERVER_PIDS; do
        # macOS 使用 ps，Linux 也可以
        CPU_USAGE=$(ps -p $pid -o %cpu= 2>/dev/null | tr -d ' ')
        MEM_USAGE=$(ps -p $pid -o rss= 2>/dev/null | tr -d ' ')
        echo "  PID $pid: CPU=${CPU_USAGE}%, MEM=${MEM_USAGE}KB"
    done
else
    echo "未找到服务器进程"
fi

# 3. 连接分布分析
echo ""
echo ">>> 连接分布（如果有服务器运行）"
if [ -n "$SERVER_PIDS" ]; then
    for pid in $SERVER_PIDS; do
        if [[ "$OSTYPE" == "darwin"* ]]; then
            # macOS 使用 lsof
            CONN_COUNT=$(lsof -p $pid -i TCP 2>/dev/null | grep -c ESTABLISHED || echo 0)
            echo "  PID $pid: $CONN_COUNT 个已建立连接"
        else
            # Linux 使用 ss 或 netstat
            CONN_COUNT=$(ss -tnp 2>/dev/null | grep "pid=$pid" | grep -c ESTAB || echo 0)
            echo "  PID $pid: $CONN_COUNT 个已建立连接"
        fi
    done
fi

# 4. 上下文切换分析（Linux）
echo ""
echo ">>> 上下文切换分析"
if [[ "$OSTYPE" != "darwin"* ]]; then
    if command -v pidstat &> /dev/null; then
        echo "正在监控上下文切换（5秒）..."
        pidstat -w -t -p ALL 1 5 2>/dev/null | grep -E "production_server|minimal_server" || echo "无显著上下文切换"
    else
        echo "pidstat 未安装，使用 vmstat"
        vmstat 1 5 | tail -5
    fi
else
    echo "macOS: 使用 sample 命令分析（需要手动运行）"
    echo "  sample <pid> 5"
fi

# 5. 建议
echo ""
echo ">>> 性能建议"
if [ "$CPU_CORES" -le 4 ]; then
    echo "⚠️  CPU 核数较少 ($CPU_CORES)，建议减少 Worker 数量"
    echo "   建议 Worker 数量: 1-2"
else
    echo "✅ CPU 核数充足 ($CPU_CORES)，可以使用 4+ Workers"
fi

if [ "$PROCESS_COUNT" -gt "$CPU_CORES" ]; then
    echo "⚠️  进程数 ($PROCESS_COUNT) > CPU 核数 ($CPU_CORES)，存在竞争"
fi

echo ""
echo "=== 诊断完成 ==="