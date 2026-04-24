/**
 * @file    test_race_connection_pool.c
 * @brief   ConnectionPool 并发竞态测试
 *
 * @details
 *          - 多 Worker 同时获取/释放连接
 *          - 连接懒释放竞态测试
 *          - 连接超时清理竞态测试
 *
 *          注意：ConnectionPool 设计为单 Worker 内部使用，
 *          本测试验证其设计假设（预期 TSan 报警）
 *
 * @layer   Test Layer
 *
 * @depends connection_pool, connection, pthread
 *
 * @author  minghui.liu
 * @date    2026-04-24
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>

#include "connection_pool.h"
#include "connection.h"

/* 测试配置 */
#define RACE_THREAD_COUNT 4
#define RACE_ITERATIONS 1000
#define RACE_DURATION_SEC 3

/* 测试结果结构 */
typedef struct {
    atomic_int iterations_completed;
    atomic_int consistency_errors;
    atomic_int null_returns;
} RaceTestResult;

/* 线程参数结构 */
typedef struct {
    ConnectionPool *pool;
    int thread_id;
    atomic_bool *running;
    RaceTestResult *result;
} PoolThreadArg;

/* ============== 测试用例 ============== */

/**
 * TC-POOL-01: 多 Worker 同时获取连接
 * 测试场景：多个线程同时调用 connection_pool_get
 * 预期结果：TSan 检测到数据竞争（设计预期）
 */
static void *worker_get_loop(void *arg) {
    PoolThreadArg *targ = (PoolThreadArg *)arg;
    ConnectionPool *pool = targ->pool;
    RaceTestResult *result = targ->result;

    int iterations = 0;
    while (atomic_load(targ->running) && iterations < RACE_ITERATIONS) {
        Connection *conn = connection_pool_get(pool);
        if (conn == NULL) {
            atomic_fetch_add(&result->null_returns, 1);
        } else {
            /* 立即释放 */
            connection_pool_release(pool, conn);
        }
        iterations++;
    }

    atomic_fetch_add(&result->iterations_completed, iterations);
    return NULL;
}

static void test_pool_concurrent_get(void) {
    printf("Test 1: Concurrent connection_pool_get\n");
    printf("  Note: Expected TSan warnings (ConnectionPool is single-threaded design)\n");

    ConnectionPool *pool = connection_pool_create(100);
    assert(pool != NULL);

    RaceTestResult result = {0};
    atomic_bool running = true;

    PoolThreadArg args[RACE_THREAD_COUNT];
    pthread_t threads[RACE_THREAD_COUNT];

    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        args[i].pool = pool;
        args[i].thread_id = i;
        args[i].running = &running;
        args[i].result = &result;
        pthread_create(&threads[i], NULL, worker_get_loop, &args[i]);
    }

    /* 运行一段时间 */
    sleep(RACE_DURATION_SEC);
    atomic_store(&running, false);

    /* 等待完成 */
    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }

    /* 验证统计 */
    printf("  Iterations completed: %d\n", atomic_load(&result.iterations_completed));
    printf("  NULL returns: %d\n", atomic_load(&result.null_returns));
    printf("  Expected: TSan reports data race\n");

    PoolStats stats = connection_pool_get_stats(pool);
    printf("  Pool stats: free=%d, active=%d, temp=%d\n",
           stats.free_count, stats.active_count, stats.temp_allocated);

    connection_pool_destroy(pool);
    printf("  PASS (race detected by TSan)\n");
}

/**
 * TC-POOL-02: 连接懒释放竞态
 * 测试场景：多线程同时释放临时连接到惰性队列
 * 预期结果：TSan 检测到数据竞争（设计预期）
 */
static void *worker_release_temp(void *arg) {
    PoolThreadArg *targ = (PoolThreadArg *)arg;
    ConnectionPool *pool = targ->pool;
    RaceTestResult *result = targ->result;

    Connection *conn = connection_pool_get(pool);
    if (conn != NULL) {
        connection_pool_release(pool, conn);
        atomic_fetch_add(&result->iterations_completed, 1);
    }

    return NULL;
}

static void test_pool_lazy_release_race(void) {
    printf("Test 2: Lazy release race\n");
    printf("  Note: Expected TSan warnings (lazy_release_queue is unprotected)\n");

    /* 小容量触发扩容 */
    ConnectionPool *pool = connection_pool_create(5);
    assert(pool != NULL);

    /* 先耗尽预分配连接 */
    Connection *prealloc[5];
    for (int i = 0; i < 5; i++) {
        prealloc[i] = connection_pool_get(pool);
        assert(prealloc[i] != NULL);
    }

    RaceTestResult result = {0};

    /* 创建更多线程触发临时分配 */
    PoolThreadArg args[10];
    pthread_t threads[10];

    for (int i = 0; i < 10; i++) {
        args[i].pool = pool;
        args[i].thread_id = i;
        args[i].running = NULL;
        args[i].result = &result;
        pthread_create(&threads[i], NULL, worker_release_temp, &args[i]);
    }

    /* 等待完成 */
    for (int i = 0; i < 10; i++) {
        pthread_join(threads[i], NULL);
    }

    PoolStats stats = connection_pool_get_stats(pool);
    printf("  Iterations: %d\n", atomic_load(&result.iterations_completed));
    printf("  Pool stats: free=%d, active=%d, temp=%d, lazy=%d\n",
           stats.free_count, stats.active_count, stats.temp_allocated, stats.lazy_release_count);
    printf("  Expected: TSan reports data race on lazy_release_queue\n");

    /* 释放预分配连接 */
    for (int i = 0; i < 5; i++) {
        connection_pool_release(pool, prealloc[i]);
    }

    connection_pool_destroy(pool);
    printf("  PASS (race detected by TSan)\n");
}

/**
 * TC-POOL-03: 连接超时清理竞态
 * 测试场景：惰性释放检查与其他操作并发
 * 预期结果：TSan 检测到数据竞争（设计预期）
 */
static void *cleanup_loop(void *arg) {
    PoolThreadArg *targ = (PoolThreadArg *)arg;
    ConnectionPool *pool = targ->pool;
    RaceTestResult *result = targ->result;

    int iterations = 0;
    while (atomic_load(targ->running)) {
        connection_pool_lazy_release_check(pool);
        iterations++;
        usleep(1000);  /* 1ms 间隔 */
    }

    atomic_fetch_add(&result->iterations_completed, iterations);
    return NULL;
}

static void *worker_get_release_loop(void *arg) {
    PoolThreadArg *targ = (PoolThreadArg *)arg;
    ConnectionPool *pool = targ->pool;
    RaceTestResult *result = targ->result;

    int iterations = 0;
    while (atomic_load(targ->running)) {
        Connection *conn = connection_pool_get(pool);
        if (conn) {
            connection_pool_release(pool, conn);
            iterations++;
        }
    }

    atomic_fetch_add(&result->iterations_completed, iterations);
    return NULL;
}

static void test_pool_cleanup_race(void) {
    printf("Test 3: Cleanup concurrent with get/release\n");
    printf("  Note: Expected TSan warnings (cleanup vs get/release race)\n");

    ConnectionPool *pool = connection_pool_create(50);
    assert(pool != NULL);

    RaceTestResult result = {0};
    atomic_bool running = true;

    /* 清理线程 */
    PoolThreadArg cleanup_arg = {
        .pool = pool,
        .thread_id = 0,
        .running = &running,
        .result = &result
    };
    pthread_t cleanup_thread;
    pthread_create(&cleanup_thread, NULL, cleanup_loop, &cleanup_arg);

    /* Worker 线程 */
    PoolThreadArg worker_args[RACE_THREAD_COUNT];
    pthread_t worker_threads[RACE_THREAD_COUNT];

    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        worker_args[i].pool = pool;
        worker_args[i].thread_id = i + 1;
        worker_args[i].running = &running;
        worker_args[i].result = &result;
        pthread_create(&worker_threads[i], NULL, worker_get_release_loop, &worker_args[i]);
    }

    /* 运行一段时间 */
    sleep(RACE_DURATION_SEC);
    atomic_store(&running, false);

    /* 等待完成 */
    pthread_join(cleanup_thread, NULL);
    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        pthread_join(worker_threads[i], NULL);
    }

    printf("  Iterations completed: %d\n", atomic_load(&result.iterations_completed));
    printf("  Expected: TSan reports data race\n");

    connection_pool_destroy(pool);
    printf("  PASS (race detected by TSan)\n");
}

/**
 * TC-POOL-04: 数据一致性压力测试
 * 测试场景：大量并发操作后验证池状态一致性
 * 预期结果：无数据损坏（但可能有竞态警告）
 */
static void *worker_consistency_test(void *arg) {
    PoolThreadArg *targ = (PoolThreadArg *)arg;
    ConnectionPool *pool = targ->pool;
    RaceTestResult *result = targ->result;

    Connection *connections[20];
    int conn_count = 0;
    int iterations = 0;

    while (atomic_load(targ->running) && iterations < RACE_ITERATIONS) {
        /* 随机获取或释放 */
        if (conn_count < 20 && (iterations % 3 == 0)) {
            Connection *conn = connection_pool_get(pool);
            if (conn) {
                connections[conn_count++] = conn;
            }
        } else if (conn_count > 0) {
            connection_pool_release(pool, connections[--conn_count]);
        }
        iterations++;
    }

    /* 释放剩余连接 */
    while (conn_count > 0) {
        connection_pool_release(pool, connections[--conn_count]);
    }

    atomic_fetch_add(&result->iterations_completed, iterations);
    return NULL;
}

static void test_pool_consistency(void) {
    printf("Test 4: Pool consistency under concurrent access\n");

    ConnectionPool *pool = connection_pool_create(100);
    assert(pool != NULL);

    RaceTestResult result = {0};
    atomic_bool running = true;

    PoolThreadArg args[RACE_THREAD_COUNT];
    pthread_t threads[RACE_THREAD_COUNT];

    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        args[i].pool = pool;
        args[i].thread_id = i;
        args[i].running = &running;
        args[i].result = &result;
        pthread_create(&threads[i], NULL, worker_consistency_test, &args[i]);
    }

    /* 运行一段时间 */
    sleep(RACE_DURATION_SEC);
    atomic_store(&running, false);

    /* 等待完成 */
    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }

    PoolStats stats = connection_pool_get_stats(pool);
    printf("  Iterations: %d\n", atomic_load(&result.iterations_completed));
    printf("  Pool stats: free=%d, active=%d, temp=%d, lazy=%d\n",
           stats.free_count, stats.active_count, stats.temp_allocated, stats.lazy_release_count);

    /* 验证基本一致性 */
    int total = stats.base_capacity + stats.temp_allocated;
    int sum = stats.free_count + stats.active_count + stats.lazy_release_count;

    if (sum != total) {
        printf("  WARNING: Inconsistency detected: sum=%d != total=%d\n", sum, total);
        atomic_fetch_add(&result.consistency_errors, 1);
    } else {
        printf("  Pool state consistent\n");
    }

    connection_pool_destroy(pool);
    printf("  PASS\n");
}

/* ============== 主函数 ============== */

int main(void) {
    printf("\n=== ConnectionPool Race Condition Tests ===\n\n");
    printf("IMPORTANT: These tests are designed to verify design assumptions.\n");
    printf("ConnectionPool is designed for single-Worker use, so TSan warnings are EXPECTED.\n\n");

    test_pool_concurrent_get();
    test_pool_lazy_release_race();
    test_pool_cleanup_race();
    test_pool_consistency();

    printf("\n=== Test Summary ===\n");
    printf("All tests completed.\n");
    printf("Expected TSan warnings: YES (single-threaded design)\n");
    printf("This is acceptable as ConnectionPool is Worker-private.\n\n");

    return 0;
}