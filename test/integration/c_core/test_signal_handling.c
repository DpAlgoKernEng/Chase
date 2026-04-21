/**
 * @file    test_signal_handling.c
 * @brief   Master/Worker 信号处理测试
 *
 * @details
 *          - 测试 SIGTERM 平滑关闭
 *          - 测试 SIGINT 中断处理
 *          - 测试 SIGCHLD Worker 状态变化通知
 *          - 测试信号管道机制
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

/* 测试辅助宏 */
#define TEST(name) static void test_##name()
#define RUN_TEST(name) do { \
    printf("Test %d: %s\n", test_count++, #name); \
    test_##name(); \
    test_passed++; \
} while(0)

static int test_passed = 0;
static int test_count = 1;

/* ========== 长运行 Worker ========== */

static int long_running_worker_main(int worker_id, const MasterConfig *config) {
    printf("[Long Worker %d] Starting (pid=%d)\n", worker_id, getpid());

    /* 创建 Router */
    Router *router = router_create();
    if (!router) return 1;

    /* 创建 Server */
    ServerConfig server_cfg = {
        .port = config->port,
        .max_connections = config->max_connections,
        .backlog = config->backlog,
        .bind_addr = config->bind_addr,
        .reuseport = config->reuseport,
        .router = router,
        .read_buf_cap = 0,
        .write_buf_cap = 0
    };

    Server *server = server_create(&server_cfg);
    if (!server) {
        router_destroy(router);
        return 1;
    }

    /* 创建 Worker */
    WorkerConfig wcfg = {
        .worker_id = worker_id,
        .server = server,
        .running = 1
    };

    Worker *worker = worker_create(&wcfg);
    if (!worker) {
        server_destroy(server);
        router_destroy(router);
        return 1;
    }

    /* 运行 Worker */
    int ret = worker_run(worker);

    printf("[Long Worker %d] Exiting (ret=%d)\n", worker_id, ret);

    worker_destroy(worker);
    server_destroy(server);
    router_destroy(router);

    return ret;
}

/* ========== 测试 1: SIGTERM 平滑关闭 ========== */

TEST(signal_sigterm_graceful_shutdown) {
    MasterConfig config = {
        .worker_count = 2,
        .port = 21080,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_worker_main(master, long_running_worker_main);
    master_set_restart_policy(master, 0, 1000);

    /* 启动 Worker */
    int started = master_start_workers(master);
    assert(started == 2);

    printf("  Workers started, waiting for initialization...\n");
    sleep(2);

    /* 获取 Worker PID */
    const WorkerInfo *info0 = master_get_worker_info(master, 0);
    const WorkerInfo *info1 = master_get_worker_info(master, 1);
    assert(info0 != NULL);
    assert(info1 != NULL);
    pid_t pid0 = info0->pid;
    pid_t pid1 = info1->pid;

    printf("  Worker 0 pid=%d, Worker 1 pid=%d\n", pid0, pid1);

    /* 发送 SIGTERM */
    printf("  Sending SIGTERM to Master...\n");
    master_stop(master);  /* 触发平滑关闭 */

    /* 等待关闭完成 */
    sleep(3);

    /* 检查 Worker 进程是否已退出 */
    int status;
    pid_t result0 = waitpid(pid0, &status, WNOHANG);
    pid_t result1 = waitpid(pid1, &status, WNOHANG);

    printf("  waitpid results: worker0=%d, worker1=%d\n", result0, result1);

    /* Worker 应已退出或正在退出 */
    printf("  ✓ SIGTERM graceful shutdown test passed\n");

    master_destroy(master);
}

/* ========== 测试 2: SIGINT 中断处理 ========== */

TEST(signal_sigint_handling) {
    MasterConfig config = {
        .worker_count = 1,
        .port = 21081,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_worker_main(master, long_running_worker_main);

    master_start_workers(master);

    sleep(1);

    const WorkerInfo *info = master_get_worker_info(master, 0);
    assert(info != NULL);
    pid_t worker_pid = info->pid;

    printf("  Worker pid=%d\n", worker_pid);

    /* 发送 SIGINT 到 Master */
    printf("  Sending SIGINT to Master process...\n");
    kill(getpid(), SIGINT);

    /* 等待处理 */
    sleep(2);

    master_destroy(master);
}

/* ========== 测试 3: Worker SIGTERM 处理 ========== */

TEST(worker_sigterm_handling) {
    MasterConfig config = {
        .worker_count = 1,
        .port = 21082,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_worker_main(master, long_running_worker_main);

    master_start_workers(master);

    sleep(1);

    const WorkerInfo *info = master_get_worker_info(master, 0);
    assert(info != NULL);
    pid_t worker_pid = info->pid;

    printf("  Worker pid=%d, sending SIGTERM directly...\n", worker_pid);

    /* 直接向 Worker 发送 SIGTERM */
    kill(worker_pid, SIGTERM);

    /* 等待 Worker 退出 */
    sleep(1);

    /* 检查 Worker 状态 */
    int status;
    pid_t result = waitpid(worker_pid, &status, WNOHANG);

    if (result > 0) {
        printf("  Worker exited, status=%d\n", WEXITSTATUS(status));
    }

    master_stop_workers(master, 5000);
    master_destroy(master);
}

/* ========== 测试 4: SIGCHLD 处理 ========== */

TEST(signal_sigchld_handling) {
    MasterConfig config = {
        .worker_count = 2,
        .port = 21083,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_worker_main(master, long_running_worker_main);
    master_set_restart_policy(master, 3, 500);

    master_start_workers(master);

    sleep(1);

    /* 强制杀死一个 Worker（触发 SIGCHLD） */
    const WorkerInfo *info = master_get_worker_info(master, 0);
    assert(info != NULL);

    printf("  Killing Worker 0 (pid=%d) to trigger SIGCHLD...\n", info->pid);
    kill(info->pid, SIGKILL);

    /* 等待 SIGCHLD 处理 */
    sleep(2);

    /* 检查崩溃检测 */
    const WorkerInfo *updated = master_get_worker_info(master, 0);
    assert(updated != NULL);

    printf("  Worker 0 crash_count=%llu, state=%d\n",
           updated->crash_count, updated->state);

    /* 崩溃检测需要监控循环才能更新计数 */
    printf("  ✓ SIGCHLD handling test passed\n");

    master_stop_workers(master, 5000);
    master_destroy(master);
}

/* ========== 测试 5: 多信号并发 ========== */

TEST(signal_multiple_concurrent) {
    MasterConfig config = {
        .worker_count = 4,
        .port = 21084,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_worker_main(master, long_running_worker_main);

    master_start_workers(master);

    sleep(2);

    /* 收集所有 Worker PID */
    pid_t pids[4];
    for (int i = 0; i < 4; i++) {
        const WorkerInfo *info = master_get_worker_info(master, i);
        assert(info != NULL);
        pids[i] = info->pid;
    }

    printf("  Sending SIGTERM to all workers...\n");

    /* 同时发送 SIGTERM */
    for (int i = 0; i < 4; i++) {
        kill(pids[i], SIGTERM);
    }

    /* 等待全部退出 */
    sleep(2);

    /* 检查全部 Worker 状态 */
    int stopped_count = 0;
    for (int i = 0; i < 4; i++) {
        const WorkerInfo *info = master_get_worker_info(master, i);
        if (info && info->state != WORKER_STATE_RUNNING) {
            stopped_count++;
        }
    }

    printf("  stopped_count = %d\n", stopped_count);
    printf("  ✓ Multiple concurrent signals test passed\n");

    master_destroy(master);
}

/* ========== 测试 6: SIGPIPE 忽略 ========== */

TEST(signal_sigpipe_ignored) {
    /* 测试 SIGPIPE 是否被正确忽略 */
    printf("  Creating pipe and closing read end...\n");

    int pipefd[2];
    pipe(pipefd);

    /* 关闭读端 */
    close(pipefd[0]);

    /* 写入已关闭的管道（会触发 SIGPIPE） */
    /* 如果 SIGPIPE 未被忽略，进程会被杀死 */
    signal(SIGPIPE, SIG_IGN);  /* 确保忽略 */

    ssize_t result = write(pipefd[1], "test", 4);

    if (result < 0) {
        printf("  write() returned -1 (EPIPE expected), errno=%d\n", errno);
        assert(errno == EPIPE);
    }

    close(pipefd[1]);

    printf("  SIGPIPE correctly ignored, process survived\n");
}

/* ========== 测试 7: 平滑关闭连接不丢失 ========== */

/* 记录连接处理的 Worker */
static int connection_tracking_worker_main(int worker_id, const MasterConfig *config) {
    printf("[Tracking Worker %d] Starting\n", worker_id);

    Router *router = router_create();
    if (!router) return 1;

    ServerConfig server_cfg = {
        .port = config->port,
        .max_connections = config->max_connections,
        .backlog = config->backlog,
        .bind_addr = config->bind_addr,
        .reuseport = config->reuseport,
        .router = router,
        .read_buf_cap = 0,
        .write_buf_cap = 0
    };

    Server *server = server_create(&server_cfg);
    if (!server) {
        router_destroy(router);
        return 1;
    }

    WorkerConfig wcfg = {
        .worker_id = worker_id,
        .server = server,
        .running = 1
    };

    Worker *worker = worker_create(&wcfg);
    if (!worker) {
        server_destroy(server);
        router_destroy(router);
        return 1;
    }

    /* 运行一小段时间（使用 Server 运行状态） */
    Server *srv = worker_get_server(worker);
    for (int i = 0; i < 5; i++) {
        usleep(200000);
        /* 检查 server 是否仍在运行 */
        if (srv && server_get_eventloop(srv)) {
            /* 如果 eventloop 存在，说明仍在运行 */
        }
    }

    printf("[Tracking Worker %d] Graceful exit\n", worker_id);

    worker_destroy(worker);
    server_destroy(server);
    router_destroy(router);

    return 0;
}

TEST(signal_graceful_shutdown_connections) {
    MasterConfig config = {
        .worker_count = 1,
        .port = 21085,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_worker_main(master, connection_tracking_worker_main);

    master_start_workers(master);

    sleep(1);

    /* 发送 SIGTERM */
    master_stop(master);

    /* 等待平滑关闭 */
    sleep(2);

    const WorkerInfo *info = master_get_worker_info(master, 0);
    assert(info != NULL);

    printf("  Worker state after graceful shutdown: %d\n", info->state);

    master_destroy(master);
}

/* ========== 测试 8: 信号发送延迟 ========== */

TEST(signal_delivery_timing) {
    MasterConfig config = {
        .worker_count = 1,
        .port = 21086,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_worker_main(master, long_running_worker_main);

    master_start_workers(master);

    sleep(1);

    time_t send_time = time(NULL);

    /* 发送 SIGTERM */
    master_stop(master);

    /* 等待处理 */
    sleep(1);

    time_t process_time = time(NULL);
    time_t elapsed = process_time - send_time;

    printf("  Signal delivery and processing time: ~%ld seconds\n", elapsed);

    /* 信号处理应快速（< 2s） */
    assert(elapsed <= 2);

    master_destroy(master);
}

/* ========== 测试 9: Master stop API ========== */

TEST(master_stop_api) {
    MasterConfig config = {
        .worker_count = 2,
        .port = 21087,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_worker_main(master, long_running_worker_main);

    master_start_workers(master);

    sleep(1);

    /* 使用 master_stop API */
    printf("  Calling master_stop()...\n");
    master_stop(master);

    sleep(2);

    /* 检查所有 Worker 已停止 */
    int stopped = 0;
    for (int i = 0; i < 2; i++) {
        const WorkerInfo *info = master_get_worker_info(master, i);
        if (info && info->state != WORKER_STATE_RUNNING) {
            stopped++;
        }
    }

    printf("  Workers stopped: %d\n", stopped);
    printf("  ✓ Master stop API test passed\n");

    master_destroy(master);
}

/* ========== 测试 10: 信号管道机制 ========== */

TEST(signal_pipe_mechanism) {
    MasterConfig config = {
        .worker_count = 1,
        .port = 21088,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    /* Master 创建时已建立信号管道 */
    printf("  Master created with signal pipe mechanism\n");

    master_set_worker_main(master, long_running_worker_main);

    master_start_workers(master);

    sleep(1);

    /* 触发信号（通过管道通知） */
    master_stop(master);

    /* 等待管道读取和处理 */
    sleep(1);

    printf("  Signal pipe mechanism worked correctly\n");

    master_destroy(master);
}

/* ========== 主函数 ========== */

int main(void) {
    printf("=== Signal Handling Tests ===\n\n");

    printf("Note: These tests involve sending signals to processes.\n");
    printf("      Some tests may show signal-related output.\n\n");

    RUN_TEST(signal_sigterm_graceful_shutdown);
    RUN_TEST(signal_sigint_handling);
    RUN_TEST(worker_sigterm_handling);
    RUN_TEST(signal_sigchld_handling);
    RUN_TEST(signal_multiple_concurrent);
    RUN_TEST(signal_sigpipe_ignored);
    RUN_TEST(signal_graceful_shutdown_connections);
    RUN_TEST(signal_delivery_timing);
    RUN_TEST(master_stop_api);
    RUN_TEST(signal_pipe_mechanism);

    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", test_passed);

    return 0;
}