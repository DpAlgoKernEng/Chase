#!/bin/bash
# Valgrind 内存检测脚本
# 用于检测 Chase HTTP Server 库的内存泄漏和内存错误

set -e

# 配置
BUILD_DIR="${BUILD_DIR:-build}"
TEST_DIR="$BUILD_DIR/test/integration/c_core"
VALGRIND_OPTS="--leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=== Chase Valgrind Memory Check ==="
echo ""

# 检查 Valgrind 是否安装
if ! command -v valgrind &> /dev/null; then
    echo -e "${RED}Error: Valgrind not installed.${NC}"
    echo "Install on Linux: sudo apt-get install valgrind"
    echo "Install on macOS: brew install valgrind"
    exit 1
fi

echo -e "${YELLOW}Valgrind version:${NC}"
valgrind --version
echo ""

# 检查构建目录是否存在
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}Building project...${NC}"
    cmake -B "$BUILD_DIR" -S .
    cmake --build "$BUILD_DIR"
fi

# 检查测试目录
if [ ! -d "$TEST_DIR" ]; then
    echo -e "${RED}Error: Test directory not found: $TEST_DIR${NC}"
    exit 1
fi

# 运行 Valgrind 检测每个测试程序
TEST_PASSED=0
TEST_FAILED=0
TEST_TOTAL=0

for test_bin in "$TEST_DIR"/test_*; do
    if [ -f "$test_bin" ] && [ -x "$test_bin" ]; then
        test_name=$(basename "$test_bin")
        TEST_TOTAL=$((TEST_TOTAL + 1))

        echo -e "${YELLOW}Testing: $test_name${NC}"

        # 运行 Valgrind
        if valgrind $VALGRIND_OPTS "$test_bin" > /dev/null 2>&1; then
            echo -e "  ${GREEN}PASS${NC} - No memory leaks detected"
            TEST_PASSED=$((TEST_PASSED + 1))
        else
            echo -e "  ${RED}FAIL${NC} - Memory issues detected"
            echo "  Running with verbose output:"
            valgrind $VALGRIND_OPTS "$test_bin" 2>&1 | head -50
            TEST_FAILED=$((TEST_FAILED + 1))
        fi
        echo ""
    fi
done

# 输出总结
echo "=== Valgrind Test Summary ==="
echo -e "Total tests:  $TEST_TOTAL"
echo -e "${GREEN}Passed:       $TEST_PASSED${NC}"
echo -e "${RED}Failed:       $TEST_FAILED${NC}"
echo ""

# 如果有失败，显示详细报告
if [ $TEST_FAILED -gt 0 ]; then
    echo -e "${RED}Memory issues detected!${NC}"
    echo "Run individual tests with:"
    echo "  valgrind --leak-check=full $TEST_DIR/test_<name>"
    exit 1
fi

echo -e "${GREEN}All memory checks passed!${NC}"
exit 0