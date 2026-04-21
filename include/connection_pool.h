/**
 * @file    connection_pool.h
 * @brief   连接池管理，预分配和复用 Connection 对象
 *
 * @details
 *          - 预分配固定数量的 Connection，避免频繁 malloc
 *          - 支持动态扩容（阈值触发临时分配）
 *          - 惰性释放策略（60秒延迟释放临时连接）
 *          - 双向链表管理空闲/活跃连接
 *
 * @layer   Core Layer
 *
 * @depends connection
 * @usedby  server, examples
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#ifndef CHASE_CONNECTION_POOL_H
#define CHASE_CONNECTION_POOL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Connection 结构体前向声明 */
typedef struct Connection Connection;

/* 连接池结构体（不透明指针） */
typedef struct ConnectionPool ConnectionPool;

/* 池统计信息结构 */
typedef struct PoolStats {
    int base_capacity;      /* 预分配容量 */
    int free_count;         /* 空闲连接数 */
    int active_count;       /* 活跃连接数 */
    int temp_allocated;     /* 临时分配的连接数 */
    int lazy_release_count; /* 惰性释放队列中的连接数 */
    float utilization;      /* 利用率（active / total） */
} PoolStats;

/**
 * 创建连接池
 * @param base_capacity 预分配的连接数量
 * @return ConnectionPool 指针，失败返回 NULL
 */
ConnectionPool *connection_pool_create(int base_capacity);

/**
 * 销毁连接池
 * @param pool ConnectionPool 指针
 */
void connection_pool_destroy(ConnectionPool *pool);

/**
 * 从池获取 Connection
 * @param pool ConnectionPool 指针
 * @return Connection 指针，失败返回 NULL
 */
Connection *connection_pool_get(ConnectionPool *pool);

/**
 * 释放 Connection 到池
 * @param pool ConnectionPool 指针
 * @param conn Connection 指针
 */
void connection_pool_release(ConnectionPool *pool, Connection *conn);

/**
 * 惰性释放检查（定时器调用）
 * @param pool ConnectionPool 指针
 */
void connection_pool_lazy_release_check(ConnectionPool *pool);

/**
 * 获取池统计信息
 * @param pool ConnectionPool 指针
 * @return PoolStats 结构
 */
PoolStats connection_pool_get_stats(ConnectionPool *pool);

/**
 * 检查是否需要扩容（内部函数）
 * @param pool ConnectionPool 指针
 * @return 1 需要扩容，0 不需要
 */
int connection_pool_should_expand(ConnectionPool *pool);

/**
 * 获取池的基础容量
 * @param pool ConnectionPool 指针
 * @return 基础容量
 */
int connection_pool_get_base_capacity(ConnectionPool *pool);

/**
 * 获取池的空闲连接数
 * @param pool ConnectionPool 指针
 * @return 空闲连接数
 */
int connection_pool_get_free_count(ConnectionPool *pool);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_CONNECTION_POOL_H */