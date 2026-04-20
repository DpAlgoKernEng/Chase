#ifndef CHASE_THREAD_POOL_MANAGER_H
#define CHASE_THREAD_POOL_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "eventloop.h"
#include "connection_pool.h"
#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 分发策略类型 */
typedef enum {
    DISPATCH_LEAST_CONNECTIONS,  /* 最少连接优先 */
    DISPATCH_ROUND_ROBIN         /* 轮询 */
} DispatchStrategy;

/* 连接处理回调类型 */
typedef void (*ConnectionHandler)(int client_fd, EventLoop *loop, void *user_data);

/* WorkerThread 结构体（不透明指针） */
typedef struct WorkerThread WorkerThread;

/* ThreadPoolManager 结构体（不透明指针） */
typedef struct ThreadPoolManager ThreadPoolManager;

/* 连接分发回调 */
typedef void (*DispatchCallback)(int fd, void *user_data);

/**
 * 创建线程池管理器
 * @param worker_count Worker 线程数量
 * @param max_events_per_worker 每个 Worker 的最大事件数
 * @return ThreadPoolManager 指针，失败返回 NULL
 */
ThreadPoolManager *thread_pool_manager_create(int worker_count, int max_events_per_worker);

/**
 * 销毁线程池管理器
 * @param manager ThreadPoolManager 指针
 */
void thread_pool_manager_destroy(ThreadPoolManager *manager);

/**
 * 启动所有 Worker 线程
 * @param manager ThreadPoolManager 指针
 * @return 0 成功，-1 失败
 */
int thread_pool_manager_start(ThreadPoolManager *manager);

/**
 * 停止所有 Worker 线程
 * @param manager ThreadPoolManager 指针
 */
void thread_pool_manager_stop(ThreadPoolManager *manager);

/**
 * 分发新连接到 Worker
 * @param manager ThreadPoolManager 指针
 * @param fd 新连接的文件描述符
 * @return 0 成功，-1 失败
 */
int thread_pool_manager_dispatch(ThreadPoolManager *manager, int fd);

/**
 * 设置分发策略
 * @param manager ThreadPoolManager 指针
 * @param strategy 分发策略类型
 * @return 0 成功，-1 失败
 */
int thread_pool_manager_set_strategy(ThreadPoolManager *manager, DispatchStrategy strategy);

/**
 * 获取当前分发策略
 * @param manager ThreadPoolManager 指针
 * @return 当前分发策略
 */
DispatchStrategy thread_pool_manager_get_strategy(ThreadPoolManager *manager);

/**
 * 获取 Worker 数量
 * @param manager ThreadPoolManager 指针
 * @return Worker 数量
 */
int thread_pool_manager_get_worker_count(ThreadPoolManager *manager);

/**
 * 获取指定 Worker 的当前连接数
 * @param manager ThreadPoolManager 指针
 * @param worker_index Worker 索引
 * @return 连接数，-1 表示错误
 */
int thread_pool_manager_get_worker_connections(ThreadPoolManager *manager, int worker_index);

/**
 * 获取负载均衡统计信息
 * @param manager ThreadPoolManager 指针
 * @param connections 各 Worker 连接数数组（需预分配）
 * @param count Worker 数量
 * @return 0 成功，-1 失败
 */
int thread_pool_manager_get_balance_stats(ThreadPoolManager *manager, int *connections, int count);

/**
 * 获取指定 Worker 的 EventLoop
 * @param manager ThreadPoolManager 指针
 * @param worker_index Worker 索引
 * @return EventLoop 指针，失败返回 NULL
 */
EventLoop *thread_pool_manager_get_worker_eventloop(ThreadPoolManager *manager, int worker_index);

/* ========== WorkerThread API ========== */

/**
 * 创建 Worker 线程
 * @param max_events 最大事件数
 * @param worker_id Worker ID
 * @return WorkerThread 指针，失败返回 NULL
 */
WorkerThread *worker_thread_create(int max_events, int worker_id);

/**
 * 销毁 Worker 线程
 * @param worker WorkerThread 指针
 */
void worker_thread_destroy(WorkerThread *worker);

/**
 * 启动 Worker 线程
 * @param worker WorkerThread 指针
 * @return 0 成功，-1 失败
 */
int worker_thread_start(WorkerThread *worker);

/**
 * 停止 Worker 线程
 * @param worker WorkerThread 指针
 */
void worker_thread_stop(WorkerThread *worker);

/**
 * 获取 Worker 的通知文件描述符（用于分发连接）
 * @param worker WorkerThread 指针
 * @return 通知 fd，-1 表示错误
 */
int worker_thread_get_notify_fd(WorkerThread *worker);

/**
 * 获取 Worker 的当前连接数
 * @param worker WorkerThread 指针
 * @return 连接数
 */
int worker_thread_get_connection_count(WorkerThread *worker);

/**
 * 增加 Worker 连接计数
 * @param worker WorkerThread 指针
 */
void worker_thread_increment_connections(WorkerThread *worker);

/**
 * 减少 Worker 连接计数
 * @param worker WorkerThread 指针
 */
void worker_thread_decrement_connections(WorkerThread *worker);

/**
 * 获取 Worker 的 EventLoop
 * @param worker WorkerThread 指针
 * @return EventLoop 指针
 */
EventLoop *worker_thread_get_eventloop(WorkerThread *worker);

/**
 * 获取 Worker ID
 * @param worker WorkerThread 指针
 * @return Worker ID
 */
int worker_thread_get_id(WorkerThread *worker);

/**
 * 创建带 ConnectionPool 的 Worker 线程
 * @param max_events 最大事件数
 * @param worker_id Worker ID
 * @param pool_capacity 连接池容量
 * @return WorkerThread 指针，失败返回 NULL
 */
WorkerThread *worker_thread_create_with_pool(int max_events, int worker_id, int pool_capacity);

/**
 * 获取 Worker 的 ConnectionPool
 * @param worker WorkerThread 指针
 * @return ConnectionPool 指针
 */
ConnectionPool *worker_thread_get_connection_pool(WorkerThread *worker);

/**
 * 设置 Worker 的共享 Router
 * @param worker WorkerThread 指针
 * @param router Router 指针
 */
void worker_thread_set_router(WorkerThread *worker, Router *router);

/**
 * 获取 Worker 的共享 Router
 * @param worker WorkerThread 指针
 * @return Router 指针
 */
Router *worker_thread_get_router(WorkerThread *worker);

/**
 * 设置 Worker 的连接处理回调
 * @param worker WorkerThread 指针
 * @param handler 连接处理回调
 * @param user_data 用户数据
 */
void worker_thread_set_connection_handler(WorkerThread *worker, ConnectionHandler handler, void *user_data);

/**
 * 获取指定索引的 Worker
 * @param manager ThreadPoolManager 指针
 * @param index Worker 索引
 * @return WorkerThread 指针，失败返回 NULL
 */
WorkerThread *thread_pool_manager_get_worker(ThreadPoolManager *manager, int index);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_THREAD_POOL_MANAGER_H */