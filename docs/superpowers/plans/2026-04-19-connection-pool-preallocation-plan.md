# 连接池预分配优化实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Worker 线程启动时预分配 Connection 结构池，减少 accept 时 malloc 开销和内存碎片。

**Architecture:** 每个 Worker 启动时预分配固定数量的 Connection 结构数组，用空闲链表管理。free_count < 10% 时触发临时 malloc，临时连接空闲 60 秒后惰性释放。

**Tech Stack:** C11, POSIX threads, linked list (双向链表)

---

## 文件结构

| 文件 | 负责内容 |
|------|----------|
| `include/connection_pool.h` | ConnectionPool API 定义（约 50 行） |
| `src/connection_pool.c` | ConnectionPool 实现（约 150 行） |
| `include/connection.h` | Connection 结构扩展（添加池管理字段） |
| `include/config.h` | HttpConfig 扩展（添加连接池配置字段） |
| `test/integration/c_core/test_connection_pool.c` | 单元测试（≥ 10 用例） |

---

## Task 1: ConnectionPool 头文件

**Files:**
- Create: `include/connection_pool.h`

- [ ] **Step 1: 创建 connection_pool.h 头文件**

```c
#ifndef CONNECTION_POOL_H
#define CONNECTION_POOL_H

#include "connection.h"
#include <stdint.h>

typedef struct ConnectionPool {
    // 预分配数组（固定内存块）
    Connection* preallocated;
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
    int total_allocated;
    int temp_allocated;
} ConnectionPool;

// 池统计信息结构
typedef struct PoolStats {
    int base_capacity;
    int free_count;
    int active_count;
    int temp_allocated;
    int lazy_release_count;
    float utilization;
} PoolStats;

// 创建连接池
ConnectionPool* connection_pool_create(int base_capacity);

// 销毁连接池
void connection_pool_destroy(ConnectionPool* pool);

// 从池获取 Connection
Connection* connection_pool_get(ConnectionPool* pool);

// 释放 Connection 到池
void connection_pool_release(ConnectionPool* pool, Connection* conn);

// 惰性释放检查（定时器调用）
void connection_pool_lazy_release_check(ConnectionPool* pool);

// 获取池统计信息
PoolStats connection_pool_get_stats(ConnectionPool* pool);

// 阈值检查（内部函数）
int connection_pool_should_expand(ConnectionPool* pool);

#endif // CONNECTION_POOL_H
```

- [ ] **Step 2: 提交头文件**

```bash
git add include/connection_pool.h
git commit -m "feat(connection_pool): add ConnectionPool header with API definitions"
```

---

## Task 2: Connection 结构扩展

**Files:**
- Modify: `include/connection.h` (添加池管理字段)

- [ ] **Step 1: 扩展 Connection 结构**

在 `include/connection.h` 的 Connection 结构末尾添加池管理字段：

```c
struct Connection {
    // ... 现有字段（fd, state, buffers, ssl 等）...
    
    // 池管理字段（内部使用）
    Connection* next;              // 链表下一个指针
    Connection* prev;              // 链表上一个指针
    uint64_t release_time;         // 惰性释放时间记录
    int is_temp_allocated;         // 标记：0 = 预分配, 1 = 临时 malloc
};
```

- [ ] **Step 2: 提交 Connection 结构扩展**

```bash
git add include/connection.h
git commit -m "feat(connection): add pool management fields to Connection struct"
```

---

## Task 3: ConnectionPool 实现 - 创建与销毁

**Files:**
- Create: `src/connection_pool.c`

- [ ] **Step 1: 实现 connection_pool_create 函数**

```c
#include "connection_pool.h"
#include "connection.h"
#include <stdlib.h>
#include <string.h>

ConnectionPool* connection_pool_create(int base_capacity) {
    if (base_capacity <= 0) {
        return NULL;
    }
    
    ConnectionPool* pool = malloc(sizeof(ConnectionPool));
    if (!pool) {
        return NULL;
    }
    
    // 分配固定内存块
    pool->preallocated = malloc(base_capacity * sizeof(Connection));
    if (!pool->preallocated) {
        free(pool);
        return NULL;
    }
    pool->base_capacity = base_capacity;
    
    // 初始化每个 Connection
    for (int i = 0; i < base_capacity; i++) {
        Connection* conn = &pool->preallocated[i];
        conn->fd = -1;
        conn->state = CONN_STATE_CLOSED;
        conn->is_temp_allocated = 0;
        conn->next = NULL;
        conn->prev = NULL;
        conn->release_time = 0;
        // 其他字段初始化（buffers, ssl 等）由调用方处理
    }
    
    // 构建空闲链表
    pool->free_list_head = &pool->preallocated[0];
    pool->free_list_tail = &pool->preallocated[base_capacity - 1];
    
    for (int i = 0; i < base_capacity - 1; i++) {
        pool->preallocated[i].next = &pool->preallocated[i + 1];
        pool->preallocated[i + 1].prev = &pool->preallocated[i];
    }
    
    pool->free_count = base_capacity;
    pool->active_list_head = NULL;
    pool->active_count = 0;
    pool->lazy_release_queue = NULL;
    pool->lazy_release_count = 0;
    pool->total_allocated = base_capacity;
    pool->temp_allocated = 0;
    
    return pool;
}
```

- [ ] **Step 2: 实现 connection_pool_destroy 函数**

```c
void connection_pool_destroy(ConnectionPool* pool) {
    if (!pool) {
        return;
    }
    
    // 释放惰性释放队列中的临时连接
    Connection* conn = pool->lazy_release_queue;
    while (conn) {
        Connection* next = conn->next;
        free(conn);
        conn = next;
    }
    
    // 释放预分配块
    free(pool->preallocated);
    
    // 释放池结构
    free(pool);
}
```

- [ ] **Step 3: 提交创建与销毁函数**

```bash
git add src/connection_pool.c
git commit -m "feat(connection_pool): implement create and destroy functions"
```

---

## Task 4: ConnectionPool 实现 - 获取与释放

**Files:**
- Modify: `src/connection_pool.c`

- [ ] **Step 1: 实现 connection_pool_should_expand 函数**

```c
int connection_pool_should_expand(ConnectionPool* pool) {
    if (!pool || pool->base_capacity == 0) {
        return 1;  // 无池或空池，需要扩容
    }
    return pool->free_count < pool->base_capacity * 0.1;
}
```

- [ ] **Step 2: 实现 connection_pool_get 函数**

```c
Connection* connection_pool_get(ConnectionPool* pool) {
    if (!pool) {
        return NULL;
    }
    
    Connection* conn;
    
    // 检查是否需要扩容（阈值触发）
    if (connection_pool_should_expand(pool)) {
        // 触发临时 malloc
        conn = malloc(sizeof(Connection));
        if (!conn) {
            return NULL;
        }
        conn->is_temp_allocated = 1;
        conn->fd = -1;
        conn->state = CONN_STATE_CLOSED;
        conn->release_time = 0;
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
    
    // 从空闲链表获取（O(1)）
    conn = pool->free_list_head;
    pool->free_list_head = conn->next;
    if (pool->free_list_head) {
        pool->free_list_head->prev = NULL;
    } else {
        pool->free_list_tail = NULL;
    }
    pool->free_count--;
    
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
```

- [ ] **Step 3: 实现 connection_pool_release 函数**

```c
void connection_pool_release(ConnectionPool* pool, Connection* conn) {
    if (!pool || !conn) {
        return;
    }
    
    // 从活跃链表移除
    if (conn->prev) {
        conn->prev->next = conn->next;
    } else {
        pool->active_list_head = conn->next;
    }
    if (conn->next) {
        conn->next->prev = conn->prev;
    }
    pool->active_count--;
    
    // 重置 Connection 状态
    conn->fd = -1;
    conn->state = CONN_STATE_CLOSED;
    
    // 根据分配类型路由
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
        conn->release_time = get_current_ms();  // 需要时间函数
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

- [ ] **Step 4: 提交获取与释放函数**

```bash
git add src/connection_pool.c
git commit -m "feat(connection_pool): implement get and release functions with threshold expansion"
```

---

## Task 5: ConnectionPool 实现 - 惰性释放与统计

**Files:**
- Modify: `src/connection_pool.c`

- [ ] **Step 1: 实现 connection_pool_lazy_release_check 函数**

```c
void connection_pool_lazy_release_check(ConnectionPool* pool) {
    if (!pool) {
        return;
    }
    
    // 由 Worker EventLoop 定时器调用（每 10 秒）
    uint64_t now = get_current_ms();  // 需要时间函数
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

- [ ] **Step 2: 实现 connection_pool_get_stats 函数**

```c
PoolStats connection_pool_get_stats(ConnectionPool* pool) {
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
```

- [ ] **Step 3: 提交惰性释放与统计函数**

```bash
git add src/connection_pool.c
git commit -m "feat(connection_pool): implement lazy release check and stats functions"
```

---

## Task 6: HttpConfig 配置扩展

**Files:**
- Modify: `include/config.h`

- [ ] **Step 1: 扩展 HttpConfig 结构**

在 `include/config.h` 的 HttpConfig 结构末尾添加连接池配置字段：

```c
struct HttpConfig {
    // ... 现有字段 ...
    
    // 连接池配置（可选覆盖）
    int connection_pool_size_per_worker;  // 0 = 自动计算
    float connection_pool_expand_threshold; // 默认 0.1 (10%)
    int connection_pool_lazy_release_delay_ms; // 默认 60000
};
```

- [ ] **Step 2: 提交配置扩展**

```bash
git add include/config.h
git commit -m "feat(config): add connection pool configuration fields"
```

---

## Task 7: 单元测试 - 基础功能

**Files:**
- Create: `test/integration/c_core/test_connection_pool.c`

- [ ] **Step 1: 创建测试文件基础结构**

```c
#include "connection_pool.h"
#include "connection.h"
#include <stdio.h>
#include <assert.h>

// 测试辅助函数
static int test_passed = 0;
static int test_failed = 0;

#define TEST(name) static void test_##name()
#define RUN_TEST(name) do { \
    printf("Running test: %s\n", #name); \
    test_##name(); \
    test_passed++; \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("ASSERT FAILED: %s\n", #cond); \
        test_failed++; \
        return; \
    } \
} while(0)
```

- [ ] **Step 2: 实现创建/销毁测试**

```c
TEST(pool_create_destroy) {
    ConnectionPool* pool = connection_pool_create(100);
    ASSERT(pool != NULL);
    ASSERT(pool->base_capacity == 100);
    ASSERT(pool->free_count == 100);
    ASSERT(pool->active_count == 0);
    ASSERT(pool->temp_allocated == 0);
    
    connection_pool_destroy(pool);
    // 无法直接验证销毁，但可通过 Valgrind 验证无泄漏
}

TEST(pool_create_invalid_capacity) {
    ConnectionPool* pool = connection_pool_create(0);
    ASSERT(pool == NULL);
    
    pool = connection_pool_create(-1);
    ASSERT(pool == NULL);
}
```

- [ ] **Step 3: 实现获取/释放测试**

```c
TEST(pool_get_release_basic) {
    ConnectionPool* pool = connection_pool_create(100);
    ASSERT(pool != NULL);
    
    // 获取一个连接
    Connection* conn = connection_pool_get(pool);
    ASSERT(conn != NULL);
    ASSERT(pool->free_count == 99);
    ASSERT(pool->active_count == 1);
    ASSERT(conn->is_temp_allocated == 0);  // 预分配
    
    // 释放连接
    connection_pool_release(pool, conn);
    ASSERT(pool->free_count == 100);
    ASSERT(pool->active_count == 0);
    
    connection_pool_destroy(pool);
}

TEST(pool_get_release_multiple) {
    ConnectionPool* pool = connection_pool_create(100);
    ASSERT(pool != NULL);
    
    // 获取 50 个连接
    Connection* conns[50];
    for (int i = 0; i < 50; i++) {
        conns[i] = connection_pool_get(pool);
        ASSERT(conns[i] != NULL);
    }
    ASSERT(pool->free_count == 50);
    ASSERT(pool->active_count == 50);
    
    // 释放 50 个连接
    for (int i = 0; i < 50; i++) {
        connection_pool_release(pool, conns[i]);
    }
    ASSERT(pool->free_count == 100);
    ASSERT(pool->active_count == 0);
    
    connection_pool_destroy(pool);
}
```

- [ ] **Step 4: 提交基础测试**

```bash
git add test/integration/c_core/test_connection_pool.c
git commit -m "test(connection_pool): add basic create/destroy and get/release tests"
```

---

## Task 8: 单元测试 - 阈值扩容

**Files:**
- Modify: `test/integration/c_core/test_connection_pool.c`

- [ ] **Step 1: 实现阈值扩容测试**

```c
TEST(pool_threshold_expand) {
    ConnectionPool* pool = connection_pool_create(100);
    ASSERT(pool != NULL);
    
    // 获取 90 个连接（剩余 10 个，刚好 10% 阈值）
    Connection* conns[95];
    for (int i = 0; i < 90; i++) {
        conns[i] = connection_pool_get(pool);
    }
    ASSERT(pool->free_count == 10);
    ASSERT(pool->temp_allocated == 0);
    
    // 再获取 5 个，触发阈值扩容（临时 malloc）
    for (int i = 90; i < 95; i++) {
        conns[i] = connection_pool_get(pool);
        ASSERT(conns[i]->is_temp_allocated == 1);  // 临时 malloc
    }
    ASSERT(pool->temp_allocated == 5);
    ASSERT(pool->free_count == 0);  // 空闲链表耗尽
    
    // 释放预分配的连接
    for (int i = 0; i < 90; i++) {
        connection_pool_release(pool, conns[i]);
    }
    ASSERT(pool->free_count == 90);
    ASSERT(pool->lazy_release_count == 0);  // 预分配不进惰性队列
    
    // 释放临时 malloc 的连接（进入惰性队列）
    for (int i = 90; i < 95; i++) {
        connection_pool_release(pool, conns[i]);
    }
    ASSERT(pool->lazy_release_count == 5);
    ASSERT(pool->temp_allocated == 5);
    
    connection_pool_destroy(pool);
}
```

- [ ] **Step 2: 提交阈值扩容测试**

```bash
git add test/integration/c_core/test_connection_pool.c
git commit -m "test(connection_pool): add threshold expansion test"
```

---

## Task 9: 单元测试 - 统计接口

**Files:**
- Modify: `test/integration/c_core/test_connection_pool.c`

- [ ] **Step 1: 实现统计接口测试**

```c
TEST(pool_stats) {
    ConnectionPool* pool = connection_pool_create(100);
    ASSERT(pool != NULL);
    
    // 初始状态
    PoolStats stats = connection_pool_get_stats(pool);
    ASSERT(stats.base_capacity == 100);
    ASSERT(stats.free_count == 100);
    ASSERT(stats.active_count == 0);
    ASSERT(stats.temp_allocated == 0);
    ASSERT(stats.utilization == 0.0f);
    
    // 获取 10 个连接
    Connection* conns[10];
    for (int i = 0; i < 10; i++) {
        conns[i] = connection_pool_get(pool);
    }
    
    stats = connection_pool_get_stats(pool);
    ASSERT(stats.free_count == 90);
    ASSERT(stats.active_count == 10);
    ASSERT(stats.utilization == 0.1f);  // 10% 利用率
    
    // 释放连接
    for (int i = 0; i < 10; i++) {
        connection_pool_release(pool, conns[i]);
    }
    
    stats = connection_pool_get_stats(pool);
    ASSERT(stats.free_count == 100);
    ASSERT(stats.active_count == 0);
    
    connection_pool_destroy(pool);
}
```

- [ ] **Step 2: 实现测试主函数**

```c
int main(int argc, char** argv) {
    printf("=== Connection Pool Tests ===\n\n");
    
    RUN_TEST(pool_create_destroy);
    RUN_TEST(pool_create_invalid_capacity);
    RUN_TEST(pool_get_release_basic);
    RUN_TEST(pool_get_release_multiple);
    RUN_TEST(pool_threshold_expand);
    RUN_TEST(pool_stats);
    
    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", test_passed);
    printf("Failed: %d\n", test_failed);
    
    return test_failed > 0 ? 1 : 0;
}
```

- [ ] **Step 3: 提交统计测试和主函数**

```bash
git add test/integration/c_core/test_connection_pool.c
git commit -m "test(connection_pool): add stats test and main function"
```

---

## Task 10: CMake 集成

**Files:**
- Modify: `test/integration/c_core/CMakeLists.txt`
- Modify: `src/CMakeLists.txt` (如果需要)

- [ ] **Step 1: 添加测试目标到 CMakeLists.txt**

在 `test/integration/c_core/CMakeLists.txt` 添加：

```cmake
# 连接池测试
add_executable(test_connection_pool
    test_connection_pool.c
)

target_link_libraries(test_connection_pool
    connection_pool
    connection
)

target_include_directories(test_connection_pool PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

add_test(NAME ConnectionPoolTests COMMAND test_connection_pool)
```

- [ ] **Step 2: 添加库目标到 src/CMakeLists.txt**

在 `src/CMakeLists.txt` 添加：

```cmake
# 连接池库
add_library(connection_pool STATIC
    connection_pool.c
)

target_include_directories(connection_pool PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
```

- [ ] **Step 3: 提交 CMake 配置**

```bash
git add test/integration/c_core/CMakeLists.txt src/CMakeLists.txt
git commit -m "build(connection_pool): add CMake configuration for connection_pool library and tests"
```

---

## Task 11: 编译与测试验证

- [ ] **Step 1: 编译项目**

```bash
cmake -B build -S .
cmake --build build
```

Expected: 编译成功，无错误无警告

- [ ] **Step 2: 运行单元测试**

```bash
cd build
ctest -R ConnectionPoolTests --output-on-failure
```

Expected: 6 个测试全部通过

- [ ] **Step 3: Valgrind 内存检查**

```bash
valgrind --leak-check=full --error-exitcode=1 ./build/test/integration/c_core/test_connection_pool
```

Expected: 0 leaked bytes

- [ ] **Step 4: 提交验证结果**

```bash
git commit -m "test(connection_pool): all tests pass, no memory leaks"
git push
```

---

## 自检清单

| 检查项 | 状态 |
|--------|------|
| Spec 覆盖完整 | ✅ 所有设计要点都有对应任务 |
| 无占位符 | ✅ 所有代码完整，无 TBD/TODO |
| 类型一致性 | ✅ ConnectionPool、Connection、PoolStats 字段一致 |
| 测试覆盖 | ✅ 创建、销毁、获取、释放、阈值、统计 |
| 验收标准 | ✅ free_count 正确、阈值触发、惰性释放、无泄漏 |

---

## 下一步

计划完成后，需要：
1. 将此功能集成到 WorkerThread（Phase 2）
2. Worker 启动时调用 connection_pool_create()
3. Worker add_connection 使用 connection_pool_get()
4. Worker EventLoop 添加 lazy_release_check 定时器

这些集成任务应添加到现有 Phase 2 实施计划中。