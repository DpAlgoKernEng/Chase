/**
 * @file    worker.h
 * @brief   Worker 进程管理，处理进程信号和生命周期
 *
 * @details
 *          - 管理单个 Worker 进程的生命周期
 *          - 处理 SIGINT、SIGTERM、SIGPIPE 信号
 *          - 调用 Server 模块运行 HTTP 服务
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

#ifndef CHASE_WORKER_H
#define CHASE_WORKER_H

#include <stdint.h>
#include <stdbool.h>
#include <signal.h>

/* Server 前向声明 */
typedef struct Server Server;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Worker 进程配置
 */
typedef struct WorkerConfig {
    int worker_id;              /**< Worker 序号（0 ~ N-1） */
    Server *server;             /**< Server 指针（外部创建） */
    volatile sig_atomic_t running; /**< 运行标志 */
} WorkerConfig;

/**
 * Worker 进程上下文（不透明指针）
 */
typedef struct Worker Worker;

/**
 * 创建 Worker 进程上下文
 * @param config Worker 配置
 * @return Worker 指针，失败返回 NULL
 */
Worker *worker_create(const WorkerConfig *config);

/**
 * 销毁 Worker 进程上下文
 * @param worker Worker 指针
 */
void worker_destroy(Worker *worker);

/**
 * 运行 Worker 主循环
 * @param worker Worker 指针
 * @return 0 正常退出，非零错误码
 */
int worker_run(Worker *worker);

/**
 * 停止 Worker 主循环
 * @param worker Worker 指针
 */
void worker_stop(Worker *worker);

/**
 * 获取 Worker ID
 * @param worker Worker 指针
 * @return Worker ID，-1 表示无效
 */
int worker_get_id(Worker *worker);

/**
 * 获取 Worker 的 Server 指针
 * @param worker Worker 指针
 * @return Server 指针
 */
Server *worker_get_server(Worker *worker);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_WORKER_H */