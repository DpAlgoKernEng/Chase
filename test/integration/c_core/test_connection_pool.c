/**
 * @file    test_connection_pool.c
 * @brief   Connection Pool 模块测试
 *
 * @details
 *          - 测试连接池创建和销毁
 *          - 测试获取和释放连接
 *          - 测试阈值扩容
 *          - 测试惰性释放
 *          - 测试统计接口
 *
 * @layer   Test
 *
 * @depends connection_pool, connection
 * @usedby  测试框架
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "connection_pool.h"
#include "connection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* 测试辅助宏 */
#define TEST(name) static void test_##name()
#define RUN_TEST(name) do { \
    printf("Test %d: %s\n", test_count++, #name); \
    test_##name(); \
    test_passed++; \
} while(0)

static int test_passed = 0;
static int test_count = 1;

/* 测试 1: 创建和销毁 */
TEST(pool_create_destroy) {
    ConnectionPool *pool = connection_pool_create(100);
    assert(pool != NULL);
    assert(connection_pool_get_base_capacity(pool) == 100);
    assert(connection_pool_get_free_count(pool) == 100);

    connection_pool_destroy(pool);
}

/* 测试 2: 创建无效参数 */
TEST(pool_create_invalid_capacity) {
    ConnectionPool *pool = connection_pool_create(0);
    assert(pool == NULL);

    pool = connection_pool_create(-1);
    assert(pool == NULL);
}

/* 测试 3: 获取和释放基础 */
TEST(pool_get_release_basic) {
    ConnectionPool *pool = connection_pool_create(100);
    assert(pool != NULL);

    /* 获取一个连接 */
    Connection *conn = connection_pool_get(pool);
    assert(conn != NULL);
    assert(connection_pool_get_free_count(pool) == 99);

    PoolStats stats = connection_pool_get_stats(pool);
    assert(stats.active_count == 1);
    assert(stats.free_count == 99);

    /* 验证连接状态 */
    assert(connection_get_state(conn) == CONN_STATE_CLOSED);
    assert(connection_get_fd(conn) == -1);

    /* 释放连接 */
    connection_pool_release(pool, conn);
    assert(connection_pool_get_free_count(pool) == 100);

    stats = connection_pool_get_stats(pool);
    assert(stats.active_count == 0);
    assert(stats.free_count == 100);

    connection_pool_destroy(pool);
}

/* 测试 4: 获取和释放多个 */
TEST(pool_get_release_multiple) {
    ConnectionPool *pool = connection_pool_create(100);
    assert(pool != NULL);

    /* 获取 50 个连接 */
    Connection *conns[50];
    for (int i = 0; i < 50; i++) {
        conns[i] = connection_pool_get(pool);
        assert(conns[i] != NULL);
    }
    assert(connection_pool_get_free_count(pool) == 50);

    PoolStats stats = connection_pool_get_stats(pool);
    assert(stats.active_count == 50);

    /* 释放 50 个连接 */
    for (int i = 0; i < 50; i++) {
        connection_pool_release(pool, conns[i]);
    }
    assert(connection_pool_get_free_count(pool) == 100);

    stats = connection_pool_get_stats(pool);
    assert(stats.active_count == 0);

    connection_pool_destroy(pool);
}

/* 测试 5: 阈值扩容 */
TEST(pool_threshold_expand) {
    ConnectionPool *pool = connection_pool_create(100);
    assert(pool != NULL);

    /* 获取 90 个连接（剩余 10 个，刚好 10% 阈值） */
    Connection *conns[105];
    for (int i = 0; i < 90; i++) {
        conns[i] = connection_pool_get(pool);
        assert(conns[i] != NULL);
    }
    assert(connection_pool_get_free_count(pool) == 10);
    /* 当 free_count == 10% 时，不触发扩容 */
    assert(connection_pool_should_expand(pool) == 0);

    PoolStats stats = connection_pool_get_stats(pool);
    assert(stats.temp_allocated == 0);

    /* 再获取 1 个，使 free_count = 9 (< 10%)，触发扩容阈值 */
    conns[90] = connection_pool_get(pool);
    assert(conns[90] != NULL);
    assert(connection_pool_get_free_count(pool) == 9);
    assert(connection_pool_should_expand(pool) == 1);

    /* 下一次获取会触发临时 malloc（因为 should_expand 返回 1） */
    conns[91] = connection_pool_get(pool);
    stats = connection_pool_get_stats(pool);
    assert(stats.temp_allocated == 1);

    /* 继续获取 */
    for (int i = 92; i < 105; i++) {
        conns[i] = connection_pool_get(pool);
        assert(conns[i] != NULL);
    }

    stats = connection_pool_get_stats(pool);
    assert(stats.temp_allocated > 0);

    connection_pool_destroy(pool);
}

/* 测试 6: 统计接口 */
TEST(pool_stats) {
    ConnectionPool *pool = connection_pool_create(100);
    assert(pool != NULL);

    /* 初始状态 */
    PoolStats stats = connection_pool_get_stats(pool);
    assert(stats.base_capacity == 100);
    assert(stats.free_count == 100);
    assert(stats.active_count == 0);
    assert(stats.temp_allocated == 0);
    assert(stats.utilization == 0.0f);

    /* 获取 10 个连接 */
    Connection *conns[10];
    for (int i = 0; i < 10; i++) {
        conns[i] = connection_pool_get(pool);
    }

    stats = connection_pool_get_stats(pool);
    assert(stats.free_count == 90);
    assert(stats.active_count == 10);
    assert(stats.utilization > 0.0f && stats.utilization < 0.2f);

    /* 释放连接 */
    for (int i = 0; i < 10; i++) {
        connection_pool_release(pool, conns[i]);
    }

    stats = connection_pool_get_stats(pool);
    assert(stats.free_count == 100);
    assert(stats.active_count == 0);

    connection_pool_destroy(pool);
}

/* 测试 7: 惰性释放检查 */
TEST(pool_lazy_release) {
    ConnectionPool *pool = connection_pool_create(10);
    assert(pool != NULL);

    /* 获取 9 个连接（free_count = 1 = 10% 阈值） */
    Connection *conns[15];
    for (int i = 0; i < 9; i++) {
        conns[i] = connection_pool_get(pool);
        assert(conns[i] != NULL);
    }
    assert(connection_pool_get_free_count(pool) == 1);

    /* 再获取 1 个（free_count = 0 < 10%），触发扩容阈值 */
    conns[9] = connection_pool_get(pool);
    assert(connection_pool_should_expand(pool) == 1);

    /* 再获取 5 个，触发临时 malloc */
    for (int i = 10; i < 15; i++) {
        conns[i] = connection_pool_get(pool);
        assert(conns[i] != NULL);
    }

    PoolStats stats = connection_pool_get_stats(pool);
    assert(stats.temp_allocated > 0);

    /* 释放临时连接 */
    for (int i = 10; i < 15; i++) {
        connection_pool_release(pool, conns[i]);
    }

    stats = connection_pool_get_stats(pool);
    assert(stats.lazy_release_count > 0);

    /* 惰性释放检查（不会立即释放，因为延迟是 60 秒） */
    connection_pool_lazy_release_check(pool);

    stats = connection_pool_get_stats(pool);
    /* 仍然在惰性队列中 */
    assert(stats.lazy_release_count > 0);

    connection_pool_destroy(pool);
}

/* 测试 8: 深度压力测试 */
TEST(pool_stress) {
    ConnectionPool *pool = connection_pool_create(1000);
    assert(pool != NULL);

    /* 获取 900 个连接（free_count = 100 = 10% 阈值） */
    Connection *conns[1100];
    for (int i = 0; i < 900; i++) {
        conns[i] = connection_pool_get(pool);
        assert(conns[i] != NULL);
    }
    assert(connection_pool_get_free_count(pool) == 100);

    /* 再获取 1 个，触发阈值 */
    conns[900] = connection_pool_get(pool);
    assert(connection_pool_should_expand(pool) == 1);

    /* 再获取 99 个，触发临时 malloc */
    for (int i = 901; i < 1000; i++) {
        conns[i] = connection_pool_get(pool);
    }

    /* 再获取 100 个额外连接 */
    for (int i = 1000; i < 1100; i++) {
        conns[i] = connection_pool_get(pool);
    }

    PoolStats stats = connection_pool_get_stats(pool);
    assert(stats.temp_allocated > 0);

    /* 释放所有 */
    for (int i = 0; i < 1100; i++) {
        connection_pool_release(pool, conns[i]);
    }

    stats = connection_pool_get_stats(pool);
    assert(stats.active_count == 0);

    connection_pool_destroy(pool);
}

int main(void) {
    printf("=== Connection Pool Tests ===\n\n");

    RUN_TEST(pool_create_destroy);
    RUN_TEST(pool_create_invalid_capacity);
    RUN_TEST(pool_get_release_basic);
    RUN_TEST(pool_get_release_multiple);
    RUN_TEST(pool_threshold_expand);
    RUN_TEST(pool_stats);
    RUN_TEST(pool_lazy_release);
    RUN_TEST(pool_stress);

    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", test_passed);

    return 0;
}