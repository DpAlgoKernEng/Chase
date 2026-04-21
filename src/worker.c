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

/* ========== API 实现 ========== */

Worker *worker_create(const WorkerConfig *config) {
    if (!config || !config->server) return NULL;

    Worker *worker = malloc(sizeof(Worker));
    if (!worker) return NULL;

    memcpy(&worker->config, config, sizeof(WorkerConfig));
    worker->config.running = 1;

    printf("[Worker %d] Created (pid=%d)\n",
           config->worker_id, getpid());

    return worker;
}

void worker_destroy(Worker *worker) {
    if (!worker) return;

    printf("[Worker %d] Destroyed\n", worker->config.worker_id);

    free(worker);
}

int worker_run(Worker *worker) {
    if (!worker) return -1;

    printf("[Worker %d] Running (pid=%d)\n",
           worker->config.worker_id, getpid());

    /* 运行 Server（Server 内部有完整的信号处理机制） */
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