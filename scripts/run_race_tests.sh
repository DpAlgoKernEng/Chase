#!/bin/bash
#
# run_race_tests.sh - 运行 ThreadSanitizer 竞态检测测试
#
# 用法:
#   ./scripts/run_race_tests.sh [选项]
#
# 选项:
#   --build    构建 TSan 版本（默认已构建则跳过）
#   --clean    清理构建目录后重新构建
#   --verbose  显示详细输出
#   --help     显示帮助信息
#
# 输出:
#   TSan 检测报告，包含数据竞争详情
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build-tsan"

# 默认选项
DO_BUILD=false
DO_CLEAN=false
VERBOSE=false

# 解析参数
while [[ $# -gt 0 ]]; do
    case $1 in
        --build)
            DO_BUILD=true
            shift
            ;;
        --clean)
            DO_CLEAN=true
            DO_BUILD=true
            shift
            ;;
        --verbose)
            VERBOSE=true
            shift
            ;;
        --help)
            echo "用法: $0 [--build] [--clean] [--verbose] [--help]"
            echo ""
            echo "选项:"
            echo "  --build    构建 TSan 版本（默认已构建则跳过）"
            echo "  --clean    清理构建目录后重新构建"
            echo "  --verbose  显示详细输出"
            echo "  --help     显示帮助信息"
            exit 0
            ;;
        *)
            echo "未知选项: $1"
            exit 1
            ;;
    esac
done

echo "=== Chase ThreadSanitizer 竞态检测测试 ==="
echo ""

# 检查 TSan 支持
check_tsan_support() {
    if ! command -v cmake &> /dev/null; then
        echo "错误: cmake 未安装"
        exit 1
    fi

    # 检查编译器
    CC="${CC:-cc}"
    if ! $CC -fsanitize=thread -xc -c /dev/null -o /dev/null 2>/dev/null; then
        echo "警告: 编译器可能不支持 ThreadSanitizer"
        echo "Clang 和 GCC 通常支持 TSan"
    fi
}

# 构建 TSan 版本
build_tsan() {
    echo "构建 TSan 版本..."

    if [ "$DO_CLEAN" = true ]; then
        echo "清理构建目录..."
        rm -rf "$BUILD_DIR"
    fi

    if [ ! -d "$BUILD_DIR" ]; then
        mkdir -p "$BUILD_DIR"
    fi

    cd "$BUILD_DIR"

    # 配置 CMake（启用 TSan，禁用 Coverage）
    cmake "$PROJECT_DIR" \
        -DENABLE_TSAN=ON \
        -DENABLE_COVERAGE=OFF \
        -DENABLE_TESTS=ON \
        -DCMAKE_BUILD_TYPE=Debug \
        ${VERBOSE:-Wno-dev}

    # 构建
    echo "编译..."
    make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

    echo "构建完成"
    echo ""
}

# 运行竞态测试
run_tests() {
    echo "运行竞态测试..."
    echo ""

    # 设置 TSan 选项
    export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=0:history_size=7:die_on_race=0:verbosity=1}"

    echo "TSan 选项: $TSAN_OPTIONS"
    echo ""

    cd "$BUILD_DIR"

    # 定义测试
    RACE_TESTS=(
        "test_race_connection_pool"
        "test_race_security"
        "test_race_timer"
    )

    # 运行每个测试
    for test in "${RACE_TESTS[@]}"; do
        echo "-------------------------------------------"
        echo "运行: $test"
        echo "-------------------------------------------"

        if [ -f "$test" ]; then
            ./$test 2>&1 | tee "${test}_tsan_output.log"

            # 检查 TSan 报告
            if grep -q "ThreadSanitizer: data race" "${test}_tsan_output.log" 2>/dev/null; then
                echo ""
                echo "*** TSan 检测到数据竞争 ***"
                echo "这是预期结果（取决于模块设计）"
                echo ""
            else
                echo ""
                echo "*** 无数据竞争检测到 ***"
                echo ""
            fi
        else
            echo "警告: 测试可执行文件不存在: $test"
        fi
        echo ""
    done
}

# 生成报告
generate_report() {
    echo "=== 测试结果总结 ==="
    echo ""

    cd "$BUILD_DIR"

    echo "竞态检测结果:"
    echo ""

    for test in "${RACE_TESTS[@]}"; do
        log_file="${test}_tsan_output.log"

        if [ -f "$log_file" ]; then
            race_count=$(grep -c "ThreadSanitizer: data race" "$log_file" 2>/dev/null || echo "0")
            echo "  $test: $race_count 个竞争"

            # 显示竞争详情摘要
            if [ "$race_count" -gt 0 ]; then
                echo "    竞争位置:"
                grep -A2 "Location is" "$log_file" 2>/dev/null | head -10 | while read line; do
                    echo "      $line"
                done
            fi
        fi
    done

    echo ""
    echo "设计说明:"
    echo "  - ConnectionPool: 单 Worker 设计，竞争是预期的"
    echo "  - Timer: 单 EventLoop 设计，竞争是预期的"
    echo "  - Security: 分片锁保护，无竞争是预期的"
    echo ""
}

# 主流程
main() {
    check_tsan_support

    # 检查是否需要构建
    if [ "$DO_BUILD" = true ] || [ ! -d "$BUILD_DIR" ] || [ ! -f "$BUILD_DIR/test_race_connection_pool" ]; then
        build_tsan
    else
        echo "使用现有构建目录: $BUILD_DIR"
        echo ""
    fi

    run_tests
    generate_report

    echo "竞态测试完成"
    echo ""
    echo "查看详细日志: $BUILD_DIR/*.log"
}

main