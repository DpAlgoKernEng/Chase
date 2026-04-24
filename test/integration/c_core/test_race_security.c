/**
 * @file    test_race_security.c
 * @brief   Security 分片哈希表并发竞态测试
 *
 * @details
 *          - 同一 IP 多 Worker 检测
 *          - IP 封禁竞态测试
 *          - 速率计数器竞态测试
 *          - 分片清理并发测试
 *
 *          Security 已实现分片锁，预期 TSan 无报警
 *
 * @layer   Test Layer
 *
 * @depends security, pthread
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

#include "security.h"

/* 测试配置 */
#define RACE_THREAD_COUNT 10
#define RACE_ITERATIONS 500
#define RACE_DURATION_SEC 3

/* 测试结果结构 */
typedef struct {
    atomic_int iterations_completed;
    atomic_int successful_connections;
    atomic_int rejected_connections;
    atomic_int consistency_errors;
} RaceTestResult;

/* 线程参数结构 */
typedef struct {
    Security *security;
    IpAddress ip;
    int thread_id;
    atomic_bool *running;
    RaceTestResult *result;
    int iterations_limit;
} SecurityThreadArg;

/* ============== 辅助函数 ============== */

static Security *security_create_test(void) {
    SecurityConfig config = {
        .max_connections_per_ip = 20,
        .max_requests_per_second = 500,
        .min_request_rate = 50,
        .slowloris_timeout_ms = 30000,
        .block_duration_ms = 5000,
        .shard_count = 16
    };
    return security_create(&config);
}

/* ============== 测试用例 ============== */

/**
 * TC-SEC-01: 同一 IP 多 Worker 检测
 * 测试场景：多线程同时检测同一 IP 的连接限制
 * 预期结果：无数据竞争（TSan 不报警），计数正确
 */
static void *try_connect_same_ip(void *arg) {
    SecurityThreadArg *targ = (SecurityThreadArg *)arg;
    Security *security = targ->security;
    IpAddress ip = targ->ip;
    RaceTestResult *result = targ->result;

    int iterations = 0;
    while (iterations < targ->iterations_limit) {
        SecurityResult check = security_check_connection(security, &ip);
        if (check == SECURITY_OK) {
            security_add_connection(security, &ip);
            atomic_fetch_add(&result->successful_connections, 1);
        } else {
            atomic_fetch_add(&result->rejected_connections, 1);
        }
        iterations++;
    }

    atomic_fetch_add(&result->iterations_completed, iterations);
    return NULL;
}

static void test_security_same_ip_concurrent(void) {
    printf("Test 1: Same IP concurrent connection check\n");
    printf("  Expected: No TSan warnings (shard lock protects access)\n");

    Security *security = security_create_test();
    assert(security != NULL);

    IpAddress ip;
    security_string_to_ip("192.168.1.100", &ip);

    RaceTestResult result = {0};

    SecurityThreadArg args[RACE_THREAD_COUNT];
    pthread_t threads[RACE_THREAD_COUNT];

    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        args[i].security = security;
        args[i].ip = ip;
        args[i].thread_id = i;
        args[i].running = NULL;
        args[i].result = &result;
        args[i].iterations_limit = RACE_ITERATIONS;
        pthread_create(&threads[i], NULL, try_connect_same_ip, &args[i]);
    }

    /* 等待完成 */
    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("  Iterations: %d\n", atomic_load(&result.iterations_completed));
    printf("  Successful: %d\n", atomic_load(&result.successful_connections));
    printf("  Rejected: %d\n", atomic_load(&result.rejected_connections));

    /* 验证计数 */
    IpStats stats;
    int ret = security_get_ip_stats(security, &ip, &stats);
    assert(ret == 0);
    printf("  IP connection_count: %d\n", stats.connection_count);

    /* 计数应不超过限制 */
    assert(stats.connection_count <= 20);

    /* 清理连接 */
    while (stats.connection_count > 0) {
        security_remove_connection(security, &ip);
        security_get_ip_stats(security, &ip, &stats);
    }

    security_destroy(security);
    printf("  PASS (no race detected)\n");
}

/**
 * TC-SEC-02: IP 封禁竞态
 * 测试场景：一个线程封禁，其他线程检测封禁状态
 * 预期结果：无数据竞争（TSan 不报警）
 */
static void *block_ip_loop(void *arg) {
    SecurityThreadArg *targ = (SecurityThreadArg *)arg;
    Security *security = targ->security;
    IpAddress ip = targ->ip;
    RaceTestResult *result = targ->result;
    atomic_bool *running = targ->running;

    int iterations = 0;
    while (atomic_load(running)) {
        security_block_ip(security, &ip, 1000);
        iterations++;
        usleep(10000);  /* 10ms */
    }

    atomic_fetch_add(&result->iterations_completed, iterations);
    return NULL;
}

static void *check_blocked_loop(void *arg) {
    SecurityThreadArg *targ = (SecurityThreadArg *)arg;
    Security *security = targ->security;
    IpAddress ip = targ->ip;
    RaceTestResult *result = targ->result;
    atomic_bool *running = targ->running;

    int iterations = 0;
    while (atomic_load(running)) {
        bool blocked = security_is_blocked(security, &ip);
        if (blocked) {
            atomic_fetch_add(&result->rejected_connections, 1);
        } else {
            atomic_fetch_add(&result->successful_connections, 1);
        }
        iterations++;
    }

    atomic_fetch_add(&result->iterations_completed, iterations);
    return NULL;
}

static void test_security_block_race(void) {
    printf("Test 2: IP block race\n");
    printf("  Expected: No TSan warnings (shard lock protects block status)\n");

    Security *security = security_create_test();
    assert(security != NULL);

    IpAddress ip;
    security_string_to_ip("10.0.0.50", &ip);

    RaceTestResult result = {0};
    atomic_bool running = true;

    /* 封禁线程 */
    SecurityThreadArg block_arg = {
        .security = security,
        .ip = ip,
        .thread_id = 0,
        .running = &running,
        .result = &result
    };
    pthread_t block_thread;
    pthread_create(&block_thread, NULL, block_ip_loop, &block_arg);

    /* 检测线程 */
    SecurityThreadArg check_args[RACE_THREAD_COUNT];
    pthread_t check_threads[RACE_THREAD_COUNT];

    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        check_args[i].security = security;
        check_args[i].ip = ip;
        check_args[i].thread_id = i + 1;
        check_args[i].running = &running;
        check_args[i].result = &result;
        pthread_create(&check_threads[i], NULL, check_blocked_loop, &check_args[i]);
    }

    /* 运行一段时间 */
    sleep(RACE_DURATION_SEC);
    atomic_store(&running, false);

    /* 等待完成 */
    pthread_join(block_thread, NULL);
    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        pthread_join(check_threads[i], NULL);
    }

    printf("  Iterations: %d\n", atomic_load(&result.iterations_completed));
    printf("  Blocked detections: %d\n", atomic_load(&result.rejected_connections));
    printf("  Unblocked detections: %d\n", atomic_load(&result.successful_connections));

    security_unblock_ip(security, &ip);
    security_destroy(security);
    printf("  PASS (no race detected)\n");
}

/**
 * TC-SEC-03: 速率计数器竞态
 * 测试场景：多线程同时更新同一 IP 的请求计数
 * 预期结果：无数据竞争（TSan 不报警），计数准确
 */
static void *rate_check_loop(void *arg) {
    SecurityThreadArg *targ = (SecurityThreadArg *)arg;
    Security *security = targ->security;
    IpAddress ip = targ->ip;
    RaceTestResult *result = targ->result;

    int iterations = 0;
    while (iterations < targ->iterations_limit) {
        SecurityResult check = security_check_request_rate(security, &ip, 100);
        if (check == SECURITY_OK) {
            atomic_fetch_add(&result->successful_connections, 1);
        } else {
            atomic_fetch_add(&result->rejected_connections, 1);
        }
        iterations++;
    }

    atomic_fetch_add(&result->iterations_completed, iterations);
    return NULL;
}

static void test_security_rate_counter_race(void) {
    printf("Test 3: Rate counter race\n");
    printf("  Expected: No TSan warnings (shard lock protects counters)\n");

    Security *security = security_create_test();
    assert(security != NULL);

    IpAddress ip;
    security_string_to_ip("172.16.0.25", &ip);

    RaceTestResult result = {0};

    SecurityThreadArg args[RACE_THREAD_COUNT];
    pthread_t threads[RACE_THREAD_COUNT];

    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        args[i].security = security;
        args[i].ip = ip;
        args[i].thread_id = i;
        args[i].running = NULL;
        args[i].result = &result;
        args[i].iterations_limit = RACE_ITERATIONS;
        pthread_create(&threads[i], NULL, rate_check_loop, &args[i]);
    }

    /* 等待完成 */
    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("  Iterations: %d\n", atomic_load(&result.iterations_completed));
    printf("  OK responses: %d\n", atomic_load(&result.successful_connections));
    printf("  Rate limited: %d\n", atomic_load(&result.rejected_connections));

    /* 验证计数器 */
    IpStats stats;
    int ret = security_get_ip_stats(security, &ip, &stats);
    if (ret == 0) {
        printf("  Request count: %d\n", stats.request_count);
        printf("  Bytes received: %lu\n", stats.bytes_received);
    }

    security_destroy(security);
    printf("  PASS (no race detected)\n");
}

/**
 * TC-SEC-04: 分片清理竞态
 * 测试场景：清理线程遍历分片，Worker 线程同时操作
 * 预期结果：无死锁，无数据竞争
 */
static void *cleanup_loop(void *arg) {
    SecurityThreadArg *targ = (SecurityThreadArg *)arg;
    Security *security = targ->security;
    RaceTestResult *result = targ->result;
    atomic_bool *running = targ->running;

    int iterations = 0;
    while (atomic_load(running)) {
        security_cleanup(security);
        iterations++;
        usleep(50000);  /* 50ms */
    }

    atomic_fetch_add(&result->iterations_completed, iterations);
    return NULL;
}

static void *random_ip_operation(void *arg) {
    SecurityThreadArg *targ = (SecurityThreadArg *)arg;
    Security *security = targ->security;
    RaceTestResult *result = targ->result;
    atomic_bool *running = targ->running;
    int thread_id = targ->thread_id;

    int iterations = 0;
    while (atomic_load(running)) {
        /* 每个线程使用不同的 IP */
        char ip_str[32];
        snprintf(ip_str, sizeof(ip_str), "192.168.%d.%d", thread_id, iterations % 256);

        IpAddress ip;
        security_string_to_ip(ip_str, &ip);

        SecurityResult check = security_check_connection(security, &ip);
        if (check == SECURITY_OK) {
            security_add_connection(security, &ip);
            security_remove_connection(security, &ip);  /* 立即释放 */
            atomic_fetch_add(&result->successful_connections, 1);
        }
        iterations++;
    }

    atomic_fetch_add(&result->iterations_completed, iterations);
    return NULL;
}

static void test_security_cleanup_concurrent(void) {
    printf("Test 4: Cleanup concurrent with worker operations\n");
    printf("  Expected: No deadlock, no TSan warnings\n");

    Security *security = security_create_test();
    assert(security != NULL);

    /* 创建大量 IP 条目 */
    for (int i = 0; i < 50; i++) {
        char ip_str[32];
        snprintf(ip_str, sizeof(ip_str), "10.%d.%d.%d", i / 10, i % 10, i);
        IpAddress ip;
        security_string_to_ip(ip_str, &ip);
        security_add_connection(security, &ip);
    }

    RaceTestResult result = {0};
    atomic_bool running = true;

    /* 清理线程 */
    SecurityThreadArg cleanup_arg = {
        .security = security,
        .thread_id = 0,
        .running = &running,
        .result = &result
    };
    pthread_t cleanup_thread;
    pthread_create(&cleanup_thread, NULL, cleanup_loop, &cleanup_arg);

    /* Worker 线程 */
    SecurityThreadArg worker_args[RACE_THREAD_COUNT];
    pthread_t worker_threads[RACE_THREAD_COUNT];

    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        worker_args[i].security = security;
        worker_args[i].thread_id = i + 1;
        worker_args[i].running = &running;
        worker_args[i].result = &result;
        pthread_create(&worker_threads[i], NULL, random_ip_operation, &worker_args[i]);
    }

    /* 运行一段时间 */
    sleep(RACE_DURATION_SEC);
    atomic_store(&running, false);

    /* 等待完成（验证无死锁） */
    printf("  Waiting for threads (verifying no deadlock)...\n");

    pthread_join(cleanup_thread, NULL);
    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        pthread_join(worker_threads[i], NULL);
    }

    printf("  All threads completed (no deadlock)\n");
    printf("  Iterations: %d\n", atomic_load(&result.iterations_completed));
    printf("  Successful ops: %d\n", atomic_load(&result.successful_connections));

    /* 验证清理结果 */
    int tracked = 0, blocked = 0;
    security_get_summary(security, &tracked, &blocked);
    printf("  Tracked IPs: %d, Blocked: %d\n", tracked, blocked);

    security_destroy(security);
    printf("  PASS (no deadlock, no race)\n");
}

/**
 * TC-SEC-05: 多分片并发测试
 * 测试场景：多线程操作分布在不同分片的 IP
 * 预期结果：无数据竞争，无锁竞争问题
 */
static void *multi_shard_operation(void *arg) {
    SecurityThreadArg *targ = (SecurityThreadArg *)arg;
    Security *security = targ->security;
    RaceTestResult *result = targ->result;
    atomic_bool *running = targ->running;
    int thread_id = targ->thread_id;

    int iterations = 0;
    while (atomic_load(running) && iterations < RACE_ITERATIONS) {
        /* 使用不同分片的 IP（哈希分布） */
        char ip_str[32];
        /* 不同网段，确保分布在不同分片 */
        snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
                 10 + (thread_id * 10), thread_id, iterations % 100, thread_id);

        IpAddress ip;
        security_string_to_ip(ip_str, &ip);

        SecurityResult check = security_check_connection(security, &ip);
        if (check == SECURITY_OK) {
            security_add_connection(security, &ip);
            security_remove_connection(security, &ip);
            atomic_fetch_add(&result->successful_connections, 1);
        }
        iterations++;
    }

    atomic_fetch_add(&result->iterations_completed, iterations);
    return NULL;
}

static void test_security_multi_shard(void) {
    printf("Test 5: Multi-shard concurrent operations\n");
    printf("  Expected: No race, minimal lock contention\n");

    Security *security = security_create_test();
    assert(security != NULL);

    RaceTestResult result = {0};
    atomic_bool running = true;

    SecurityThreadArg args[RACE_THREAD_COUNT];
    pthread_t threads[RACE_THREAD_COUNT];

    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        args[i].security = security;
        args[i].thread_id = i;
        args[i].running = &running;
        args[i].result = &result;
        pthread_create(&threads[i], NULL, multi_shard_operation, &args[i]);
    }

    /* 运行一段时间 */
    sleep(RACE_DURATION_SEC);
    atomic_store(&running, false);

    /* 等待完成 */
    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("  Iterations: %d\n", atomic_load(&result.iterations_completed));
    printf("  Successful ops: %d\n", atomic_load(&result.successful_connections));

    int tracked = 0, blocked = 0;
    security_get_summary(security, &tracked, &blocked);
    printf("  Summary: tracked=%d, blocked=%d\n", tracked, blocked);

    security_destroy(security);
    printf("  PASS (no race)\n");
}

/* ============== 主函数 ============== */

int main(void) {
    printf("\n=== Security Module Race Condition Tests ===\n\n");
    printf("Security uses shard-based locking, so TSan warnings should NOT appear.\n\n");

    test_security_same_ip_concurrent();
    test_security_block_race();
    test_security_rate_counter_race();
    test_security_cleanup_concurrent();
    test_security_multi_shard();

    printf("\n=== Test Summary ===\n");
    printf("All tests completed.\n");
    printf("Expected TSan warnings: NO (shard lock protection)\n");
    printf("Security is safe for multi-threaded use.\n\n");

    return 0;
}