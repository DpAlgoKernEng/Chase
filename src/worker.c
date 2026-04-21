/**
 * @file    worker.c
 * @brief   Worker 进程管理实现
 *
 * @details
 *          - 简化为进程信号处理和生命周期管理
 *          - Server 创建逻辑移到 Server 模块
 *          - Worker 只管理运行状态和信号
 *          - 与 Master 进程配合实现多进程架构
 *
 * @layer   Process Layer
 *
 * @depends server, eventloop
 * @usedby  master, examples
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "worker.h"
#include "server.h"
#include "eventloop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

/* Worker 进程上下文 */
struct Worker {
    WorkerConfig config;
};

/* 全局 Worker 指针（信号处理需要） */
static Worker *g_worker = NULL;

/* ========== Worker 信号处理 ========== */

static void worker_signal_handler(int sig) {
    if (g_worker == NULL) return;

    switch (sig) {
        case SIGINT:
        case SIGTERM:
            g_worker->config.running = 0;
            /* 停止 Server */
            if (g_worker->config.server) {
                server_stop(g_worker->config.server);
            }
            break;
        default:
            break;
    }
}

static int install_worker_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = worker_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGINT, &sa, NULL) < 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0) return -1;

    /* 忽略 SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    return 0;
}

/* ========== API 实现 ========== */

Worker *worker_create(const WorkerConfig *config) {
    if (!config || !config->server) return NULL;

    Worker *worker = malloc(sizeof(Worker));
    if (!worker) return NULL;

    memcpy(&worker->config, config, sizeof(WorkerConfig));
    worker->config.running = 1;

    /* 设置全局指针 */
    g_worker = worker;

    printf("[Worker %d] Created (pid=%d)\n",
           config->worker_id, getpid());

    return worker;
}

void worker_destroy(Worker *worker) {
    if (!worker) return;

    g_worker = NULL;

    printf("[Worker %d] Destroyed\n", worker->config.worker_id);

    free(worker);
}

int worker_run(Worker *worker) {
    if (!worker) return -1;

    /* 安装信号处理器 */
    if (install_worker_signal_handlers() < 0) {
        fprintf(stderr, "[Worker %d] Failed to install signal handlers\n",
                worker->config.worker_id);
        return -1;
    }

    printf("[Worker %d] Running (pid=%d)\n",
           worker->config.worker_id, getpid());

    /* 运行 Server */
    int result = server_run(worker->config.server);

    printf("[Worker %d] Exited (result=%d)\n",
           worker->config.worker_id, result);

    return result;
}

void worker_stop(Worker *worker) {
    if (!worker) return;

    worker->config.running = 0;

    if (worker->config.server) {
        server_stop(worker->config.server);
    }
}

int worker_get_id(Worker *worker) {
    if (!worker) return -1;
    return worker->config.worker_id;
}

Server *worker_get_server(Worker *worker) {
    if (!worker) return NULL;
    return worker->config.server;
}