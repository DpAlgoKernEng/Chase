/**
 * @file    connection_pool.c
 * @brief   连接池实现，管理 Connection 的预分配和复用
 *
 * @details
 *          - 预分配固定数量的 Connection
 *          - 支持动态扩容（阈值触发）
 *          - 惰性释放临时分配的 Connection
 *          - 使用内部 PoolEntry 结构管理链表和池字段
 *
 * @layer   Core Layer
 *
 * @depends connection, buffer
 * @usedby  server, examples
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "connection_pool.h"
#include "connection.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 惰性释放延迟（毫秒） */
#define LAZY_RELEASE_DELAY_MS 60000

/* 阈值比例（10%） */
#define EXPAND_THRESHOLD_RATIO 0.1f

/* PoolEntry 结构体 - 池管理字段独立封装 */
typedef struct PoolEntry {
    Connection *conn;           /* 关联的 Connection */
    struct PoolEntry *next;     /* 链表下一个指针 */
    struct PoolEntry *prev;     /* 链表上一个指针 */
    uint64_t release_time;      /* 惰性释放时间记录 */
    int is_temp_allocated;      /* 标记：0 = 预分配, 1 = 临时 malloc */
} PoolEntry;

/* ConnectionPool 内部结构 */
struct ConnectionPool {
    /* 预分配的 PoolEntry 指针数组 */
    PoolEntry **preallocated;
    int base_capacity;

    /* 空闲链表 */
    PoolEntry *free_list_head;
    PoolEntry *free_list_tail;
    int free_count;

    /* 活跃链表 */
    PoolEntry *active_list_head;
    int active_count;

    /* 惰性释放队列 */
    PoolEntry *lazy_release_queue;
    int lazy_release_count;

    /* 统计信息 */
    int total_allocated;
    int temp_allocated;
};

/* 获取当前时间（毫秒） */
static uint64_t get_current_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ========== API 实现 ========== */

ConnectionPool *connection_pool_create(int base_capacity) {
    if (base_capacity <= 0) {
        return NULL;
    }

    ConnectionPool *pool = malloc(sizeof(ConnectionPool));
    if (!pool) {
        return NULL;
    }

    /* 分配 PoolEntry 指针数组 */
    pool->preallocated = malloc(base_capacity * sizeof(PoolEntry *));
    if (!pool->preallocated) {
        free(pool);
        return NULL;
    }
    pool->base_capacity = base_capacity;

    /* 预分配每个 PoolEntry 和 Connection */
    for (int i = 0; i < base_capacity; i++) {
        /* 创建 PoolEntry */
        pool->preallocated[i] = malloc(sizeof(PoolEntry));
        if (!pool->preallocated[i]) {
            /* 清理已创建的 */
            for (int j = 0; j < i; j++) {
                connection_destroy(pool->preallocated[j]->conn);
                free(pool->preallocated[j]);
            }
            free(pool->preallocated);
            free(pool);
            return NULL;
        }

        /* 创建 Connection（fd=-1 表示未使用，无回调） */
        pool->preallocated[i]->conn = connection_create(-1, NULL, NULL);
        if (!pool->preallocated[i]->conn) {
            free(pool->preallocated[i]);
            for (int j = 0; j < i; j++) {
                connection_destroy(pool->preallocated[j]->conn);
                free(pool->preallocated[j]);
            }
            free(pool->preallocated);
            free(pool);
            return NULL;
        }

        /* 设置池管理字段 */
        connection_set_state(pool->preallocated[i]->conn, CONN_STATE_CLOSED);
        pool->preallocated[i]->is_temp_allocated = 0;
        pool->preallocated[i]->release_time = 0;
        pool->preallocated[i]->next = NULL;
        pool->preallocated[i]->prev = NULL;
    }

    /* 构建空闲链表（双向链表） */
    pool->free_list_head = pool->preallocated[0];
    pool->free_list_tail = pool->preallocated[base_capacity - 1];

    for (int i = 0; i < base_capacity - 1; i++) {
        pool->preallocated[i]->next = pool->preallocated[i + 1];
        pool->preallocated[i + 1]->prev = pool->preallocated[i];
    }
    pool->preallocated[0]->prev = NULL;
    pool->preallocated[base_capacity - 1]->next = NULL;

    pool->free_count = base_capacity;
    pool->active_list_head = NULL;
    pool->active_count = 0;
    pool->lazy_release_queue = NULL;
    pool->lazy_release_count = 0;
    pool->total_allocated = base_capacity;
    pool->temp_allocated = 0;

    return pool;
}

void connection_pool_destroy(ConnectionPool *pool) {
    if (!pool) {
        return;
    }

    /* 释放惰性释放队列中的临时 PoolEntry */
    PoolEntry *entry = pool->lazy_release_queue;
    while (entry) {
        PoolEntry *next = entry->next;
        connection_destroy(entry->conn);
        free(entry);
        entry = next;
    }

    /* 释放活跃链表中的临时 PoolEntry */
    entry = pool->active_list_head;
    while (entry) {
        PoolEntry *next = entry->next;
        if (entry->is_temp_allocated) {
            connection_destroy(entry->conn);
            free(entry);
        }
        entry = next;
    }

    /* 释放所有预分配的 PoolEntry */
    for (int i = 0; i < pool->base_capacity; i++) {
        connection_destroy(pool->preallocated[i]->conn);
        free(pool->preallocated[i]);
    }

    free(pool->preallocated);
    free(pool);
}

int connection_pool_should_expand(ConnectionPool *pool) {
    if (!pool || pool->base_capacity == 0) {
        return 1;  /* 无池或空池，需要扩容 */
    }
    return pool->free_count < (int)(pool->base_capacity * EXPAND_THRESHOLD_RATIO);
}

Connection *connection_pool_get(ConnectionPool *pool) {
    if (!pool) {
        return NULL;
    }

    PoolEntry *entry;

    /* 检查是否需要扩容（阈值触发） */
    if (connection_pool_should_expand(pool)) {
        /* 触发临时 malloc */
        entry = malloc(sizeof(PoolEntry));
        if (!entry) {
            return NULL;
        }

        entry->conn = connection_create(-1, NULL, NULL);
        if (!entry->conn) {
            free(entry);
            return NULL;
        }

        entry->is_temp_allocated = 1;
        entry->release_time = 0;
        entry->next = NULL;
        entry->prev = NULL;

        pool->temp_allocated++;
        pool->total_allocated++;

        /* 加入活跃链表 */
        entry->next = pool->active_list_head;
        entry->prev = NULL;
        if (pool->active_list_head) {
            pool->active_list_head->prev = entry;
        }
        pool->active_list_head = entry;
        pool->active_count++;

        return entry->conn;
    }

    /* 从空闲链表获取（O(1)） */
    entry = pool->free_list_head;
    PoolEntry *next_entry = entry->next;
    pool->free_list_head = next_entry;
    if (pool->free_list_head) {
        pool->free_list_head->prev = NULL;
    } else {
        pool->free_list_tail = NULL;
    }
    pool->free_count--;

    /* 加入活跃链表 */
    entry->next = pool->active_list_head;
    entry->prev = NULL;
    if (pool->active_list_head) {
        pool->active_list_head->prev = entry;
    }
    pool->active_list_head = entry;
    pool->active_count++;

    return entry->conn;
}

void connection_pool_release(ConnectionPool *pool, Connection *conn) {
    if (!pool || !conn) {
        return;
    }

    /* 查找对应的 PoolEntry */
    PoolEntry *entry = pool->active_list_head;
    while (entry) {
        if (entry->conn == conn) {
            break;
        }
        entry = entry->next;
    }

    if (!entry) {
        /* 未找到，可能是无效连接 */
        return;
    }

    PoolEntry *prev_entry = entry->prev;
    PoolEntry *next_entry = entry->next;

    /* 从活跃链表移除 */
    if (prev_entry) {
        prev_entry->next = next_entry;
    } else {
        pool->active_list_head = next_entry;
    }
    if (next_entry) {
        next_entry->prev = prev_entry;
    }
    pool->active_count--;

    /* 重置 Connection 状态 */
    connection_set_state(conn, CONN_STATE_CLOSED);

    /* 根据分配类型路由 */
    if (entry->is_temp_allocated == 0) {
        /* 预分配：加入空闲链表 */
        entry->next = pool->free_list_head;
        entry->prev = NULL;
        if (pool->free_list_head) {
            pool->free_list_head->prev = entry;
        }
        pool->free_list_head = entry;
        pool->free_count++;
    } else {
        /* 临时 malloc：加入惰性释放队列 */
        entry->release_time = get_current_ms();
        entry->next = pool->lazy_release_queue;
        entry->prev = NULL;
        if (pool->lazy_release_queue) {
            pool->lazy_release_queue->prev = entry;
        }
        pool->lazy_release_queue = entry;
        pool->lazy_release_count++;
    }
}

void connection_pool_lazy_release_check(ConnectionPool *pool) {
    if (!pool) {
        return;
    }

    /* 由 Server EventLoop 定时器调用（每 10 秒） */
    uint64_t now = get_current_ms();
    PoolEntry *entry = pool->lazy_release_queue;

    while (entry) {
        PoolEntry *next_entry = entry->next;
        uint64_t release_time = entry->release_time;

        if (now - release_time > LAZY_RELEASE_DELAY_MS) {
            PoolEntry *prev_entry = entry->prev;

            /* 从惰性释放队列移除 */
            if (prev_entry) {
                prev_entry->next = next_entry;
            } else {
                pool->lazy_release_queue = next_entry;
            }
            if (next_entry) {
                next_entry->prev = prev_entry;
            }

            /* 释放内存 */
            connection_destroy(entry->conn);
            free(entry);
            pool->temp_allocated--;
            pool->total_allocated--;
            pool->lazy_release_count--;
        }

        entry = next_entry;
    }
}

PoolStats connection_pool_get_stats(ConnectionPool *pool) {
    PoolStats stats = {0};

    if (!pool) {
        return stats;
    }

    stats.base_capacity = pool->base_capacity;
    stats.free_count = pool->free_count;
    stats.active_count = pool->active_count;
    stats.temp_allocated = pool->temp_allocated;
    stats.lazy_release_count = pool->lazy_release_count;

    int total = pool->base_capacity + pool->temp_allocated;
    if (total > 0) {
        stats.utilization = (float)pool->active_count / total;
    } else {
        stats.utilization = 0.0f;
    }

    return stats;
}

int connection_pool_get_base_capacity(ConnectionPool *pool) {
    if (!pool) return 0;
    return pool->base_capacity;
}

int connection_pool_get_free_count(ConnectionPool *pool) {
    if (!pool) return 0;
    return pool->free_count;
}