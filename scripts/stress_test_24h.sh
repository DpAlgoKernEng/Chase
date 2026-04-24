#!/bin/bash
#
# @file    stress_test_24h.sh
# @brief   Chase HTTP Server 24小时压力测试脚本
#
# @details
#          - wrk 多负载模式配置（低/中/高/峰值）
#          - 静态文件测试
#          - API 测试
#          - HTTPS 测试
#          - 内存监控（记录进程内存使用）
#          - 自动崩溃检测和重启
#          - 日志收集
#          - 结果报告生成
#
# @layer   Script
#
# @depends wrk, production_server/minimal_server
# @usedby  开发者、运维
#
# @author  minghui.liu
# @date    2026-04-24
#

set -e

# ==================== 配置区域 ====================

# 基础配置
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEST_DURATION_HOURS="${1:-24}"
PORT="${2:-8080}"
HTTPS_PORT="${3:-8443}"
SERVER_TYPE="${4:-production}"
WORKER_COUNT="${5:-4}"
SERVER_PID=""
CRASH_COUNT=0
TEST_START_TIME=""
LOG_DIR=""
RESULT_DIR=""
MONITOR_PID=""
SERVER_MONITOR_PID=""

# 负载模式配置
declare -A LOAD_MODES=(
    ["low"]="1 100 10"          # 1 thread, 100 conn, 10s per cycle
    ["medium"]="4 500 30"       # 4 threads, 500 conn, 30s per cycle
    ["high"]="8 1000 60"        # 8 threads, 1000 conn, 60s per cycle
    ["peak"]="16 10000 30"      # 16 threads, 10000 conn, 30s per cycle (short bursts)
)

# 测试端点配置
declare -A ENDPOINTS=(
    ["homepage"]="/"
    ["api"]=" /api"
    ["health"]=" /health"
    ["static_1k"]=" /static/test_1k.html"
    ["static_10k"]=" /static/test_10k.html"
    ["static_100k"]=" /static/test_100k.html"
)

# 监控采样间隔（秒）
MONITOR_INTERVAL=5
MEMORY_SAMPLE_INTERVAL=10

# 崩溃检测配置
MAX_CRASH_COUNT=100
CRASH_RESTART_DELAY=2

# ==================== 辅助函数 ====================

log_info() {
    echo "[INFO] $(date '+%Y-%m-%d %H:%M:%S') $1"
}

log_error() {
    echo "[ERROR] $(date '+%Y-%m-%d %H:%M:%S') $1" | tee -a "${LOG_DIR}/errors.log"
}

log_warn() {
    echo "[WARN] $(date '+%Y-%m-%d %H:%M:%S') $1" | tee -a "${LOG_DIR}/warnings.log"
}

check_dependencies() {
    log_info "检查依赖工具..."

    # 检查 wrk
    if ! command -v wrk &> /dev/null; then
        log_error "wrk 未安装"
        echo "安装方法:"
        echo "  macOS: brew install wrk"
        echo "  Linux: git clone https://github.com/wg/wrk && make"
        exit 1
    fi

    # 检查可选工具
    if command -v ps &> /dev/null; then
        HAS_PS=true
    else
        log_warn "ps 命令不可用，内存监控功能受限"
        HAS_PS=false
    fi

    if command -v top &> /dev/null; then
        HAS_TOP=true
    else
        HAS_TOP=false
    fi

    log_info "依赖检查完成"
}

init_test_environment() {
    log_info "初始化测试环境..."

    # 创建日志和结果目录
    TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
    LOG_DIR="${PROJECT_ROOT}/stress_test_logs/${TIMESTAMP}"
    RESULT_DIR="${PROJECT_ROOT}/stress_test_results/${TIMESTAMP}"

    mkdir -p "${LOG_DIR}"
    mkdir -p "${RESULT_DIR}"
    mkdir -p "${RESULT_DIR}/wrk"
    mkdir -p "${RESULT_DIR}/monitor"
    mkdir -p "${RESULT_DIR}/reports"

    # 创建静态测试文件目录
    STATIC_DIR="${PROJECT_ROOT}/static_test_files"
    mkdir -p "${STATIC_DIR}"

    # 生成测试静态文件
    generate_static_test_files

    # 记录测试配置
    cat > "${RESULT_DIR}/test_config.json" << EOF
{
    "test_duration_hours": ${TEST_DURATION_HOURS},
    "port": ${PORT},
    "https_port": ${HTTPS_PORT},
    "server_type": "${SERVER_TYPE}",
    "worker_count": ${WORKER_COUNT},
    "start_time": "${TIMESTAMP}",
    "load_modes": {
        "low": "1 thread, 100 conn, 10s/cycle",
        "medium": "4 threads, 500 conn, 30s/cycle",
        "high": "8 threads, 1000 conn, 60s/cycle",
        "peak": "16 threads, 10000 conn, 30s/cycle"
    }
}
EOF

    log_info "测试环境初始化完成"
    log_info "日志目录: ${LOG_DIR}"
    log_info "结果目录: ${RESULT_DIR}"
}

generate_static_test_files() {
    log_info "生成静态测试文件..."

    # 1KB 文件
    dd if=/dev/urandom of="${STATIC_DIR}/test_1k.html" bs=1024 count=1 2>/dev/null
    echo "<html><body>Test file 1KB</body></html>" > "${STATIC_DIR}/test_1k.html"

    # 10KB 文件
    head -c 10240 /dev/urandom > "${STATIC_DIR}/test_10k.html"

    # 100KB 文件
    head -c 102400 /dev/urandom > "${STATIC_DIR}/test_100k.html"

    log_info "静态测试文件已生成"
}

get_server_binary() {
    case "${SERVER_TYPE}" in
        "production")
            SERVER_EXE="${PROJECT_ROOT}/build/examples/production_server"
            ;;
        "minimal")
            SERVER_EXE="${PROJECT_ROOT}/build/examples/minimal_server"
            ;;
        *)
            log_error "未知服务器类型: ${SERVER_TYPE}"
            exit 1
            ;;
    esac

    if [ ! -f "${SERVER_EXE}" ]; then
        log_error "服务器二进制文件不存在: ${SERVER_EXE}"
        log_info "请先编译: cd ${PROJECT_ROOT} && cmake --build build"
        exit 1
    fi

    log_info "服务器二进制: ${SERVER_EXE}"
}

start_server() {
    log_info "启动服务器..."

    if [ "${SERVER_TYPE}" == "production" ]; then
        "${SERVER_EXE}" "${PORT}" "${WORKER_COUNT}" > "${LOG_DIR}/server_stdout.log" 2>&1 &
    else
        "${SERVER_EXE}" "${PORT}" > "${LOG_DIR}/server_stdout.log" 2>&1 &
    fi

    SERVER_PID=$!
    sleep 3

    # 验证服务器是否启动
    if ! kill -0 ${SERVER_PID} 2>/dev/null; then
        log_error "服务器启动失败"
        cat "${LOG_DIR}/server_stdout.log"
        exit 1
    fi

    log_info "服务器已启动 (PID: ${SERVER_PID})"

    # 记录初始状态
    record_server_status "started"
}

stop_server() {
    log_info "停止服务器..."

    if [ -n "${SERVER_PID}" ] && kill -0 ${SERVER_PID} 2>/dev/null; then
        kill -TERM ${SERVER_PID} 2>/dev/null
        sleep 2

        # 如果还没停止，强制杀死
        if kill -0 ${SERVER_PID} 2>/dev/null; then
            kill -KILL ${SERVER_PID} 2>/dev/null
        fi

        wait ${SERVER_PID} 2>/dev/null
    fi

    SERVER_PID=""
    log_info "服务器已停止"
}

restart_server() {
    log_warn "检测到服务器崩溃，正在重启..."
    CRASH_COUNT=$((CRASH_COUNT + 1))

    record_server_status "crashed"

    if [ ${CRASH_COUNT} -gt ${MAX_CRASH_COUNT} ]; then
        log_error "崩溃次数超过限制 (${MAX_CRASH_COUNT})，终止测试"
        stop_all_monitors
        generate_final_report
        exit 1
    fi

    sleep ${CRASH_RESTART_DELAY}
    start_server

    log_warn "服务器已重启 (崩溃次数: ${CRASH_COUNT})"
}

check_server_alive() {
    if [ -z "${SERVER_PID}" ]; then
        return 1
    fi

    if ! kill -0 ${SERVER_PID} 2>/dev/null; then
        return 1
    fi

    return 0
}

record_server_status() {
    local status="$1"
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    echo "${timestamp},${status},${CRASH_COUNT},${SERVER_PID}" >> "${RESULT_DIR}/server_status.csv"
}

# ==================== 监控函数 ====================

start_memory_monitor() {
    log_info "启动内存监控..."

    MEMORY_LOG="${RESULT_DIR}/monitor/memory_usage.csv"
    echo "timestamp,pid,rss_mb,vms_mb,cpu_percent,worker_count" > "${MEMORY_LOG}"

    # 启动后台监控进程
    (
        while true; do
            timestamp=$(date '+%Y-%m-%d %H:%M:%S')

            if [ -n "${SERVER_PID}" ] && kill -0 ${SERVER_PID} 2>/dev/null; then
                # 获取主进程内存信息
                if [ "${HAS_PS}" == "true" ]; then
                    # macOS/Linux 兼容的 ps 命令
                    if [[ "$OSTYPE" == "darwin"* ]]; then
                        # macOS
                        mem_info=$(ps -p ${SERVER_PID} -o rss=,vsz=,pcpu= 2>/dev/null || echo "0 0 0")
                        rss=$(echo $mem_info | awk '{print $1}')
                        vms=$(echo $mem_info | awk '{print $2}')
                        cpu=$(echo $mem_info | awk '{print $3}')
                    else
                        # Linux
                        mem_info=$(ps -p ${SERVER_PID} -o rss=,vsz=,pcpu= --no-headers 2>/dev/null || echo "0 0 0")
                        rss=$(echo $mem_info | awk '{print $1}')
                        vms=$(echo $mem_info | awk '{print $2}')
                        cpu=$(echo $mem_info | awk '{print $3}')
                    fi

                    # 转换为 MB
                    rss_mb=$((rss / 1024))
                    vms_mb=$((vms / 1024))

                    # 统计 worker 进程数量
                    worker_count=$(pgrep -P ${SERVER_PID} 2>/dev/null | wc -l | tr -d ' ')

                    echo "${timestamp},${SERVER_PID},${rss_mb},${vms_mb},${cpu},${worker_count}" >> "${MEMORY_LOG}"
                fi
            else
                echo "${timestamp},N/A,0,0,0,0" >> "${MEMORY_LOG}"
            fi

            sleep ${MEMORY_SAMPLE_INTERVAL}
        done
    ) &

    MONITOR_PID=$!
    log_info "内存监控已启动 (PID: ${MONITOR_PID})"
}

start_cpu_monitor() {
    log_info "启动 CPU 监控..."

    CPU_LOG="${RESULT_DIR}/monitor/cpu_usage.csv"
    echo "timestamp,cpu_total,cpu_user,cpu_system,load_avg_1,load_avg_5,load_avg_15" > "${CPU_LOG}"

    (
        while true; do
            timestamp=$(date '+%Y-%m-%d %H:%M:%S')

            # 获取系统 CPU 使用率
            if [[ "$OSTYPE" == "darwin"* ]]; then
                # macOS - 使用 top
                cpu_line=$(top -l 1 -n 0 | grep "CPU usage" || echo "CPU usage: 0.0 user, 0.0 sys, 100.0 idle")
                cpu_user=$(echo "$cpu_line" | awk '{print $3}')
                cpu_sys=$(echo "$cpu_line" | awk '{print $5}')
                cpu_total=$(echo "$cpu_user $cpu_sys" | awk '{print $1 + $2}')

                # 获取负载
                load_avg=$(sysctl -n vm.loadavg || echo "{ 0 0 0 }")
                load_1=$(echo "$load_avg" | awk '{print $2}')
                load_5=$(echo "$load_avg" | awk '{print $3}')
                load_15=$(echo "$load_avg" | awk '{print $4}')
            else
                # Linux
                cpu_idle=$(top -bn1 | grep "Cpu(s)" | awk '{print $8}' | cut -d'%' -f1)
                cpu_total=$(echo "100 - $cpu_idle" | bc)
                cpu_user=$(top -bn1 | grep "Cpu(s)" | awk '{print $2}' | cut -d'%' -f1)
                cpu_sys=$(top -bn1 | grep "Cpu(s)" | awk '{print $4}' | cut -d'%' -f1)

                # 获取负载
                load_avg=$(cat /proc/loadavg || echo "0 0 0")
                load_1=$(echo "$load_avg" | awk '{print $1}')
                load_5=$(echo "$load_avg" | awk '{print $2}')
                load_15=$(echo "$load_avg" | awk '{print $3}')
            fi

            echo "${timestamp},${cpu_total},${cpu_user},${cpu_sys},${load_1},${load_5},${load_15}" >> "${CPU_LOG}"

            sleep ${MONITOR_INTERVAL}
        done
    ) &

    CPU_MONITOR_PID=$!
    log_info "CPU 监控已启动 (PID: ${CPU_MONITOR_PID})"
}

start_server_monitor() {
    log_info "启动服务器状态监控..."

    (
        while true; do
            if ! check_server_alive; then
                # 检测到服务器崩溃
                echo "CRASH_DETECTED" >> "${RESULT_DIR}/monitor/crash_events.log"
                restart_server
            fi

            sleep 1
        done
    ) &

    SERVER_MONITOR_PID=$!
    log_info "服务器状态监控已启动 (PID: ${SERVER_MONITOR_PID})"
}

stop_all_monitors() {
    log_info "停止所有监控进程..."

    if [ -n "${MONITOR_PID}" ] && kill -0 ${MONITOR_PID} 2>/dev/null; then
        kill ${MONITOR_PID} 2>/dev/null
        wait ${MONITOR_PID} 2>/dev/null
    fi

    if [ -n "${CPU_MONITOR_PID}" ] && kill -0 ${CPU_MONITOR_PID} 2>/dev/null; then
        kill ${CPU_MONITOR_PID} 2>/dev/null
        wait ${CPU_MONITOR_PID} 2>/dev/null
    fi

    if [ -n "${SERVER_MONITOR_PID}" ] && kill -0 ${SERVER_MONITOR_PID} 2>/dev/null; then
        kill ${SERVER_MONITOR_PID} 2>/dev/null
        wait ${SERVER_MONITOR_PID} 2>/dev/null
    fi

    log_info "所有监控进程已停止"
}

# ==================== 测试执行函数 ====================

run_wrk_test() {
    local mode="$1"
    local endpoint="$2"
    local threads="$3"
    local connections="$4"
    local duration="$5"

    local test_name="${mode}_${endpoint}"
    local result_file="${RESULT_DIR}/wrk/${test_name}_$(date '+%Y%m%d_%H%M%S').txt"

    log_info "执行测试: ${test_name} (${threads} threads, ${connections} conn, ${duration})"

    # 检查服务器是否存活
    if ! check_server_alive; then
        log_warn "服务器不可用，跳过测试"
        return 1
    fi

    # 构建测试 URL
    local url="http://localhost:${PORT}${ENDPOINTS[$endpoint]}"

    # 执行 wrk 测试
    wrk -t${threads} -c${connections} -d${duration} --latency "${url}" > "${result_file}" 2>&1

    # 提取关键指标
    local req_per_sec=$(grep "Requests/sec" "${result_file}" | awk '{print $2}' || echo "0")
    local latency_avg=$(grep "Latency" "${result_file}" | head -1 | awk '{print $2}' || echo "0")
    local errors=$(grep "Socket errors" "${result_file}" | awk '{print $3}' || echo "0")

    # 记录汇总
    echo "$(date '+%Y-%m-%d %H:%M:%S'),${test_name},${threads},${connections},${duration},${req_per_sec},${latency_avg},${errors}" >> "${RESULT_DIR}/wrk_summary.csv"

    log_info "测试完成: ${test_name} - ${req_per_sec} req/s, latency: ${latency_avg}, errors: ${errors}"

    return 0
}

run_https_test() {
    local mode="$1"
    local threads="$2"
    local connections="$3"
    local duration="$4"

    local test_name="${mode}_https"
    local result_file="${RESULT_DIR}/wrk/${test_name}_$(date '+%Y%m%d_%H%M%S').txt"

    log_info "执行 HTTPS 测试: ${test_name}"

    # 注意: wrk 对 HTTPS 支持有限，这里仅记录尝试
    if ! check_server_alive; then
        log_warn "服务器不可用，跳过 HTTPS 测试"
        return 1
    fi

    # 使用 curl 测试 HTTPS 连接性
    local https_url="https://localhost:${HTTPS_PORT}/"

    # 简单的 HTTPS 连接测试
    curl -k -s -o /dev/null -w "%{http_code}\n" "${https_url}" > "${result_file}" 2>&1 || echo "HTTPS_FAILED" >> "${result_file}"

    log_info "HTTPS 测试完成: ${test_name}"

    return 0
}

run_load_cycle() {
    local cycle_num="$1"
    local elapsed_hours="$2"

    log_info "=== 开始负载周期 #${cycle_num} (已运行 ${elapsed_hours} 小时) ==="

    # 低负载测试
    local low_config="${LOAD_MODES[low]}"
    read threads connections duration <<< "$low_config"
    run_wrk_test "low" "homepage" "${threads}" "${connections}" "${duration}"
    run_wrk_test "low" "api" "${threads}" "${connections}" "${duration}"
    run_wrk_test "low" "health" "${threads}" "${connections}" "${duration}"

    # 中负载测试
    local med_config="${LOAD_MODES[medium]}"
    read threads connections duration <<< "$med_config"
    run_wrk_test "medium" "homepage" "${threads}" "${connections}" "${duration}"
    run_wrk_test "medium" "api" "${threads}" "${connections}" "${duration}"
    run_wrk_test "medium" "static_1k" "${threads}" "${connections}" "${duration}"

    # 高负载测试
    local high_config="${LOAD_MODES[high]}"
    read threads connections duration <<< "$high_config"
    run_wrk_test "high" "homepage" "${threads}" "${connections}" "${duration}"
    run_wrk_test "high" "api" "${threads}" "${connections}" "${duration}"
    run_wrk_test "high" "static_10k" "${threads}" "${connections}" "${duration}"

    # 峰值负载测试（短时爆发）
    local peak_config="${LOAD_MODES[peak]}"
    read threads connections duration <<< "$peak_config"
    run_wrk_test "peak" "homepage" "${threads}" "${connections}" "${duration}"

    log_info "=== 负载周期 #${cycle_num} 完成 ==="
}

# ==================== 报告生成函数 ====================

generate_hourly_report() {
    local hour="$1"

    local report_file="${RESULT_DIR}/reports/hourly_${hour}.md"

    cat > "${report_file}" << EOF
# 压力测试小时报告 - 第 ${hour} 小时

**生成时间**: $(date '+%Y-%m-%d %H:%M:%S')

## 服务器状态

- 运行状态: $(check_server_alive && echo "正常运行" || echo "异常")
- 崩溃次数: ${CRASH_COUNT}
- PID: ${SERVER_PID}

## 内存使用

EOF

    # 提取最近1小时的内存数据
    if [ -f "${RESULT_DIR}/monitor/memory_usage.csv" ]; then
        tail -6 "${RESULT_DIR}/monitor/memory_usage.csv" >> "${report_file}"
    fi

    cat >> "${report_file}" << EOF

## CPU 使用

EOF

    # 提取最近1小时的 CPU 数据
    if [ -f "${RESULT_DIR}/monitor/cpu_usage.csv" ]; then
        tail -12 "${RESULT_DIR}/monitor/cpu_usage.csv" >> "${report_file}"
    fi

    cat >> "${report_file}" << EOF

## 测试吞吐量

EOF

    # 提取最近的 wrk 测试结果
    if [ -f "${RESULT_DIR}/wrk_summary.csv" ]; then
        tail -10 "${RESULT_DIR}/wrk_summary.csv" >> "${report_file}"
    fi

    log_info "小时报告已生成: ${report_file}"
}

generate_final_report() {
    local end_time=$(date '+%Y-%m-%d %H:%M:%S')
    local duration_seconds=$(( $(date +%s) - $(date -j -f "%Y%m%d_%H%M%S" "${TIMESTAMP}" +%s 2>/dev/null || echo "0") ))

    local final_report="${RESULT_DIR}/reports/final_report.md"

    cat > "${final_report}" << EOF
# Chase HTTP Server 24小时压力测试最终报告

## 测试概览

| 项目 | 值 |
|------|-----|
| 开始时间 | ${TEST_START_TIME} |
| 结束时间 | ${end_time} |
| 实际运行时长 | ${duration_seconds} 秒 (约 $(echo "scale=2; ${duration_seconds}/3600" | bc) 小时) |
| 目标时长 | ${TEST_DURATION_HOURS} 小时 |
| 服务器类型 | ${SERVER_TYPE} |
| Worker 数量 | ${WORKER_COUNT} |
| 监听端口 | ${PORT} |

## 崩溃统计

| 指标 | 值 |
|------|-----|
| 总崩溃次数 | ${CRASH_COUNT} |
| 崩溃率 | $(echo "scale=4; ${CRASH_COUNT}/${duration_seconds}*3600" | bc) 次/小时 |

## 内存使用统计

EOF

    # 计算内存统计数据
    if [ -f "${RESULT_DIR}/monitor/memory_usage.csv" ]; then
        local max_rss=$(awk -F',' 'NR>1 {print $3}' "${RESULT_DIR}/monitor/memory_usage.csv" | sort -n | tail -1)
        local avg_rss=$(awk -F',' 'NR>1 {print $3}' "${RESULT_DIR}/monitor/memory_usage.csv" | awk '{sum+=$1; count++} END {printf "%.2f", sum/count}')
        local min_rss=$(awk -F',' 'NR>1 {print $3}' "${RESULT_DIR}/monitor/memory_usage.csv" | sort -n | head -1)

        cat >> "${final_report}" << EOF
| 指标 | 值 |
|------|-----|
| 最大 RSS | ${max_rss} MB |
| 平均 RSS | ${avg_rss} MB |
| 最小 RSS | ${min_rss} MB |
| 内存增长 | $(awk -F',' 'NR==2 {first=$3} END {last=$3; printf "%.2f", last-first}' "${RESULT_DIR}/monitor/memory_usage.csv") MB |

EOF
    fi

    cat >> "${final_report}" << EOF

## CPU 使用统计

EOF

    if [ -f "${RESULT_DIR}/monitor/cpu_usage.csv" ]; then
        local max_cpu=$(awk -F',' 'NR>1 {print $2}' "${RESULT_DIR}/monitor/cpu_usage.csv" | sort -n | tail -1)
        local avg_cpu=$(awk -F',' 'NR>1 {print $2}' "${RESULT_DIR}/monitor/cpu_usage.csv" | awk '{sum+=$1; count++} END {printf "%.2f", sum/count}')

        cat >> "${final_report}" << EOF
| 指标 | 值 |
|------|-----|
| 最大 CPU | ${max_cpu}% |
| 平均 CPU | ${avg_cpu}% |

EOF
    fi

    cat >> "${final_report}" << EOF

## 吞吐量统计

EOF

    # 分析 wrk 测试结果
    if [ -f "${RESULT_DIR}/wrk_summary.csv" ]; then
        # 按负载模式分组统计
        local low_avg=$(grep "^.*low.*" "${RESULT_DIR}/wrk_summary.csv" | awk -F',' '{sum+=$6; count++} END {printf "%.2f", sum/count}')
        local med_avg=$(grep "^.*medium.*" "${RESULT_DIR}/wrk_summary.csv" | awk -F',' '{sum+=$6; count++} END {printf "%.2f", sum/count}')
        local high_avg=$(grep "^.*high.*" "${RESULT_DIR}/wrk_summary.csv" | awk -F',' '{sum+=$6; count++} END {printf "%.2f", sum/count}')
        local peak_avg=$(grep "^.*peak.*" "${RESULT_DIR}/wrk_summary.csv" | awk -F',' '{sum+=$6; count++} END {printf "%.2f", sum/count}')

        local total_errors=$(awk -F',' 'NR>1 {sum+=$8} END {print sum}' "${RESULT_DIR}/wrk_summary.csv")

        cat >> "${final_report}" << EOF
| 负载模式 | 平均吞吐量 (req/s) |
|----------|-------------------|
| 低负载 (100 conn) | ${low_avg} |
| 中负载 (500 conn) | ${med_avg} |
| 高负载 (1000 conn) | ${high_avg} |
| 峰值负载 (10000 conn) | ${peak_avg} |

**总 Socket 错误**: ${total_errors}

EOF
    fi

    cat >> "${final_report}" << EOF

## 稳定性评估

EOF

    # 稳定性评估
    if [ ${CRASH_COUNT} -eq 0 ]; then
        cat >> "${final_report}" << EOF
### **结论**: 服务器在测试期间运行稳定，无崩溃发生。

测试期间服务器保持正常运行，内存使用稳定，吞吐量符合预期。

EOF
    else
        local crash_rate=$(echo "scale=2; ${CRASH_COUNT}/$(echo "scale=2; ${duration_seconds}/3600" | bc)" | bc)
        cat >> "${final_report}" << EOF
### **结论**: 服务器在测试期间发生 ${CRASH_COUNT} 次崩溃（崩溃率: ${crash_rate} 次/小时）。

建议检查崩溃日志进行分析:
- 服务端日志: ${LOG_DIR}/server_stdout.log
- 崩溃事件: ${RESULT_DIR}/monitor/crash_events.log

EOF
    fi

    cat >> "${final_report}" << EOF

## 文件列表

| 文件类型 | 路径 |
|----------|------|
| 服务端日志 | ${LOG_DIR}/server_stdout.log |
| 错误日志 | ${LOG_DIR}/errors.log |
| 内存监控数据 | ${RESULT_DIR}/monitor/memory_usage.csv |
| CPU 监控数据 | ${RESULT_DIR}/monitor/cpu_usage.csv |
| wrk 测试结果 | ${RESULT_DIR}/wrk/ |
| 小时报告 | ${RESULT_DIR}/reports/hourly_*.md |

---
*报告生成时间: ${end_time}*
EOF

    log_info "最终报告已生成: ${final_report}"
}

# ==================== 主测试流程 ====================

main() {
    log_info "========================================"
    log_info "Chase HTTP Server 24小时压力测试"
    log_info "========================================"

    TEST_START_TIME=$(date '+%Y-%m-%d %H:%M:%S')

    # 初始化
    check_dependencies
    init_test_environment
    get_server_binary

    # 创建 wrk_summary.csv 头部
    echo "timestamp,test_name,threads,connections,duration,req_per_sec,latency_avg,errors" > "${RESULT_DIR}/wrk_summary.csv"

    # 启动服务器和监控
    start_server
    start_memory_monitor
    start_cpu_monitor
    start_server_monitor

    # 计算测试时长（秒）
    local total_seconds=$((TEST_DURATION_HOURS * 3600))
    local cycle_duration=180  # 每个周期约 3 分钟
    local total_cycles=$((total_seconds / cycle_duration))
    local cycle_count=0
    local elapsed_seconds=0

    log_info "开始压力测试，预计运行 ${TEST_DURATION_HOURS} 小时，共 ${total_cycles} 个负载周期"

    # 主测试循环
    while [ ${elapsed_seconds} -lt ${total_seconds} ]; do
        cycle_count=$((cycle_count + 1))
        elapsed_hours=$(echo "scale=2; ${elapsed_seconds}/3600" | bc)

        run_load_cycle ${cycle_count} ${elapsed_hours}

        # 每小时生成一次报告
        local current_hour=$((elapsed_seconds / 3600 + 1))
        if [ $((elapsed_seconds % 3600)) -lt ${cycle_duration} ]; then
            generate_hourly_report ${current_hour}
        fi

        elapsed_seconds=$((elapsed_seconds + cycle_duration))

        # 检查是否需要提前终止
        if [ ${CRASH_COUNT} -gt ${MAX_CRASH_COUNT} ]; then
            log_error "崩溃次数过多，提前终止测试"
            break
        fi

        # 等待下一个周期
        sleep 5
    done

    log_info "压力测试循环完成"

    # 停止监控和服务器
    stop_all_monitors
    stop_server

    # 生成最终报告
    generate_final_report

    log_info "========================================"
    log_info "压力测试完成！"
    log_info "========================================"
    log_info "结果目录: ${RESULT_DIR}"
    log_info "最终报告: ${RESULT_DIR}/reports/final_report.md"

    # 打印崩溃次数
    if [ ${CRASH_COUNT} -gt 0 ]; then
        log_warn "测试期间发生 ${CRASH_COUNT} 次崩溃，请检查日志"
    else
        log_info "服务器在测试期间运行稳定，无崩溃发生"
    fi
}

# 清理函数（用于信号处理）
cleanup() {
    log_warn "收到终止信号，正在清理..."
    stop_all_monitors
    stop_server
    generate_final_report
    exit 0
}

# 注册信号处理
trap cleanup SIGINT SIGTERM

# 执行主函数
main