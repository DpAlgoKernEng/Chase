/**
 * @file    test_process_mgmt.c
 * @brief   Master/Worker 进程管理集成测试
 *
 * @details
 *          - 测试 Master 创建和销毁
 *          - 测试 Worker 启动和停止
 *          - 测试多 Worker 并行
 *          - 测试 SO_REUSEPORT socket
 *
 * @layer   Test
 *
 * @depends master, worker, server, router
 * @usedby  测试框架
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "master.h"
#include "worker.h"
#include "server.h"
#include "router.h"
#include "socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

/* 测试辅助宏 */
#define TEST(name) static void test_##name()
#define RUN_TEST(name) do { \
    printf("Test %d: %s\n", test_count++, #name); \
    test_##name(); \
    test_passed++; \
} while(0)

static int test_passed = 0;
static int test_count = 1;

/* ========== 测试 1: Master 创建销毁 ========== */

TEST(master_create_destroy) {
    MasterConfig config = {
        .worker_count = 2,
        .port = 19080,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);
    assert(master_get_worker_count(master) == 2);

    /* 检查 Worker 信息初始化 */
    const WorkerInfo *info = master_get_worker_info(master, 0);
    assert(info != NULL);
    assert(info->id == 0);
    assert(info->state == WORKER_STATE_IDLE);

    master_destroy(master);
}

/* ========== 测试 2: Worker 启动 ========== */

TEST(master_start_workers) {
    MasterConfig config = {
        .worker_count = 2,
        .port = 19081,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    /* 设置无限重启策略 */
    master_set_restart_policy(master, 0, 1000);

    /* 启动 Worker */
    int started = master_start_workers(master);
    assert(started == 2);

    /* 检查 Worker 状态 */
    sleep(1);  /* 等待 Worker 启动 */

    const WorkerInfo *info0 = master_get_worker_info(master, 0);
    const WorkerInfo *info1 = master_get_worker_info(master, 1);

    assert(info0 != NULL);
    assert(info1 != NULL);
    assert(info0->state == WORKER_STATE_RUNNING);
    assert(info1->state == WORKER_STATE_RUNNING);
    assert(info0->pid > 0);
    assert(info1->pid > 0);

    /* 停止 Worker */
    master_stop_workers(master, 5000);

    /* 等待所有 Worker 停止 */
    sleep(1);

    master_destroy(master);
}

/* ========== 测试 3: Worker 重启 ========== */

TEST(master_restart_single_worker) {
    MasterConfig config = {
        .worker_count = 2,
        .port = 19082,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_restart_policy(master, 5, 500);

    /* 启动 Worker */
    int started = master_start_workers(master);
    assert(started == 2);

    sleep(1);

    /* 重启单个 Worker */
    int result = master_restart_worker(master, 0);
    assert(result == 0);

    sleep(1);

    /* 检查重启计数 */
    const WorkerInfo *info = master_get_worker_info(master, 0);
    assert(info != NULL);
    assert(info->restart_count >= 1);
    assert(info->state == WORKER_STATE_RUNNING);

    /* 停止所有 Worker */
    master_stop_workers(master, 5000);
    master_destroy(master);
}

/* ========== 测试 4: 重启策略 ========== */

TEST(master_restart_policy) {
    MasterConfig config = {
        .worker_count = 1,
        .port = 19083,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    /* 测试设置重启策略 */
    master_set_restart_policy(master, 3, 1000);

    /* 启动 Worker */
    master_start_workers(master);

    sleep(1);

    const WorkerInfo *info = master_get_worker_info(master, 0);
    assert(info != NULL);

    master_stop_workers(master, 5000);
    master_destroy(master);
}

/* ========== 测试 5: Worker 信息获取 ========== */

TEST(master_get_worker_info) {
    MasterConfig config = {
        .worker_count = 4,
        .port = 19084,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    /* 测试获取有效 Worker 信息 */
    for (int i = 0; i < 4; i++) {
        const WorkerInfo *info = master_get_worker_info(master, i);
        assert(info != NULL);
        assert(info->id == i);
    }

    /* 测试获取无效 Worker 信息 */
    const WorkerInfo *invalid = master_get_worker_info(master, -1);
    assert(invalid == NULL);

    invalid = master_get_worker_info(master, 100);
    assert(invalid == NULL);

    master_destroy(master);
}

/* ========== 测试 6: Worker 需要重启判断 ========== */

TEST(master_worker_needs_restart) {
    MasterConfig config = {
        .worker_count = 1,
        .port = 19085,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_restart_policy(master, 3, 1000);

    /* 启动前 Worker 不需要重启 */
    bool needs = master_worker_needs_restart(master, 0);
    assert(needs == false);

    /* 无效 Worker ID */
    needs = master_worker_needs_restart(master, -1);
    assert(needs == false);

    master_destroy(master);
}

/* ========== 测试 7: SO_REUSEPORT socket ========== */

TEST(socket_reuseport) {
    /* 检查 SO_REUSEPORT 是否支持 */
    bool has_reuseport = socket_has_reuseport();
    printf("  SO_REUSEPORT supported: %s\n", has_reuseport ? "yes" : "no");

    /* 创建带 SO_REUSEPORT 的 socket */
    int fd = socket_create_server_default(19086, NULL, 10);
    assert(fd >= 0);

    close(fd);

    /* 创建第二个 socket（相同端口，测试 SO_REUSEPORT） */
    int fd2 = socket_create_server_default(19086, NULL, 10);
    /* 如果支持 SO_REUSEPORT，应该成功 */
    if (has_reuseport) {
        assert(fd2 >= 0);
        close(fd2);
    }
}

/* ========== 测试 8: 多 Master 独立性 ========== */

TEST(multiple_master_independence) {
    MasterConfig config1 = {
        .worker_count = 1,
        .port = 19087,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    MasterConfig config2 = {
        .worker_count = 1,
        .port = 19088,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master1 = master_create(&config1);
    Master *master2 = master_create(&config2);

    assert(master1 != NULL);
    assert(master2 != NULL);
    assert(master1 != master2);

    master_destroy(master1);
    master_destroy(master2);
}

/* ========== 测试 9: 自定义 Worker 入口 ========== */

static int custom_worker_main(int worker_id, const MasterConfig *config) {
    printf("[Custom Worker %d] Running on port %d\n", worker_id, config->port);

    /* 简单延迟后退出 */
    usleep(500000);  /* 500ms */

    return 0;  /* 正常退出 */
}

TEST(master_custom_worker_main) {
    MasterConfig config = {
        .worker_count = 2,
        .port = 19089,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    /* 设置自定义 Worker 入口 */
    master_set_worker_main(master, custom_worker_main);

    master_set_restart_policy(master, 0, 500);

    int started = master_start_workers(master);
    assert(started == 2);

    sleep(1);

    /* 等待 Worker 完成（自定义 Worker 500ms 后退出） */
    sleep(1);

    master_stop_workers(master, 5000);
    master_destroy(master);
}

/* ========== 测试 10: Master 状态统计 ========== */

TEST(master_statistics) {
    MasterConfig config = {
        .worker_count = 3,
        .port = 19090,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    int started = master_start_workers(master);
    assert(started == 3);

    sleep(1);

    /* 统计运行中的 Worker */
    int running_count = 0;
    for (int i = 0; i < 3; i++) {
        const WorkerInfo *info = master_get_worker_info(master, i);
        if (info && info->state == WORKER_STATE_RUNNING) {
            running_count++;
        }
    }
    assert(running_count == 3);

    master_stop_workers(master, 5000);
    master_destroy(master);
}

/* ========== 主函数 ========== */

int main(void) {
    printf("=== Process Management Integration Tests ===\n\n");

    RUN_TEST(master_create_destroy);
    RUN_TEST(master_start_workers);
    RUN_TEST(master_restart_single_worker);
    RUN_TEST(master_restart_policy);
    RUN_TEST(master_get_worker_info);
    RUN_TEST(master_worker_needs_restart);
    RUN_TEST(socket_reuseport);
    RUN_TEST(multiple_master_independence);
    RUN_TEST(master_custom_worker_main);
    RUN_TEST(master_statistics);

    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", test_passed);

    return 0;
}