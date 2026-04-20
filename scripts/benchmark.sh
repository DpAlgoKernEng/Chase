#!/bin/bash
# Performance benchmark script using wrk
# Usage: ./scripts/benchmark.sh [port] [server_type]
# server_type: "minimal" (single-threaded) or "threaded" (multi-threaded)

set -e

PORT="${1:-9090}"
SERVER_TYPE="${2:-threaded}"

if [ "${SERVER_TYPE}" == "minimal" ]; then
    SERVER_EXE="./build/examples/minimal_server"
    WORKER_COUNT=1
else
    SERVER_EXE="./build/examples/threaded_server"
    WORKER_COUNT=4
fi

RESULTS_DIR="./benchmark_results"

echo "=== Chase HTTP Server Benchmark ==="

# Check if wrk is available
if ! command -v wrk &> /dev/null; then
    echo "Error: wrk not found"
    echo "Install:"
    echo "  macOS: brew install wrk"
    echo "  Linux: see https://github.com/wg/wrk"
    exit 1
fi

# Check if server exists
if [ ! -f "${SERVER_EXE}" ]; then
    echo "Error: Server executable not found: ${SERVER_EXE}"
    echo "Build first: cmake --build build"
    exit 1
fi

# Create results directory
mkdir -p "${RESULTS_DIR}"

# Start server
echo "Starting ${SERVER_TYPE} server on port ${PORT}..."
if [ "${SERVER_TYPE}" == "threaded" ]; then
    "${SERVER_EXE}" "${PORT}" "${WORKER_COUNT}" &
else
    "${SERVER_EXE}" "${PORT}" &
fi
SERVER_PID=$!
sleep 2  # Wait for server to start

# Verify server is running
if ! kill -0 ${SERVER_PID} 2>/dev/null; then
    echo "Error: Server failed to start"
    exit 1
fi

echo "Server PID: ${SERVER_PID}"
echo ""

# Benchmark configurations
declare -a CONFIGS=(
    "1 10 10s"    # 1 thread, 10 connections, 10 seconds (Phase 1 target)
    "4 100 30s"   # 4 threads, 100 connections, 30 seconds (Phase 2 target)
    "4 1000 60s"  # 4 threads, 1000 connections, 60 seconds (high concurrency)
)

TIMESTAMP=$(date +%Y%m%d_%H%M%S)

for CONFIG in "${CONFIGS[@]}"; do
    read THREADS CONNECTIONS DURATION <<< "$CONFIG"

    echo "=== Benchmark: ${THREADS} threads, ${CONNECTIONS} connections, ${DURATION} ==="

    RESULT_FILE="${RESULTS_DIR}/benchmark_${THREADS}t_${CONNECTIONS}c_${TIMESTAMP}.txt"

    wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION} --latency \
        http://localhost:${PORT}/ > "${RESULT_FILE}" 2>&1

    echo "Results saved to: ${RESULT_FILE}"
    echo ""

    # Extract key metrics
    REQ_PER_SEC=$(grep "Requests/sec" "${RESULT_FILE}" | awk '{print $2}')
    echo "Throughput: ${REQ_PER_SEC} req/s"

    # Check Phase 1 target
    if [ "${THREADS}" == "1" ] && [ "${CONNECTIONS}" == "10" ]; then
        THRESHOLD=2000
        if [ "$(echo "${REQ_PER_SEC} >= ${THRESHOLD}" | bc -l)" == "1" ]; then
            echo "✓ Phase 1 target met: >= ${THRESHOLD} req/s"
        else
            echo "⚠ Phase 1 target NOT met: ${REQ_PER_SEC} < ${THRESHOLD} req/s"
        fi
    fi

    # Check Phase 2 target
    if [ "${THREADS}" == "4" ] && [ "${CONNECTIONS}" == "100" ]; then
        THRESHOLD=5000
        if [ "$(echo "${REQ_PER_SEC} >= ${THRESHOLD}" | bc -l)" == "1" ]; then
            echo "✓ Phase 2 target met: >= ${THRESHOLD} req/s"
        else
            echo "⚠ Phase 2 target NOT met: ${REQ_PER_SEC} < ${THRESHOLD} req/s"
        fi
    fi

    echo ""
    echo "---"
    echo ""
done

# Cleanup
echo "Stopping server..."
kill ${SERVER_PID} 2>/dev/null
wait ${SERVER_PID} 2>/dev/null

echo ""
echo "=== Benchmark Complete ==="
echo "Results directory: ${RESULTS_DIR}"
echo ""
echo "Summary of all benchmarks:"
for f in ${RESULTS_DIR}/benchmark_*_${TIMESTAMP}.txt; do
    echo "$(basename $f):"
    grep "Requests/sec" "$f"
    grep "Latency" "$f" | head -4
    echo ""
done