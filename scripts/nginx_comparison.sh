#!/bin/bash
#
# @file    nginx_comparison.sh
# @brief   nginx 与 Chase HTTP 服务器对照测试脚本
#
# @details
#          - 自动启动 nginx 和 Chase 服务器
#          - 并行运行 wrk 测试
#          - 收集性能数据并生成报告
#          - 支持 HTTP 和 HTTPS 测试
#
# @layer   Script
#
# @depends wrk, nginx, bc, Chase
# @usedby  开发者
#
# @author  minghui.liu
# @date    2026-04-24
#

set -e

# ============== 配置区域 ==============

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 默认配置
NGINX_PORT=8080
CHASE_PORT=8081
NGINX_HTTPS_PORT=8443
CHASE_HTTPS_PORT=8444
WORKERS=4
CONNECTIONS=100
THREADS=4
DURATION=30
RESULTS_DIR="./comparison_results"
TEST_FILES_DIR="/tmp/chase_test_files"

# 路径配置
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CHASE_BIN="${PROJECT_ROOT}/build/examples/production_server"
NGINX_CONF_HTTP="${PROJECT_ROOT}/config/nginx_http.conf"
NGINX_CONF_HTTPS="${PROJECT_ROOT}/config/nginx_https.conf"

# 测试次数
RUN_COUNT=3

# ============== 工具函数 ==============

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_dependencies() {
    log_info "检查依赖工具..."

    local missing=()

    if ! command -v wrk &> /dev/null; then
        missing+=("wrk")
    fi

    if ! command -v nginx &> /dev/null; then
        missing+=("nginx")
    fi

    if ! command -v bc &> /dev/null; then
        missing+=("bc")
    fi

    if [ ${#missing[@]} -ne 0 ]; then
        log_error "缺少以下工具: ${missing[*]}"
        echo ""
        echo "安装方法:"
        echo "  macOS: brew install wrk nginx bc"
        echo "  Ubuntu: sudo apt install wrk nginx bc"
        echo "  CentOS: sudo yum install wrk nginx bc"
        exit 1
    fi

    log_success "所有依赖工具已安装"
}

create_test_files() {
    log_info "创建测试文件..."

    mkdir -p "${TEST_FILES_DIR}"

    # 小文件 (1KB HTML)
    echo '<!DOCTYPE html><html><head><title>Test</title></head><body><h1>Hello from test!</h1><p>This is a test file for benchmark.</p></body></html>' > "${TEST_FILES_DIR}/index.html"

    # 中等文件 (10KB)
    dd if=/dev/urandom of="${TEST_FILES_DIR}/medium.html" bs=1024 count=10 2>/dev/null

    # 大文件 (1MB)
    dd if=/dev/urandom of="${TEST_FILES_DIR}/large.bin" bs=1024 count=1024 2>/dev/null

    log_success "测试文件创建完成: ${TEST_FILES_DIR}"
}

create_nginx_config() {
    log_info "创建 nginx 配置文件..."

    mkdir -p "${PROJECT_ROOT}/config"

    # HTTP 配置
    cat > "${NGINX_CONF_HTTP}" << 'EOF'
# nginx HTTP 配置 - Chase 对照测试
worker_processes 4;
worker_rlimit_nofile 1024;
error_log /tmp/nginx_error.log warn;
pid /tmp/nginx.pid;

events {
    worker_connections 1024;
    multi_accept on;
}

http {
    include /usr/local/etc/nginx/mime.types;
    default_type application/octet-stream;

    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;
    keepalive_timeout 5;
    keepalive_requests 100;

    access_log /tmp/nginx_access.log;

    server {
        listen 8080 reuseport backlog=1024;
        server_name localhost;

        location = / {
            return 200 '<!DOCTYPE html><html><head><title>Nginx Test</title></head><body><h1>Hello from Nginx!</h1><p>SO_REUSEPORT + Multi-Worker Architecture</p></body></html>';
            add_header Content-Type text/html;
        }

        location = /api {
            return 200 '{"status":"ok","version":"nginx","arch":"multi-worker"}';
            add_header Content-Type application/json;
        }

        location = /health {
            return 200 '{"status":"healthy"}';
            add_header Content-Type application/json;
        }

        location /static/ {
            alias /tmp/chase_test_files/;
        }
    }
}
EOF

    # HTTPS 配置
    cat > "${NGINX_CONF_HTTPS}" << EOF
# nginx HTTPS 配置 - Chase 对照测试
worker_processes 4;
worker_rlimit_nofile 1024;
error_log /tmp/nginx_ssl_error.log warn;
pid /tmp/nginx_ssl.pid;

events {
    worker_connections 1024;
    multi_accept on;
}

http {
    include /usr/local/etc/nginx/mime.types;
    default_type application/octet-stream;

    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;
    keepalive_timeout 5;
    keepalive_requests 100;

    access_log /tmp/nginx_ssl_access.log;

    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_prefer_server_ciphers on;
    ssl_ciphers ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256;
    ssl_session_cache shared:SSL:10m;
    ssl_session_timeout 10m;

    server {
        listen 8443 ssl reuseport backlog=1024;
        server_name localhost;

        ssl_certificate ${PROJECT_ROOT}/test/certs/test.crt;
        ssl_certificate_key ${PROJECT_ROOT}/test/certs/test.key;

        location = /api {
            return 200 '{"status":"ok","version":"nginx-ssl"}';
            add_header Content-Type application/json;
        }

        location = /health {
            return 200 '{"status":"healthy"}';
            add_header Content-Type application/json;
        }
    }
}
EOF

    log_success "nginx 配置文件创建完成"
}

build_chase() {
    log_info "构建 Chase 服务器..."

    if [ ! -f "${CHASE_BIN}" ]; then
        log_info "Chase 可执行文件不存在，开始构建..."
        cd "${PROJECT_ROOT}"
        cmake -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
        cmake --build build > /dev/null
        cd - > /dev/null
    fi

    if [ ! -f "${CHASE_BIN}" ]; then
        log_error "Chase 构建失败"
        exit 1
    fi

    log_success "Chase 服务器已准备就绪: ${CHASE_BIN}"
}

# ============== 服务器管理 ==============

NGINX_PID=""
CHASE_PID=""

start_nginx() {
    local port=$1
    local config=$2

    log_info "启动 nginx (端口: ${port})..."

    # 检查端口是否被占用
    if lsof -i :${port} &> /dev/null; then
        log_warning "端口 ${port} 已被占用，尝试停止..."
        nginx -s stop -c "${config}" 2>/dev/null || true
        sleep 1
    fi

    nginx -c "${config}"
    sleep 2

    # 验证启动成功
    if curl -s "http://localhost:${port}/health" > /dev/null; then
        log_success "nginx 启动成功 (端口: ${port})"
    else
        log_error "nginx 启动失败"
        cat /tmp/nginx_error.log 2>/dev/null || true
        exit 1
    fi
}

start_chase() {
    local port=$1
    local workers=$2

    log_info "启动 Chase (端口: ${port}, Workers: ${workers})..."

    # 检查端口是否被占用
    if lsof -i :${port} &> /dev/null; then
        log_warning "端口 ${port} 已被占用"
        local old_pid=$(lsof -t -i :${port})
        kill -9 ${old_pid} 2>/dev/null || true
        sleep 1
    fi

    "${CHASE_BIN}" "${port}" "${workers}" &
    CHASE_PID=$!
    sleep 2

    # 验证启动成功
    if kill -0 ${CHASE_PID} 2>/dev/null && curl -s "http://localhost:${port}/health" > /dev/null; then
        log_success "Chase 启动成功 (PID: ${CHASE_PID}, 端口: ${port})"
    else
        log_error "Chase 启动失败"
        exit 1
    fi
}

stop_servers() {
    log_info "停止服务器..."

    # 停止 nginx
    nginx -s stop 2>/dev/null || true

    # 停止 Chase
    if [ -n "${CHASE_PID}" ] && kill -0 ${CHASE_PID} 2>/dev/null; then
        kill ${CHASE_PID} 2>/dev/null || true
        wait ${CHASE_PID} 2>/dev/null || true
    fi

    # 清理僵尸进程
    pkill -f "production_server" 2>/dev/null || true
    pkill -f "nginx.*808[0-4]" 2>/dev/null || true

    sleep 1
    log_success "服务器已停止"
}

# ============== 资源监控 ==============

collect_resource_usage() {
    local server_name=$1
    local pid=$2
    local output_file=$3

    if [ -z "${pid}" ] || ! kill -0 ${pid} 2>/dev/null; then
        return
    fi

    local cpu=$(ps -p ${pid} -o %cpu --no-headers 2>/dev/null | head -1 | tr -d ' ')
    local mem=$(ps -p ${pid} -o rss --no-headers 2>/dev/null | head -1 | tr -d ' ')

    # 对于多进程服务器，收集所有 worker
    local total_cpu=${cpu:-0}
    local total_mem=${mem:-0}

    local pids=$(pgrep -P ${pid} 2>/dev/null || true)
    for child_pid in ${pids}; do
        local child_cpu=$(ps -p ${child_pid} -o %cpu --no-headers 2>/dev/null | head -1 | tr -d ' ')
        local child_mem=$(ps -p ${child_pid} -o rss --no-headers 2>/dev/null | head -1 | tr -d ' ')
        total_cpu=$(echo "${total_cpu} + ${child_cpu:-0}" | bc)
        total_mem=$(echo "${total_mem} + ${child_mem:-0}" | bc)
    done

    echo "${server_name},${total_cpu},${total_mem}" >> "${output_file}"
}

# ============== 测试函数 ==============

run_wrk_benchmark() {
    local name=$1
    local url=$2
    local output_dir=$3

    log_info "运行测试: ${name} -> ${url}"

    local result_file="${output_dir}/${name}.txt"

    # 运行 wrk
    wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}s --latency "${url}" > "${result_file}" 2>&1

    # 提取关键指标
    local rps=$(grep "Requests/sec" "${result_file}" | awk '{print $2}')
    local p50=$(grep "50%" "${result_file}" | awk '{print $2}')
    local p90=$(grep "90%" "${result_file}" | awk '{print $2}')
    local p99=$(grep "99%" "${result_file}" | awk '{print $2}')
    local avg=$(grep "Latency" "${result_file}" | head -1 | awk '{print $2}')

    echo "  RPS: ${rps}, P50: ${p50}, P90: ${p90}, P99: ${p99}"

    # 返回 RPS 值用于计算平均值
    echo "${rps}"
}

run_http_tests() {
    local test_name=$1
    local nginx_port=$2
    local chase_port=$3
    local output_dir=$4

    log_info "========== HTTP 测试: ${test_name} =========="

    mkdir -p "${output_dir}"

    local nginx_results=()
    local chase_results=()

    # 运行多次测试
    for i in $(seq 1 ${RUN_COUNT}); do
        log_info "--- 第 ${i}/${RUN_COUNT} 次测试 ---"

        # nginx 测试
        start_nginx "${nginx_port}" "${NGINX_CONF_HTTP}"
        sleep 1
        local nginx_rps=$(run_wrk_benchmark "nginx_run${i}" "http://localhost:${nginx_port}/api" "${output_dir}")
        nginx_results+=("${nginx_rps}")

        # 资源监控
        local nginx_master_pid=$(pgrep -f "nginx.*master" | head -1)
        collect_resource_usage "nginx" "${nginx_master_pid}" "${output_dir}/resources.csv"

        stop_servers
        sleep 2

        # Chase 测试
        start_chase "${chase_port}" "${WORKERS}"
        sleep 1
        local chase_rps=$(run_wrk_benchmark "chase_run${i}" "http://localhost:${chase_port}/api" "${output_dir}")
        chase_results+=("${chase_rps}")

        # 资源监控
        collect_resource_usage "chase" "${CHASE_PID}" "${output_dir}/resources.csv"

        stop_servers
        sleep 2
    done

    # 计算平均值
    local nginx_avg=0
    local chase_avg=0

    for rps in "${nginx_results[@]}"; do
        nginx_avg=$(echo "${nginx_avg} + ${rps}" | bc)
    done
    nginx_avg=$(echo "scale=2; ${nginx_avg} / ${RUN_COUNT}" | bc)

    for rps in "${chase_results[@]}"; do
        chase_avg=$(echo "scale=2; ${chase_avg} + ${rps}" | bc)
    done
    chase_avg=$(echo "scale=2; ${chase_avg} / ${RUN_COUNT}" | bc)

    log_success "HTTP 测试完成"
    log_info "nginx 平均 RPS: ${nginx_avg}"
    log_info "Chase 平均 RPS: ${chase_avg}"

    # 保存汇总结果
    echo "${test_name},nginx,${nginx_avg}" >> "${output_dir}/summary.csv"
    echo "${test_name},chase,${chase_avg}" >> "${output_dir}/summary.csv"
}

run_https_tests() {
    local test_name=$1
    local nginx_port=$2
    local chase_port=$3
    local output_dir=$4

    log_info "========== HTTPS 测试: ${test_name} =========="

    mkdir -p "${output_dir}"

    # nginx HTTPS 测试
    log_info "--- nginx HTTPS 测试 ---"
    start_nginx "${nginx_port}" "${NGINX_CONF_HTTPS}"
    sleep 2

    # 注意: wrk 需要 --insecure 选项来忽略自签名证书
    wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}s --latency "https://localhost:${nginx_port}/api" \
        2>&1 | tee "${output_dir}/nginx_https.txt" || true

    stop_servers
    sleep 2

    # Chase HTTPS 测试（如果支持）
    log_info "--- Chase HTTPS 测试 ---"
    log_warning "Chase HTTPS 测试需要 SSL 支持配置"

    # TODO: 添加 Chase HTTPS 测试
    # start_chase_ssl "${chase_port}" "${WORKERS}"
    # wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}s --latency "https://localhost:${chase_port}/api"

    log_success "HTTPS 测试完成"
}

run_high_concurrency_tests() {
    local test_name=$1
    local nginx_port=$2
    local chase_port=$3
    local output_dir=$4

    log_info "========== 高并发测试: ${test_name} =========="

    local high_connections=1000
    local high_threads=8
    local high_duration=60

    mkdir -p "${output_dir}"

    # nginx 高并发测试
    log_info "--- nginx 高并发测试 (${high_connections} 连接) ---"
    start_nginx "${nginx_port}" "${NGINX_CONF_HTTP}"
    sleep 2

    wrk -t${high_threads} -c${high_connections} -d${high_duration}s --latency \
        "http://localhost:${nginx_port}/api" 2>&1 | tee "${output_dir}/nginx_high_concurrency.txt" || true

    stop_servers
    sleep 3

    # Chase 高并发测试
    log_info "--- Chase 高并发测试 (${high_connections} 连接) ---"
    start_chase "${chase_port}" "${WORKERS}"
    sleep 2

    wrk -t${high_threads} -c${high_connections} -d${high_duration}s --latency \
        "http://localhost:${chase_port}/api" 2>&1 | tee "${output_dir}/chase_high_concurrency.txt" || true

    stop_servers

    log_success "高并发测试完成"
}

# ============== 报告生成 ==============

generate_report() {
    local output_dir=$1
    local report_file="${output_dir}/comparison_report.md"

    log_info "生成报告: ${report_file}"

    cat > "${report_file}" << EOF
# Nginx vs Chase 性能对照报告

**测试日期**: $(date '+%Y-%m-%d %H:%M:%S')
**测试环境**: $(uname -a)
**Workers**: ${WORKERS}
**连接数**: ${CONNECTIONS}
**测试时长**: ${DURATION}s
**测试次数**: ${RUN_COUNT}

---

## 1. HTTP 性能对比

EOF

    # 解析测试结果
    if [ -f "${output_dir}/http/nginx_run1.txt" ]; then
        cat >> "${report_file}" << 'EOF'
### 1.1 吞吐量对比

| 指标 | nginx | Chase | 差异 |
|------|-------|-------|------|
EOF

        # 提取 nginx 结果
        local nginx_rps=$(grep "Requests/sec" "${output_dir}/http/nginx_run1.txt" 2>/dev/null | awk '{print $2}' || echo "N/A")
        local nginx_p50=$(grep "50%" "${output_dir}/http/nginx_run1.txt" 2>/dev/null | awk '{print $2}' || echo "N/A")
        local nginx_p99=$(grep "99%" "${output_dir}/http/nginx_run1.txt" 2>/dev/null | awk '{print $2}' || echo "N/A")

        # 提取 Chase 结果
        local chase_rps=$(grep "Requests/sec" "${output_dir}/http/chase_run1.txt" 2>/dev/null | awk '{print $2}' || echo "N/A")
        local chase_p50=$(grep "50%" "${output_dir}/http/chase_run1.txt" 2>/dev/null | awk '{print $2}' || echo "N/A")
        local chase_p99=$(grep "99%" "${output_dir}/http/chase_run1.txt" 2>/dev/null | awk '{print $2}' || echo "N/A")

        echo "| RPS | ${nginx_rps} | ${chase_rps} | - |" >> "${report_file}"
        echo "| P50 延迟 | ${nginx_p50} | ${chase_p50} | - |" >> "${report_file}"
        echo "| P99 延迟 | ${nginx_p99} | ${chase_p99} | - |" >> "${report_file}"
    fi

    # 添加原始测试结果
    cat >> "${report_file}" << EOF

### 1.2 原始测试结果

#### nginx HTTP 测试结果

\`\`\`
EOF

    if [ -f "${output_dir}/http/nginx_run1.txt" ]; then
        cat "${output_dir}/http/nginx_run1.txt" >> "${report_file}"
    else
        echo "无测试数据" >> "${report_file}"
    fi

    echo -e "\n\`\`\`" >> "${report_file}"

    echo -e "\n#### Chase HTTP 测试结果\n" >> "${report_file}"
    echo '```' >> "${report_file}"

    if [ -f "${output_dir}/http/chase_run1.txt" ]; then
        cat "${output_dir}/http/chase_run1.txt" >> "${report_file}"
    else
        echo "无测试数据" >> "${report_file}"
    fi

    echo -e "\n\`\`\`" >> "${report_file}"

    # 添加高并发测试结果
    if [ -f "${output_dir}/high_concurrency/nginx_high_concurrency.txt" ]; then
        cat >> "${report_file}" << EOF

---

## 2. 高并发测试 (${high_connections:-1000} 连接)

### 2.1 nginx 高并发结果

\`\`\`
EOF
        cat "${output_dir}/high_concurrency/nginx_high_concurrency.txt" >> "${report_file}"
        echo -e "\n\`\`\`" >> "${report_file}"

        cat >> "${report_file}" << EOF

### 2.2 Chase 高并发结果

\`\`\`
EOF
        cat "${output_dir}/high_concurrency/chase_high_concurrency.txt" >> "${report_file}"
        echo -e "\n\`\`\`" >> "${report_file}"
    fi

    # 添加资源使用
    if [ -f "${output_dir}/http/resources.csv" ]; then
        cat >> "${report_file}" << EOF

---

## 3. 资源使用对比

EOF
        echo '```csv' >> "${report_file}"
        echo "server,cpu_percent,memory_kb" >> "${report_file}"
        cat "${output_dir}/http/resources.csv" >> "${report_file}"
        echo '```' >> "${report_file}"
    fi

    # 添加结论模板
    cat >> "${report_file}" << EOF

---

## 4. 分析与结论

### 4.1 性能总结

- **HTTP 吞吐量**:
- **延迟对比**:
- **资源效率**:

### 4.2 优势分析

#### nginx 优势
-

#### Chase 优势
-

### 4.3 待优化点

1.
2.

### 4.4 建议

1.
2.

---

*报告生成时间: $(date '+%Y-%m-%d %H:%M:%S')*
EOF

    log_success "报告已生成: ${report_file}"
}

# ============== 主函数 ==============

print_usage() {
    echo "用法: $0 [选项] [命令]"
    echo ""
    echo "命令:"
    echo "  all          运行所有测试（默认）"
    echo "  http         仅运行 HTTP 测试"
    echo "  https        仅运行 HTTPS 测试"
    echo "  high         仅运行高并发测试"
    echo "  report       仅生成报告"
    echo "  clean        清理测试结果"
    echo ""
    echo "选项:"
    echo "  -w WORKERS   Worker 数量 (默认: 4)"
    echo "  -c CONNS     连接数 (默认: 100)"
    echo "  -t THREADS   线程数 (默认: 4)"
    echo "  -d SECONDS   测试时长 (默认: 30)"
    echo "  -r COUNT     测试次数 (默认: 3)"
    echo "  -o DIR       结果目录 (默认: ./comparison_results)"
    echo "  -h           显示帮助"
    echo ""
    echo "示例:"
    echo "  $0 all                    # 运行所有测试"
    echo "  $0 -w 8 -c 200 http       # 8 Workers, 200 连接 HTTP 测试"
    echo "  $0 report                 # 仅生成报告"
}

cleanup() {
    log_info "清理资源..."
    stop_servers
    rm -f /tmp/nginx*.log /tmp/nginx*.pid 2>/dev/null || true
}

main() {
    local command="all"

    # 解析参数
    while getopts "w:c:t:d:r:o:h" opt; do
        case ${opt} in
            w) WORKERS="${OPTARG}" ;;
            c) CONNECTIONS="${OPTARG}" ;;
            t) THREADS="${OPTARG}" ;;
            d) DURATION="${OPTARG}" ;;
            r) RUN_COUNT="${OPTARG}" ;;
            o) RESULTS_DIR="${OPTARG}" ;;
            h) print_usage; exit 0 ;;
            *) print_usage; exit 1 ;;
        esac
    done
    shift $((OPTIND-1))

    if [ $# -gt 0 ]; then
        command=$1
    fi

    # 设置退出清理
    trap cleanup EXIT

    # 创建结果目录
    mkdir -p "${RESULTS_DIR}"
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local output_dir="${RESULTS_DIR}/${timestamp}"
    mkdir -p "${output_dir}"

    log_info "========== Nginx vs Chase 对照测试 =========="
    log_info "Workers: ${WORKERS}, Connections: ${CONNECTIONS}, Threads: ${THREADS}"
    log_info "Duration: ${DURATION}s, Runs: ${RUN_COUNT}"
    log_info "结果目录: ${output_dir}"
    echo ""

    case ${command} in
        all)
            check_dependencies
            create_test_files
            create_nginx_config
            build_chase
            run_http_tests "http" "${NGINX_PORT}" "${CHASE_PORT}" "${output_dir}/http"
            run_high_concurrency_tests "high_concurrency" "${NGINX_PORT}" "${CHASE_PORT}" "${output_dir}/high_concurrency"
            generate_report "${output_dir}"
            ;;
        http)
            check_dependencies
            create_test_files
            create_nginx_config
            build_chase
            run_http_tests "http" "${NGINX_PORT}" "${CHASE_PORT}" "${output_dir}/http"
            generate_report "${output_dir}"
            ;;
        https)
            check_dependencies
            create_test_files
            create_nginx_config
            run_https_tests "https" "${NGINX_HTTPS_PORT}" "${CHASE_HTTPS_PORT}" "${output_dir}/https"
            generate_report "${output_dir}"
            ;;
        high)
            check_dependencies
            create_test_files
            create_nginx_config
            build_chase
            run_high_concurrency_tests "high_concurrency" "${NGINX_PORT}" "${CHASE_PORT}" "${output_dir}/high_concurrency"
            generate_report "${output_dir}"
            ;;
        report)
            generate_report "${output_dir}"
            ;;
        clean)
            log_info "清理测试结果..."
            rm -rf "${RESULTS_DIR}"
            rm -rf "${TEST_FILES_DIR}"
            rm -f /tmp/nginx*.log /tmp/nginx*.pid
            log_success "清理完成"
            ;;
        *)
            print_usage
            exit 1
            ;;
    esac

    log_success "测试完成! 结果保存在: ${output_dir}"
}

main "$@"