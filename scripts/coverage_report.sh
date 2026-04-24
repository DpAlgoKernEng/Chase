#!/bin/bash
#
# @file    coverage_report.sh
# @brief   代码覆盖率报告生成脚本
#
# @details
#          - macOS: 使用 LLVM coverage tools (llvm-profdata, llvm-cov)
#          - Linux: 使用 lcov + genhtml
#          - 生成 HTML 和文本格式报告
#          - 需要先启用 ENABLE_COVERAGE=ON 构建并运行测试
#          - 支持覆盖率阈值检查
#
# @layer   Script
#
# @depends cmake, 测试框架
# @usedby  开发者, CI
#
# @author  minghui.liu
# @date    2026-04-24
#

set -e

# 默认配置
BUILD_DIR="${1:-build}"
OUTPUT_DIR="${BUILD_DIR}/coverage"
COVERAGE_THRESHOLD="${COVERAGE_THRESHOLD:-80}"
SOURCE_DIRS="${SOURCE_DIRS:-src,include}"
VERBOSE="${VERBOSE:-false}"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_header() {
    echo -e "${BLUE}=== $1 ===${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

# 检查覆盖率阈值
check_threshold() {
    local coverage=$1
    local threshold=$2

    if (( $(echo "$coverage >= $threshold" | bc -l) )); then
        print_success "Coverage ${coverage}% meets threshold ${threshold}%"
        return 0
    else
        print_error "Coverage ${coverage}% below threshold ${threshold}%"
        return 1
    fi
}

# 提取覆盖率百分比
extract_coverage_percent() {
    local report_file=$1

    if [[ "$OSTYPE" == "darwin"* ]]; then
        # macOS: 从 llvm-cov 报告提取
        grep "TOTAL" "$report_file" | awk '{print $NF}' | sed 's/%//'
    else
        # Linux: 从 lcov 报告提取
        grep "lines" "$report_file" | grep -oP '\d+\.\d+%' | head -1 | sed 's/%//'
    fi
}

# macOS 覆盖率处理
generate_coverage_macos() {
    print_header "Generating Coverage Report (macOS/LLVM)"

    # 检查工具
    if ! command -v llvm-profdata &> /dev/null; then
        print_error "llvm-profdata not found"
        echo "Install: xcode-select --install or use Xcode"
        exit 1
    fi

    if ! command -v llvm-cov &> /dev/null; then
        print_error "llvm-cov not found"
        echo "Install: xcode-select --install or use Xcode"
        exit 1
    fi

    # 查找 profraw 文件
    print_header "Searching for coverage data files"
    PROFILES=$(find "${BUILD_DIR}" -name "*.profraw" -type f 2>/dev/null)

    if [ -z "$PROFILES" ]; then
        print_error "No .profraw files found"
        echo ""
        echo "Run tests with coverage enabled first:"
        echo "  cmake -B build -DENABLE_COVERAGE=ON"
        echo "  cmake --build build"
        echo "  cd build && ctest"
        exit 1
    fi

    if [ "$VERBOSE" = "true" ]; then
        echo "Found profraw files:"
        echo "$PROFILES"
    fi

    # 合并覆盖率数据
    print_header "Merging coverage data"
    llvm-profdata merge -sparse ${PROFILES} -o "${BUILD_DIR}/coverage.profdata"
    print_success "Merged coverage data to coverage.profdata"

    # 查找测试可执行文件
    TEST_EXES=$(find "${BUILD_DIR}/test" -name "test_*" -type f -perm -111 2>/dev/null | head -50)

    if [ -z "$TEST_EXES" ]; then
        print_error "No test executables found in ${BUILD_DIR}/test"
        exit 1
    fi

    if [ "$VERBOSE" = "true" ]; then
        echo "Found test executables:"
        echo "$TEST_EXES"
    fi

    # 构建源文件路径参数
    IFS=',' read -ra DIRS <<< "$SOURCE_DIRS"
    SOURCE_ARGS=""
    for dir in "${DIRS[@]}"; do
        if [ -d "$dir" ]; then
            SOURCE_ARGS="$SOURCE_ARGS --source-dir=$dir"
        fi
    done

    # 生成文本报告
    print_header "Generating text report"
    llvm-cov report ${TEST_EXES} \
        -instr-profile="${BUILD_DIR}/coverage.profdata" \
        $SOURCE_ARGS \
        > "${BUILD_DIR}/coverage_text.txt"
    print_success "Text report: ${BUILD_DIR}/coverage_text.txt"

    # 生成 HTML 报告
    print_header "Generating HTML report"
    mkdir -p "${OUTPUT_DIR}"
    llvm-cov show ${TEST_EXES} \
        -instr-profile="${BUILD_DIR}/coverage.profdata" \
        -format=html \
        -output-dir="${OUTPUT_DIR}" \
        $SOURCE_ARGS \
        -show-line-counts-or-regions \
        -show-expansions \
        -show-instantiations
    print_success "HTML report: ${OUTPUT_DIR}/index.html"

    # 显示摘要
    echo ""
    print_header "Coverage Summary"
    cat "${BUILD_DIR}/coverage_text.txt" | grep -E "^(Filename|TOTAL|---)" || head -30 "${BUILD_DIR}/coverage_text.txt"

    # 提取并检查覆盖率阈值
    COVERAGE_PERCENT=$(extract_coverage_percent "${BUILD_DIR}/coverage_text.txt")

    echo ""
    echo "Total Line Coverage: ${COVERAGE_PERCENT}%"

    # 检查阈值
    if [ -n "$COVERAGE_PERCENT" ]; then
        check_threshold "$COVERAGE_PERCENT" "$COVERAGE_THRESHOLD"
        return $?
    fi
}

# Linux 覆盖率处理
generate_coverage_linux() {
    print_header "Generating Coverage Report (Linux/lcov)"

    # 检查工具
    if ! command -v lcov &> /dev/null; then
        print_error "lcov not found"
        echo "Install: sudo apt-get install lcov"
        exit 1
    fi

    if ! command -v genhtml &> /dev/null; then
        print_error "genhtml not found"
        echo "Install: sudo apt-get install lcov"
        exit 1
    fi

    # 初始化覆盖率数据
    print_header "Initializing coverage data"
    lcov --zerocounters --directory "${BUILD_DIR}" 2>/dev/null || true

    # 捕获覆盖率数据
    print_header "Capturing coverage data"
    lcov --capture \
        --directory "${BUILD_DIR}" \
        --output-file "${BUILD_DIR}/coverage.info" \
        --ignore-errors source \
        --rc lcov_branch_coverage=1

    if [ ! -f "${BUILD_DIR}/coverage.info" ]; then
        print_error "Failed to capture coverage data"
        exit 1
    fi

    # 过滤不需要的文件
    print_header "Filtering coverage data"

    # 构建排除模式
    EXCLUDE_PATTERNS=(
        "*/test/*"
        "*/tests/*"
        "*/examples/*"
        "/usr/*"
        "*/build/*"
        "*_test.c"
        "*_test.cpp"
        "*/vcpkg_installed/*"
    )

    LCOV_REMOVE_ARGS="${BUILD_DIR}/coverage.info"
    for pattern in "${EXCLUDE_PATTERNS[@]}"; do
        lcov --remove "${LCOV_REMOVE_ARGS}" "$pattern" \
            --output-file "${BUILD_DIR}/coverage.info.tmp" \
            --rc lcov_branch_coverage=1 2>/dev/null || true
        mv "${BUILD_DIR}/coverage.info.tmp" "${BUILD_DIR}/coverage.info"
        LCOV_REMOVE_ARGS="${BUILD_DIR}/coverage.info"
    done

    print_success "Filtered coverage data"

    # 生成 HTML 报告
    print_header "Generating HTML report"
    mkdir -p "${OUTPUT_DIR}"
    genhtml "${BUILD_DIR}/coverage.info" \
        --output-directory "${OUTPUT_DIR}" \
        --title "Chase HTTP Server - Coverage Report" \
        --legend \
        --show-details \
        --branch-coverage \
        --rc lcov_branch_coverage=1 \
        --highlight

    print_success "HTML report: ${OUTPUT_DIR}/index.html"

    # 显示摘要
    echo ""
    print_header "Coverage Summary"
    lcov --summary "${BUILD_DIR}/coverage.info" --rc lcov_branch_coverage=1

    # 提取行覆盖率
    COVERAGE_PERCENT=$(lcov --summary "${BUILD_DIR}/coverage.info" 2>&1 | grep "lines" | grep -oP '\d+\.\d+' | head -1)

    echo ""
    echo "Total Line Coverage: ${COVERAGE_PERCENT}%"

    # 检查阈值
    if [ -n "$COVERAGE_PERCENT" ]; then
        check_threshold "$COVERAGE_PERCENT" "$COVERAGE_THRESHOLD"
        return $?
    fi
}

# 清理覆盖率数据
clean_coverage() {
    print_header "Cleaning coverage data"

    # 清理 profraw 文件 (macOS)
    find "${BUILD_DIR}" -name "*.profraw" -type f -delete 2>/dev/null || true

    # 清理 profdata 文件 (macOS)
    rm -f "${BUILD_DIR}/coverage.profdata" 2>/dev/null || true

    # 清理 info 文件 (Linux)
    rm -f "${BUILD_DIR}/coverage.info" "${BUILD_DIR}/coverage.info.cleaned" 2>/dev/null || true

    # 清理报告目录
    rm -rf "${OUTPUT_DIR}" 2>/dev/null || true
    rm -f "${BUILD_DIR}/coverage_text.txt" 2>/dev/null || true

    print_success "Coverage data cleaned"
}

# 显示帮助
show_help() {
    cat << EOF
Usage: $0 [OPTIONS] [BUILD_DIR]

Generate code coverage report for Chase HTTP Server.

Arguments:
    BUILD_DIR       Build directory (default: build)

Options:
    -h, --help      Show this help message
    -c, --clean     Clean coverage data before generating
    -v, --verbose   Enable verbose output
    -t, --threshold Set coverage threshold (default: 80)

Environment Variables:
    COVERAGE_THRESHOLD  Coverage threshold percentage (default: 80)
    SOURCE_DIRS         Comma-separated source directories (default: src,include)
    VERBOSE             Enable verbose output (default: false)

Examples:
    $0                          # Generate report from build/
    $0 build-debug              # Generate report from build-debug/
    $0 -c build                 # Clean and generate report
    COVERAGE_THRESHOLD=90 $0    # Set threshold to 90%

Requirements:
    macOS:  Xcode Command Line Tools (llvm-profdata, llvm-cov)
    Linux:  lcov, genhtml

Build with coverage:
    cmake -B build -DENABLE_COVERAGE=ON
    cmake --build build
    cd build && ctest
    $0 build
EOF
}

# 解析参数
CLEAN=false
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -c|--clean)
            CLEAN=true
            shift
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -t|--threshold)
            COVERAGE_THRESHOLD="$2"
            shift 2
            ;;
        *)
            BUILD_DIR="$1"
            shift
            ;;
    esac
done

# 主程序
main() {
    echo ""
    echo "========================================"
    echo "  Chase HTTP Server - Coverage Report  "
    echo "========================================"
    echo ""
    echo "Build Directory: ${BUILD_DIR}"
    echo "Output Directory: ${OUTPUT_DIR}"
    echo "Coverage Threshold: ${COVERAGE_THRESHOLD}%"
    echo "Platform: ${OSTYPE}"
    echo ""

    # 检查构建目录
    if [ ! -d "${BUILD_DIR}" ]; then
        print_error "Build directory not found: ${BUILD_DIR}"
        echo "Please build the project first with ENABLE_COVERAGE=ON"
        exit 1
    fi

    # 清理旧数据
    if [ "$CLEAN" = true ]; then
        clean_coverage
    fi

    # 根据平台生成报告
    if [[ "$OSTYPE" == "darwin"* ]]; then
        generate_coverage_macos
    elif [[ "$OSTYPE" == "linux"* ]]; then
        generate_coverage_linux
    else
        print_error "Unsupported OS: $OSTYPE"
        exit 1
    fi

    RESULT=$?

    echo ""
    print_header "Report Locations"
    echo "  HTML:  ${OUTPUT_DIR}/index.html"
    if [[ "$OSTYPE" == "darwin"* ]]; then
        echo "  Text:  ${BUILD_DIR}/coverage_text.txt"
    else
        echo "  Info:  ${BUILD_DIR}/coverage.info"
    fi
    echo ""

    if [ $RESULT -eq 0 ]; then
        print_success "Coverage report generated successfully!"
    else
        print_warning "Coverage below threshold - check report for details"
    fi

    exit $RESULT
}

main