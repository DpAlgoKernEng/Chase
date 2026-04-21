#!/bin/bash
#
# @file    generate_coverage.sh
# @brief   代码覆盖率报告生成脚本
#
# @details
#          - macOS: 使用 LLVM coverage tools (llvm-profdata, llvm-cov)
#          - Linux: 使用 lcov + genhtml
#          - 生成 HTML 和文本格式报告
#          - 需要先启用 ENABLE_COVERAGE=ON 构建并运行测试
#
# @layer   Script
#
# @depends cmake, 测试框架
# @usedby  开发者
#
# @author  minghui.liu
# @date    2026-04-21
#

# Coverage report generation script
# Usage: ./scripts/generate_coverage.sh

set -e

BUILD_DIR="${1:-build}"
OUTPUT_DIR="${BUILD_DIR}/coverage_report"

echo "=== Generating Coverage Report ==="

if [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS: Use LLVM coverage tools
    echo "Using LLVM coverage (macOS)"

    # Check if tools are available
    if ! command -v llvm-profdata &> /dev/null; then
        echo "Error: llvm-profdata not found"
        echo "Install: xcode-select --install or use Xcode"
        exit 1
    fi

    if ! command -v llvm-cov &> /dev/null; then
        echo "Error: llvm-cov not found"
        echo "Install: xcode-select --install or use Xcode"
        exit 1
    fi

    # Find all profraw files
    PROFILES=$(find "${BUILD_DIR}" -name "*.profraw" -type f)

    if [ -z "$PROFILES" ]; then
        echo "No .profraw files found. Run tests first:"
        echo "  cmake -B build -DENABLE_COVERAGE=ON"
        echo "  cmake --build build"
        echo "  cd build && ctest"
        exit 1
    fi

    # Merge coverage data
    echo "Merging coverage data..."
    llvm-profdata merge -sparse ${PROFILES} -o "${BUILD_DIR}/coverage.profdata"

    # Find all test executables
    TEST_EXES=$(ls "${BUILD_DIR}/test/test_*" 2>/dev/null | grep -v ".o")

    if [ -z "$TEST_EXES" ]; then
        echo "No test executables found"
        exit 1
    fi

    # Generate text report
    echo "Generating text report..."
    llvm-cov report ${TEST_EXES} -instr-profile="${BUILD_DIR}/coverage.profdata" \
        > "${BUILD_DIR}/coverage_text.txt"

    # Generate HTML report
    echo "Generating HTML report..."
    mkdir -p "${OUTPUT_DIR}"
    llvm-cov show ${TEST_EXES} -instr-profile="${BUILD_DIR}/coverage.profdata" \
        -format=html -output-dir="${OUTPUT_DIR}"

    # Show summary
    echo ""
    echo "=== Coverage Summary ==="
    grep "TOTAL" "${BUILD_DIR}/coverage_text.txt" || head -20 "${BUILD_DIR}/coverage_text.txt"

    echo ""
    echo "HTML report: ${OUTPUT_DIR}/index.html"
    echo "Text report: ${BUILD_DIR}/coverage_text.txt"

elif [[ "$OSTYPE" == "linux"* ]]; then
    # Linux: Use lcov + genhtml
    echo "Using lcov (Linux)"

    # Check if tools are available
    if ! command -v lcov &> /dev/null; then
        echo "Error: lcov not found"
        echo "Install: sudo apt-get install lcov"
        exit 1
    fi

    if ! command -v genhtml &> /dev/null; then
        echo "Error: genhtml not found"
        echo "Install: sudo apt-get install lcov"
        exit 1
    fi

    # Capture coverage data
    echo "Capturing coverage data..."
    lcov --capture --directory "${BUILD_DIR}" --output-file "${BUILD_DIR}/coverage.info"

    # Remove unwanted files from coverage
    echo "Filtering coverage data..."
    lcov --remove "${BUILD_DIR}/coverage.info" \
        "*/test/*" \
        "*/examples/*" \
        "/usr/*" \
        --output-file "${BUILD_DIR}/coverage.info.cleaned"

    # Generate HTML report
    echo "Generating HTML report..."
    mkdir -p "${OUTPUT_DIR}"
    genhtml "${BUILD_DIR}/coverage.info.cleaned" \
        --output-directory "${OUTPUT_DIR}" \
        --title "Chase Coverage Report" \
        --legend --show-details

    # Show summary
    echo ""
    echo "=== Coverage Summary ==="
    lcov --summary "${BUILD_DIR}/coverage.info.cleaned"

    echo ""
    echo "HTML report: ${OUTPUT_DIR}/index.html"

else
    echo "Unsupported OS: $OSTYPE"
    exit 1
fi

echo ""
echo "=== Coverage Report Generated ==="