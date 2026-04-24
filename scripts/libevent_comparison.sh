#!/bin/bash
#
# libevent 对照测试脚本
# 用于 Chase EventLoop 与 libevent 的性能对比
#
# 使用方法：
#   ./scripts/libevent_comparison.sh
#
# 前置条件：
#   1. 已编译 Chase minimal_server 和 libevent_server
#   2. 已安装 wrk 基准测试工具
#

set -e

# 配置
CHASE_PORT=8080
LIBEVENT_PORT=8081
CHASE_SERVER="./build/examples/minimal_server"
LIBEVENT_SERVER="./build/examples/libevent_server"
DURATION=30
OUTPUT_DIR="./benchmark_results"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "libevent EventLoop 对照测试"
echo "=========================================="
echo ""

# 检查 wrk 是否安装
if ! command -v wrk &> /dev/null; then
    echo -e "${RED}Error: wrk not found. Please install wrk first.${NC}"
    echo "  macOS: brew install wrk"
    echo "  Linux: apt-get install wrk"
    exit 1
fi

# 检查服务器文件是否存在
if [ ! -f "$CHASE_SERVER" ]; then
    echo -e "${RED}Error: Chase minimal_server not found at $CHASE_SERVER${NC}"
    echo "Please build the project first: cmake --build build"
    exit 1
fi

if [ ! -f "$LIBEVENT_SERVER" ]; then
    echo -e "${YELLOW}Warning: libevent_server not found at $LIBEVENT_SERVER${NC}"
    echo "This may be because libevent was not installed."
    echo "Install libevent: vcpkg install libevent"
    echo ""
    echo "Continuing with Chase-only tests..."
    LIBEVENT_AVAILABLE=false
else
    LIBEVENT_AVAILABLE=true
fi

# 创建输出目录
mkdir -p "$OUTPUT_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
REPORT_FILE="$OUTPUT_DIR/libevent_comparison_$TIMESTAMP.md"

# 启动 Chase 服务器
echo -e "${GREEN}Starting Chase minimal_server on port $CHASE_PORT...${NC}"
$CHASE_SERVER $CHASE_PORT &
CHASE_PID=$!
sleep 2

# 验证 Chase 服务器启动
if ! curl -s http://localhost:$CHASE_PORT/ > /dev/null; then
    echo -e "${RED}Error: Chase server failed to start${NC}"
    kill $CHASE_PID 2>/dev/null || true
    exit 1
fi

echo -e "${GREEN}Chase server started successfully (PID: $CHASE_PID)${NC}"
echo ""

# 启动 libevent 服务器（如果可用）
if [ "$LIBEVENT_AVAILABLE" = true ]; then
    echo -e "${GREEN}Starting libevent_server on port $LIBEVENT_PORT...${NC}"
    $LIBEVENT_SERVER $LIBEVENT_PORT &
    LIBEVENT_PID=$!
    sleep 2

    # 验证 libevent 服务器启动
    if ! curl -s http://localhost:$LIBEVENT_PORT/ > /dev/null; then
        echo -e "${RED}Error: libevent server failed to start${NC}"
        kill $CHASE_PID 2>/dev/null || true
        kill $LIBEVENT_PID 2>/dev/null || true
        exit 1
    fi

    echo -e "${GREEN}libevent server started successfully (PID: $LIBEVENT_PID)${NC}"
    echo ""
fi

# 测试函数
run_benchmark() {
    local name=$1
    local port=$2
    local threads=$3
    local connections=$4
    local duration=$5

    echo "Running benchmark: $name (threads=$threads, connections=$connections, duration=$duration)"
    wrk -t$threads -c$connections -d$duration --latency http://localhost:$port/ 2>&1
}

# 开始报告
echo "# libevent EventLoop 对照测试报告" > "$REPORT_FILE"
echo "" >> "$REPORT_FILE"
echo "**测试日期**: $(date)" >> "$REPORT_FILE"
echo "**测试环境**: $(uname -a)" >> "$REPORT_FILE"
echo "" >> "$REPORT_FILE"

# 场景 1: 低并发测试
echo -e "${YELLOW}=== 场景 1: 低并发测试 (t1 c10 d10s) ===${NC}"
{
    echo "## 场景 1: 低并发测试" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
    echo "**参数**: -t1 -c10 -d10s" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"

    echo "### Chase minimal_server" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
    echo '```' >> "$REPORT_FILE"
    run_benchmark "Chase 低并发" $CHASE_PORT 1 10 10s >> "$REPORT_FILE"
    echo '```' >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"

    if [ "$LIBEVENT_AVAILABLE" = true ]; then
        echo "### libevent_server" >> "$REPORT_FILE"
        echo "" >> "$REPORT_FILE"
        echo '```' >> "$REPORT_FILE"
        run_benchmark "libevent 低并发" $LIBEVENT_PORT 1 10 10s >> "$REPORT_FILE"
        echo '```' >> "$REPORT_FILE"
        echo "" >> "$REPORT_FILE"
    fi
}

# 场景 2: 中等并发测试
echo -e "${YELLOW}=== 场景 2: 中等并发测试 (t4 c100 d30s) ===${NC}"
{
    echo "## 场景 2: 中等并发测试" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
    echo "**参数**: -t4 -c100 -d30s" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"

    echo "### Chase minimal_server" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
    echo '```' >> "$REPORT_FILE"
    run_benchmark "Chase 中等并发" $CHASE_PORT 4 100 $DURATION >> "$REPORT_FILE"
    echo '```' >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"

    if [ "$LIBEVENT_AVAILABLE" = true ]; then
        echo "### libevent_server" >> "$REPORT_FILE"
        echo "" >> "$REPORT_FILE"
        echo '```' >> "$REPORT_FILE"
        run_benchmark "libevent 中等并发" $LIBEVENT_PORT 4 100 $DURATION >> "$REPORT_FILE"
        echo '```' >> "$REPORT_FILE"
        echo "" >> "$REPORT_FILE"
    fi
}

# 场景 3: 高并发测试
echo -e "${YELLOW}=== 场景 3: 高并发测试 (t8 c500 d60s) ===${NC}"
{
    echo "## 场景 3: 高并发测试" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
    echo "**参数**: -t8 -c500 -d60s" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"

    echo "### Chase minimal_server" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
    echo '```' >> "$REPORT_FILE"
    run_benchmark "Chase 高并发" $CHASE_PORT 8 500 60s >> "$REPORT_FILE"
    echo '```' >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"

    if [ "$LIBEVENT_AVAILABLE" = true ]; then
        echo "### libevent_server" >> "$REPORT_FILE"
        echo "" >> "$REPORT_FILE"
        echo '```' >> "$REPORT_FILE"
        run_benchmark "libevent 高并发" $LIBEVENT_PORT 8 500 60s >> "$REPORT_FILE"
        echo '```' >> "$REPORT_FILE"
        echo "" >> "$REPORT_FILE"
    fi
}

# 内存分析（可选）
echo -e "${YELLOW}=== 内存分析 ===${NC}"
{
    echo "## 内存占用分析" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"

    # Chase 内存
    CHASE_RSS=$(ps -o rss= -p $CHASE_PID | awk '{print $1}')
    echo "Chase minimal_server 内存占用: ${CHASE_RSS} KB" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"

    if [ "$LIBEVENT_AVAILABLE" = true ]; then
        LIBEVENT_RSS=$(ps -o rss= -p $LIBEVENT_PID | awk '{print $1}')
        echo "libevent_server 内存占用: ${LIBEVENT_RSS} KB" >> "$REPORT_FILE"
        echo "" >> "$REPORT_FILE"
    fi
}

# API 易用性分析
{
    echo "## API 易用性分析" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
    echo "### Chase 优势" >> "$REPORT_FILE"
    echo "1. 简洁的事件模型: 仅需 eventloop_add() 一个函数注册事件" >> "$REPORT_FILE"
    echo "2. 回调设计直观: 回调函数签名清晰，参数明确" >> "$REPORT_FILE"
    echo "3. 错误处理简单: 使用统一的 Error 模块" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"

    echo "### libevent 优势" >> "$REPORT_FILE"
    echo "1. 功能丰富: 支持 DNS、HTTP、RPC 等高级功能" >> "$REPORT_FILE"
    echo "2. 跨平台成熟: 经过多年生产验证" >> "$REPORT_FILE"
    echo "3. 缓冲管理自动: bufferevent 自动管理读写缓冲" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"

    echo "### 主观评分" >> "$REPORT_FILE"
    echo "| 维度 | Chase | libevent |" >> "$REPORT_FILE"
    echo "|-----|-------|----------|" >> "$REPORT_FILE"
    echo "| 易学性 | 8/10 | 6/10 |" >> "$REPORT_FILE"
    echo "| 灵活性 | 7/10 | 9/10 |" >> "$REPORT_FILE"
    echo "| 可维护性 | 8/10 | 7/10 |" >> "$REPORT_FILE"
    echo "| 生产就绪度 | 6/10 | 9/10 |" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
}

# 结论
{
    echo "## 结论" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
    echo "测试完成，请根据上述数据进行分析。" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
    echo "**建议**:" >> "$REPORT_FILE"
    echo "1. 如果 Chase 性能接近 libevent，说明 EventLoop 设计合理" >> "$REPORT_FILE"
    echo "2. 如果 Chase 内存更低，说明资源管理高效" >> "$REPORT_FILE"
    echo "3. 参考 libevent 的 bufferevent 设计，优化连接管理" >> "$REPORT_FILE"
}

# 清理
echo ""
echo -e "${GREEN}Cleaning up servers...${NC}"
kill $CHASE_PID 2>/dev/null || true
if [ "$LIBEVENT_AVAILABLE" = true ]; then
    kill $LIBEVENT_PID 2>/dev/null || true
fi

echo ""
echo -e "${GREEN}Report saved to: $REPORT_FILE${NC}"
echo "=========================================="
echo "测试完成"
echo "=========================================="

cat "$REPORT_FILE"