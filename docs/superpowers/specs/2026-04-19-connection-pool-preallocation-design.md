# 连接池预分配设计

**日期**: 2026-04-19
**项目**: Chase (HTTP Server Library)
**模块**: connection pool optimization (连接池优化)
**版本**: 1.0

---

## 概述

在每个 Worker 线程启动时预分配 Connection 结构池，优化连接对象分配：
- 减少 accept 时 malloc 开销
- 减少内存碎片
- 提高内存占用可预测性

---

## 设计目标

1. **减少分配延迟**：避免 accept 时 malloc 开销
2. **减少内存碎片**：预分配大块连续内存
3. **内存占用可预测**：固定基础容量 + 监控的动态扩容

---

## 核心参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| base_capacity | max_connections / worker_count | 每个 Worker 基础连接池容量 |
| expand_threshold | 10% | free_count < 10% 时触发临时 malloc |
| lazy_release_delay | 60s | 临时 malloc 的连接空闲 60 秒后释放 |

---

## 数据结构

```c
typedef struct ConnectionPool {
    // 预分配数组（固定内存块）
    Connection* preallocated;      // malloc(base_capacity * sizeof(Connection))
    int base_capacity;
    
    // 空闲链表
    Connection* free_list_head;
    Connection* free_list_tail;
    int free_count;
    
    // 活跃链表
    Connection* active_list_head;
    int active_count;
    
    // 惰性释放队列
    Connection* lazy_release_queue;
    int lazy_release_count;
    
    // 统计信息
    int total_allocated;           // 总分配数（含临时 malloc）
    int temp_allocated;            // 仅临时 malloc 数量
} ConnectionPool;

// Connection 结构扩展
struct Connection {
    // ... 现有字段 ...
    
    // 池管理（内部使用）
    Connection* next;              // 链表下一个指针
    Connection* prev;              // 链表上一个指针
    uint64_t release_time;         // 惰性释放时间记录
    int is_temp_allocated;         // 标记：0 = 预分配, 1 = 临时 malloc
};
```

---

## 生命周期流程

### 1. Worker 启动时（预分配）

```c
ConnectionPool* connection_pool_create(int base_capacity) {
    ConnectionPool* pool = malloc(sizeof(ConnectionPool));
    
    // 1. 分配固定内存块
    pool->preallocated = malloc(base_capacity * sizeof(Connection));
    pool->base_capacity = base_capacity;
    
    // 2. 初始化每个 Connection
    for (int i = 0; i < base_capacity; i++) {
        Connection* conn = &pool->preallocated[i];
        conn->fd = -1;
        conn->state = CONN_STATE_CLOSED;
        conn->is_temp_allocated = 0;
        conn->next = NULL;
        conn->prev = NULL;
    }
    
    // 3. 全部加入空闲链表
    pool->free_list_head = &pool->preallocated[0];
    pool->free_list_tail = &pool->preallocated[base_capacity - 1];
    
    for (int i = 0; i < base_capacity - 1; i++) {
        pool->preallocated[i].next = &pool->preallocated[i + 1];
    }
    
    pool->free_count = base_capacity;
    pool->active_count = 0;
    pool->temp_allocated = 0;
    pool->lazy_release_count = 0;
    
    return pool;
}
```

### 2. Accept 新连接时（从池获取）

```c
Connection* connection_pool_get(ConnectionPool* pool) {
    Connection* conn;
    
    // 1. 检查是否需要扩容（阈值触发）
    if (should_expand(pool)) {
        // 触发临时 malloc
        conn = malloc(sizeof(Connection));
        conn->is_temp_allocated = 1;
        conn->fd = -1;
        conn->state = CONN_STATE_CLOSED;
        pool->temp_allocated++;
        pool->total_allocated++;
        
        // 加入活跃链表
        conn->next = pool->active_list_head;
        conn->prev = NULL;
        if (pool->active_list_head) {
            pool->active_list_head->prev = conn;
        }
        pool->active_list_head = conn;
        pool->active_count++;
        
        return conn;
    }
    
    // 2. 从空闲链表获取（O(1)）
    conn = pool->free_list_head;
    pool->free_list_head = conn->next;
    if (pool->free_list_head) {
        pool->free_list_head->prev = NULL;
    } else {
        pool->free_list_tail = NULL;
    }
    pool->free_count--;
    
    // 3. 加入活跃链表
    conn->next = pool->active_list_head;
    conn->prev = NULL;
    if (pool->active_list_head) {
        pool->active_list_head->prev = conn;
    }
    pool->active_list_head = conn;
    pool->active_count++;
    
    return conn;
}

// 阈值检查
int should_expand(ConnectionPool* pool) {
    return pool->free_count < pool->base_capacity * 0.1;
}
```

### 3. 连接关闭时（释放到池）

```c
void connection_pool_release(ConnectionPool* pool, Connection* conn) {
    // 1. 从活跃链表移除
    if (conn->prev) {
        conn->prev->next = conn->next;
    } else {
        pool->active_list_head = conn->next;
    }
    if (conn->next) {
        conn->next->prev = conn->prev;
    }
    pool->active_count--;
    
    // 2. 重置 Connection 状态
    conn->fd = -1;
    conn->state = CONN_STATE_CLOSED;
    
    // 3. 根据分配类型路由
    if (conn->is_temp_allocated == 0) {
        // 预分配：加入空闲链表
        conn->next = pool->free_list_head;
        conn->prev = NULL;
        if (pool->free_list_head) {
            pool->free_list_head->prev = conn;
        }
        pool->free_list_head = conn;
        pool->free_count++;
    } else {
        // 临时 malloc：加入惰性释放队列
        conn->release_time = get_current_ms();
        conn->next = pool->lazy_release_queue;
        conn->prev = NULL;
        if (pool->lazy_release_queue) {
            pool->lazy_release_queue->prev = conn;
        }
        pool->lazy_release_queue = conn;
        pool->lazy_release_count++;
    }
}
```

### 4. 惰性释放定时器（定期检查）

```c
void connection_pool_lazy_release_check(ConnectionPool* pool) {
    // 由 Worker EventLoop 定时器调用（每 10 秒）
    uint64_t now = get_current_ms();
    Connection* conn = pool->lazy_release_queue;
    
    while (conn) {
        Connection* next = conn->next;
        
        if (now - conn->release_time > 60000) {  // 60 秒
            // 从惰性释放队列移除
            if (conn->prev) {
                conn->prev->next = conn->next;
            } else {
                pool->lazy_release_queue = conn->next;
            }
            if (conn->next) {
                conn->next->prev = conn->prev;
            }
            
            // 释放内存
            free(conn);
            pool->temp_allocated--;
            pool->total_allocated--;
            pool->lazy_release_count--;
        }
        
        conn = next;
    }
}
```

---

## 配置扩展

```c
struct HttpConfig {
    // ... 现有字段 ...
    
    // 连接池配置（可选覆盖）
    int connection_pool_size_per_worker;  // 0 = 自动计算
    float connection_pool_expand_threshold; // 默认 0.1 (10%)
    int connection_pool_lazy_release_delay_ms; // 默认 60000
};
```

**自动计算公式**：

```c
int calculate_base_capacity(HttpConfig* cfg, int worker_count) {
    if (cfg->connection_pool_size_per_worker > 0) {
        return cfg->connection_pool_size_per_worker;  // 用户覆盖
    }
    return cfg->max_connections / worker_count;  // 自动计算
}
```

---

## 监控接口

```c
// 获取池统计信息
typedef struct PoolStats {
    int base_capacity;
    int free_count;
    int active_count;
    int temp_allocated;
    int lazy_release_count;
    float utilization;  // active_count / (base_capacity + temp_allocated)
} PoolStats;

PoolStats connection_pool_get_stats(ConnectionPool* pool);

// 示例监控输出：
// Worker-0 pool: free=2400, active=100, temp=0, lazy=0, utilization=4.0%
// Worker-1 pool: free=2450, active=50, temp=5, lazy=3, utilization=2.0%
```

---

## 测试验收标准

| 测试项 | 验收标准 |
|--------|----------|
| 预分配正确 | Worker 启动后 free_count == base_capacity |
| 获取/释放正确 | 1000 次 accept/close 循环，free_count 恢复初始值 |
| 阈值触发扩容 | free_count < 10% 时 temp_allocated 增加 |
| 惰性释放生效 | 临时连接空闲 60 秒后 temp_allocated 减少 |
| 性能提升 | accept 延迟对比 malloc 版本降低 > 50% |

---

## 集成点

1. **Phase 1**：添加 ConnectionPool 结构和池管理字段到 Connection
2. **Phase 2**：WorkerThread 启动时调用 connection_pool_create()
3. **Phase 2**：Worker add_connection 使用 connection_pool_get()
4. **Phase 2**：Connection close 调用 connection_pool_release()
5. **Phase 2**：Worker EventLoop 添加 lazy_release_check 定时器（10 秒间隔）

---

## 内存开销分析

| 项目 | 大小 | 计算 |
|------|------|------|
| Connection 结构 | ~200 字节 | fd, state, buffers, ssl, timers, 池字段 |
| 预分配块 | base_capacity * 200 | 如 2500 * 200 = 500KB/Worker |
| 链表指针 | 每个 conn 2 个指针 | 已包含在 Connection 结构中 |
| 每个 Worker 总计 | ~500KB + temp | 可预测、稳定 |

**对比 malloc-per-accept**：

| 场景 | malloc-per-accept | 池预分配 |
|------|-------------------|----------|
| 10000 连接峰值 | 10000 次 malloc | 4 * 2500 预分配 |
| 内存碎片 | 高 | 低（大块内存） |
| accept 延迟 | malloc 开销 | O(1) 指针操作 |
| 内存可预测性 | 不可预测 | 固定基础 + 监控临时 |

---

## 风险分析

| 风险 | 缓解措施 |
|------|----------|
| 池耗尽 | 阈值提前扩容 + 临时 malloc 兜底 |
| 内存浪费 | 自动计算 base_capacity，惰性释放临时连接 |
| 惰性释放定时器开销 | 10 秒间隔，O(n) 扫描但 n 很小（temp_allocated） |

---

## 总结

本设计提供简单高效的连接池：
- O(1) 获取/释放操作
- 可预测内存占用（固定基础 + 监控临时）
- 阈值提前扩容（避免边界竞争）
- 惰性释放临时连接（内存长期稳定）

实现简单（约 100 行代码），与现有 Worker/Connection 架构无缝集成。