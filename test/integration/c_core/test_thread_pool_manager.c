#include "thread_pool_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

/* 测试 1: 创建和销毁线程池管理器 */
static void test_manager_create_destroy(void) {
    printf("Test 1: ThreadPoolManager create/destroy\n");

    ThreadPoolManager *manager = thread_pool_manager_create(4, 1024);
    assert(manager != NULL);
    assert(thread_pool_manager_get_worker_count(manager) == 4);

    thread_pool_manager_destroy(manager);
    printf("  PASS\n");
}

/* 测试 2: 创建无效参数 */
static void test_manager_create_invalid(void) {
    printf("Test 2: ThreadPoolManager create invalid params\n");

    ThreadPoolManager *manager = thread_pool_manager_create(0, 1024);
    assert(manager == NULL);

    manager = thread_pool_manager_create(4, 0);
    assert(manager == NULL);

    manager = thread_pool_manager_create(-1, 1024);
    assert(manager == NULL);

    printf("  PASS\n");
}

/* 测试 3: Worker 线程启动和停止 */
static void test_worker_start_stop(void) {
    printf("Test 3: WorkerThread start/stop\n");

    ThreadPoolManager *manager = thread_pool_manager_create(2, 512);
    assert(manager != NULL);

    int result = thread_pool_manager_start(manager);
    assert(result == 0);

    /* 等待线程启动 */
    usleep(100000);  /* 100ms */

    thread_pool_manager_stop(manager);
    thread_pool_manager_destroy(manager);
    printf("  PASS\n");
}

/* 测试 4: 分发策略设置 */
static void test_dispatch_strategy(void) {
    printf("Test 4: Dispatch strategy\n");

    ThreadPoolManager *manager = thread_pool_manager_create(4, 512);
    assert(manager != NULL);

    /* 默认策略 */
    DispatchStrategy strategy = thread_pool_manager_get_strategy(manager);
    assert(strategy == DISPATCH_LEAST_CONNECTIONS);

    /* 设置 Round-Robin */
    int result = thread_pool_manager_set_strategy(manager, DISPATCH_ROUND_ROBIN);
    assert(result == 0);
    strategy = thread_pool_manager_get_strategy(manager);
    assert(strategy == DISPATCH_ROUND_ROBIN);

    /* 设置 Least-Connections */
    result = thread_pool_manager_set_strategy(manager, DISPATCH_LEAST_CONNECTIONS);
    assert(result == 0);
    strategy = thread_pool_manager_get_strategy(manager);
    assert(strategy == DISPATCH_LEAST_CONNECTIONS);

    thread_pool_manager_destroy(manager);
    printf("  PASS\n");
}

/* 测试 5: Worker 连接计数 */
static void test_worker_connection_count(void) {
    printf("Test 5: Worker connection count\n");

    WorkerThread *worker = worker_thread_create(512, 0);
    assert(worker != NULL);

    assert(worker_thread_get_connection_count(worker) == 0);
    assert(worker_thread_get_id(worker) == 0);

    /* 增加计数 */
    worker_thread_increment_connections(worker);
    assert(worker_thread_get_connection_count(worker) == 1);

    worker_thread_increment_connections(worker);
    assert(worker_thread_get_connection_count(worker) == 2);

    /* 减少计数 */
    worker_thread_decrement_connections(worker);
    assert(worker_thread_get_connection_count(worker) == 1);

    worker_thread_decrement_connections(worker);
    assert(worker_thread_get_connection_count(worker) == 0);

    worker_thread_destroy(worker);
    printf("  PASS\n");
}

/* 测试 6: Worker EventLoop */
static void test_worker_eventloop(void) {
    printf("Test 6: Worker EventLoop\n");

    WorkerThread *worker = worker_thread_create(512, 0);
    assert(worker != NULL);

    EventLoop *loop = worker_thread_get_eventloop(worker);
    assert(loop != NULL);

    worker_thread_destroy(worker);
    printf("  PASS\n");
}

/* 测试 7: 负载均衡统计 */
static void test_balance_stats(void) {
    printf("Test 7: Balance stats\n");

    ThreadPoolManager *manager = thread_pool_manager_create(4, 512);
    assert(manager != NULL);

    int connections[4];
    int result = thread_pool_manager_get_balance_stats(manager, connections, 4);
    assert(result == 0);

    /* 初始连接数应为 0 */
    for (int i = 0; i < 4; i++) {
        assert(connections[i] == 0);
    }

    thread_pool_manager_destroy(manager);
    printf("  PASS\n");
}

/* 测试 8: 单个 Worker 连接数查询 */
static void test_worker_connections_query(void) {
    printf("Test 8: Worker connections query\n");

    ThreadPoolManager *manager = thread_pool_manager_create(4, 512);
    assert(manager != NULL);

    for (int i = 0; i < 4; i++) {
        int count = thread_pool_manager_get_worker_connections(manager, i);
        assert(count == 0);
    }

    /* 测试无效索引 */
    int count = thread_pool_manager_get_worker_connections(manager, -1);
    assert(count == -1);

    count = thread_pool_manager_get_worker_connections(manager, 100);
    assert(count == -1);

    thread_pool_manager_destroy(manager);
    printf("  PASS\n");
}

/* 测试 9: Worker 通知 fd */
static void test_worker_notify_fd(void) {
    printf("Test 9: Worker notify fd\n");

    WorkerThread *worker = worker_thread_create(512, 0);
    assert(worker != NULL);

    int notify_fd = worker_thread_get_notify_fd(worker);
    assert(notify_fd >= 0);

    worker_thread_destroy(worker);
    printf("  PASS\n");
}

/* 测试 10: Manager EventLoop 查询 */
static void test_manager_eventloop_query(void) {
    printf("Test 10: Manager EventLoop query\n");

    ThreadPoolManager *manager = thread_pool_manager_create(4, 512);
    assert(manager != NULL);

    for (int i = 0; i < 4; i++) {
        EventLoop *loop = thread_pool_manager_get_worker_eventloop(manager, i);
        assert(loop != NULL);
    }

    /* 测试无效索引 */
    EventLoop *loop = thread_pool_manager_get_worker_eventloop(manager, -1);
    assert(loop == NULL);

    loop = thread_pool_manager_get_worker_eventloop(manager, 100);
    assert(loop == NULL);

    thread_pool_manager_destroy(manager);
    printf("  PASS\n");
}

int main(void) {
    printf("=== ThreadPoolManager Module Tests ===\n\n");

    test_manager_create_destroy();
    test_manager_create_invalid();
    test_worker_start_stop();
    test_dispatch_strategy();
    test_worker_connection_count();
    test_worker_eventloop();
    test_balance_stats();
    test_worker_connections_query();
    test_worker_notify_fd();
    test_manager_eventloop_query();

    printf("\n=== All tests passed ===\n");
    return 0;
}