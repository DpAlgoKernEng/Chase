#include "connection_pool.h"
#include "connection.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 惰性释放延迟（毫秒） */
#define LAZY_RELEASE_DELAY_MS 60000

/* 阈值比例（10%） */
#define EXPAND_THRESHOLD_RATIO 0.1f

/* ConnectionPool 内部结构 */
struct ConnectionPool {
    /* 预分配的 Connection 指针数组 */
    Connection **preallocated;
    int base_capacity;

    /* 空闲链表 */
    Connection *free_list_head;
    Connection *free_list_tail;
    int free_count;

    /* 活跃链表 */
    Connection *active_list_head;
    int active_count;

    /* 惰性释放队列 */
    Connection *lazy_release_queue;
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

    /* 分配 Connection 指针数组 */
    pool->preallocated = malloc(base_capacity * sizeof(Connection *));
    if (!pool->preallocated) {
        free(pool);
        return NULL;
    }
    pool->base_capacity = base_capacity;

    /* 预分配每个 Connection */
    for (int i = 0; i < base_capacity; i++) {
        /* 使用 connection_create 创建连接（fd=-1 表示未使用） */
        pool->preallocated[i] = connection_create(-1, NULL);
        if (!pool->preallocated[i]) {
            /* 清理已创建的连接 */
            for (int j = 0; j < i; j++) {
                connection_destroy(pool->preallocated[j]);
            }
            free(pool->preallocated);
            free(pool);
            return NULL;
        }
        /* 设置池管理字段 */
        connection_set_state(pool->preallocated[i], CONN_STATE_CLOSED);
        connection_set_temp_allocated(pool->preallocated[i], 0);
        connection_set_release_time(pool->preallocated[i], 0);
        connection_set_next(pool->preallocated[i], NULL);
        connection_set_prev(pool->preallocated[i], NULL);
    }

    /* 构建空闲链表（双向链表） */
    pool->free_list_head = pool->preallocated[0];
    pool->free_list_tail = pool->preallocated[base_capacity - 1];

    for (int i = 0; i < base_capacity - 1; i++) {
        connection_set_next(pool->preallocated[i], pool->preallocated[i + 1]);
        connection_set_prev(pool->preallocated[i + 1], pool->preallocated[i]);
    }
    connection_set_prev(pool->preallocated[0], NULL);
    connection_set_next(pool->preallocated[base_capacity - 1], NULL);

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

    /* 释放惰性释放队列中的临时连接 */
    Connection *conn = pool->lazy_release_queue;
    while (conn) {
        Connection *next = connection_get_next(conn);
        connection_destroy(conn);
        conn = next;
    }

    /* 释放活跃链表中的临时连接 */
    conn = pool->active_list_head;
    while (conn) {
        Connection *next = connection_get_next(conn);
        if (connection_is_temp_allocated(conn)) {
            connection_destroy(conn);
        }
        conn = next;
    }

    /* 释放所有预分配的连接 */
    for (int i = 0; i < pool->base_capacity; i++) {
        connection_destroy(pool->preallocated[i]);
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

    Connection *conn;

    /* 检查是否需要扩容（阈值触发） */
    if (connection_pool_should_expand(pool)) {
        /* 触发临时 malloc */
        conn = connection_create(-1, NULL);  /* fd=-1, loop=NULL */
        if (!conn) {
            return NULL;
        }
        connection_set_temp_allocated(conn, 1);
        connection_set_release_time(conn, 0);
        pool->temp_allocated++;
        pool->total_allocated++;

        /* 加入活跃链表 */
        connection_set_next(conn, pool->active_list_head);
        connection_set_prev(conn, NULL);
        if (pool->active_list_head) {
            connection_set_prev(pool->active_list_head, conn);
        }
        pool->active_list_head = conn;
        pool->active_count++;

        return conn;
    }

    /* 从空闲链表获取（O(1)） */
    conn = pool->free_list_head;
    Connection *next_conn = connection_get_next(conn);
    pool->free_list_head = next_conn;
    if (pool->free_list_head) {
        connection_set_prev(pool->free_list_head, NULL);
    } else {
        pool->free_list_tail = NULL;
    }
    pool->free_count--;

    /* 加入活跃链表 */
    connection_set_next(conn, pool->active_list_head);
    connection_set_prev(conn, NULL);
    if (pool->active_list_head) {
        connection_set_prev(pool->active_list_head, conn);
    }
    pool->active_list_head = conn;
    pool->active_count++;

    return conn;
}

void connection_pool_release(ConnectionPool *pool, Connection *conn) {
    if (!pool || !conn) {
        return;
    }

    Connection *prev_conn = connection_get_prev(conn);
    Connection *next_conn = connection_get_next(conn);

    /* 从活跃链表移除 */
    if (prev_conn) {
        connection_set_next(prev_conn, next_conn);
    } else {
        pool->active_list_head = next_conn;
    }
    if (next_conn) {
        connection_set_prev(next_conn, prev_conn);
    }
    pool->active_count--;

    /* 重置 Connection 状态 */
    connection_set_state(conn, CONN_STATE_CLOSED);

    /* 根据分配类型路由 */
    if (connection_is_temp_allocated(conn) == 0) {
        /* 预分配：加入空闲链表 */
        connection_set_next(conn, pool->free_list_head);
        connection_set_prev(conn, NULL);
        if (pool->free_list_head) {
            connection_set_prev(pool->free_list_head, conn);
        }
        pool->free_list_head = conn;
        pool->free_count++;
    } else {
        /* 临时 malloc：加入惰性释放队列 */
        connection_set_release_time(conn, get_current_ms());
        connection_set_next(conn, pool->lazy_release_queue);
        connection_set_prev(conn, NULL);
        if (pool->lazy_release_queue) {
            connection_set_prev(pool->lazy_release_queue, conn);
        }
        pool->lazy_release_queue = conn;
        pool->lazy_release_count++;
    }
}

void connection_pool_lazy_release_check(ConnectionPool *pool) {
    if (!pool) {
        return;
    }

    /* 由 Worker EventLoop 定时器调用（每 10 秒） */
    uint64_t now = get_current_ms();
    Connection *conn = pool->lazy_release_queue;

    while (conn) {
        Connection *next_conn = connection_get_next(conn);
        uint64_t release_time = connection_get_release_time(conn);

        if (now - release_time > LAZY_RELEASE_DELAY_MS) {
            Connection *prev_conn = connection_get_prev(conn);

            /* 从惰性释放队列移除 */
            if (prev_conn) {
                connection_set_next(prev_conn, next_conn);
            } else {
                pool->lazy_release_queue = next_conn;
            }
            if (next_conn) {
                connection_set_prev(next_conn, prev_conn);
            }

            /* 释放内存 */
            connection_destroy(conn);
            pool->temp_allocated--;
            pool->total_allocated--;
            pool->lazy_release_count--;
        }

        conn = next_conn;
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