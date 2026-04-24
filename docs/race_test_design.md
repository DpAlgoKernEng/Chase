# 多进程并发竞态测试设计

## 1. 概述

本文档设计多进程并发竞态测试用例，用于验证 Chase 核心模块在高并发场景下的正确性。

### 测试目标

- **无死锁**: 多 Worker 同时操作不应产生死锁
- **无数据竞争**: 共享数据访问应有正确的同步机制
- **无竞态条件**: 逻辑操作结果应与预期一致

### 测试工具

- **ThreadSanitizer (TSan)**: Clang/GCC 内置数据竞争检测器
- **Valgrind helgrind**: 替代方案（适用于更多平台）
- **自定义并发测试**: 多线程压力测试

---

## 2. ConnectionPool 并发测试

### 2.1 模块分析

**竞态风险点**:

| 数据结构 | 操作 | 风险等级 | 说明 |
|---------|------|---------|------|
| `free_list_head/tail` | get/release | **高** | 无锁保护，链表指针竞态 |
| `active_list_head` | get/release | **高** | 无锁保护，链表指针竞态 |
| `lazy_release_queue` | release/check | **高** | 无锁保护，队列竞态 |
| `free_count/active_count` | get/release | **中** | 计数器竞态，可能导致不一致 |
| `temp_allocated` | 扩容/释放 | **中** | 动态分配计数竞态 |
| `PoolEntry.next/prev` | 链表操作 | **高** | 指针修改竞态 |

**当前状态**: ConnectionPool **未实现锁保护**，设计上用于单 Worker 内部管理，不存在跨 Worker 共享。

### 2.2 测试用例

#### TC-POOL-01: 多 Worker 同时获取连接

```c
// 测试场景：多个线程同时调用 connection_pool_get
// 验证目标：无数据竞争，无链表损坏
// 预期结果：TSan 检测到竞争（因为无锁）
// 实际目标：验证设计假设（单 Worker 使用）

void test_pool_concurrent_get(void) {
    ConnectionPool *pool = connection_pool_create(100);
    
    // 启动 4 个 Worker 线程
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, worker_get_loop, pool);
    }
    
    // 等待完成
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    
    // 验证池状态一致性
    PoolStats stats = connection_pool_get_stats(pool);
    assert(stats.free_count + stats.active_count == stats.base_capacity + stats.temp_allocated);
}
```

#### TC-POOL-02: 连接懒释放竞态

```c
// 测试场景：多线程同时释放临时连接到惰性队列
// 验证目标：惰性队列操作的正确性
// 预期结果：TSan 检测到竞争

void test_pool_lazy_release_race(void) {
    ConnectionPool *pool = connection_pool_create(10);  // 小容量触发扩容
    
    // 先耗尽预分配连接
    Connection *conns[15];
    for (int i = 0; i < 10; i++) {
        conns[i] = connection_pool_get(pool);
    }
    
    // 获取临时分配连接
    for (int i = 10; i < 15; i++) {
        conns[i] = connection_pool_get(pool);
    }
    
    // 多线程同时释放临时连接
    pthread_t threads[5];
    for (int i = 0; i < 5; i++) {
        pthread_create(&threads[i], NULL, release_temp_conn, &conns[10 + i]);
    }
    
    // 等待完成
    for (int i = 0; i < 5; i++) {
        pthread_join(threads[i], NULL);
    }
}
```

#### TC-POOL-03: 连接超时清理竞态

```c
// 测试场景：惰性释放检查与其他操作并发
// 验证目标：清理线程与 Worker 线程的竞态

void test_pool_cleanup_race(void) {
    ConnectionPool *pool = connection_pool_create(100);
    
    // 创建清理线程（模拟定时器）
    pthread_t cleanup_thread;
    pthread_create(&cleanup_thread, NULL, cleanup_loop, pool);
    
    // Worker 线程同时获取/释放
    pthread_t worker_threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&worker_threads[i], NULL, worker_get_release_loop, pool);
    }
    
    // 运行一段时间
    sleep(5);
    
    // 停止线程
    // ... 停止逻辑
}
```

### 2.3 设计建议

**当前设计是正确的**: ConnectionPool 设计为 Worker 内部私有，不应跨 Worker 共享。

如果未来需要共享池:
```c
// 建议方案：添加互斥锁
typedef struct ConnectionPool {
    pthread_mutex_t lock;  // 新增锁
    PoolEntry **preallocated;
    // ...
} ConnectionPool;

// 获取连接（加锁版）
Connection *connection_pool_get_safe(ConnectionPool *pool) {
    pthread_mutex_lock(&pool->lock);
    Connection *conn = connection_pool_get_internal(pool);
    pthread_mutex_unlock(&pool->lock);
    return conn;
}
```

---

## 3. Security 分片哈希表竞态测试

### 3.1 模块分析

**竞态风险点**:

| 数据结构 | 操作 | 风险等级 | 说明 |
|---------|------|---------|------|
| 分片锁 `pthread_mutex_t` | 所有操作 | **低** | 已有锁保护 |
| `IpEntry.connection_count` | add/remove | **低** | 锁内操作，安全 |
| `IpEntry.is_blocked` | block/check | **低** | 锁内操作，安全 |
| `IpEntry.request_count` | rate_check | **低** | 锁内操作，安全 |
| `security->current_time_ms` | 多分片操作 | **中** | 无锁更新，但仅读取 |
| 分片遍历 `security_cleanup` | 清理操作 | **中** | 多分片锁顺序问题 |

**当前状态**: Security **已实现分片锁**，设计合理，但仍需验证边界情况。

### 3.2 测试用例

#### TC-SEC-01: 同一 IP 多 Worker 检测

```c
// 测试场景：多线程同时检测同一 IP 的连接限制
// 验证目标：分片锁的正确性
// 预期结果：无数据竞争（TSan 不报警）

void test_security_same_ip_concurrent(void) {
    SecurityConfig config = {
        .max_connections_per_ip = 10,
        .shard_count = 16
    };
    Security *security = security_create(&config);
    
    IpAddress ip;
    security_string_to_ip("192.168.1.1", &ip);
    
    // 10 个线程同时尝试连接同一 IP
    pthread_t threads[10];
    ThreadArg args[10];
    
    for (int i = 0; i < 10; i++) {
        args[i].security = security;
        args[i].ip = &ip;
        pthread_create(&threads[i], NULL, try_connect, &args[i]);
    }
    
    // 等待完成
    for (int i = 0; i < 10; i++) {
        pthread_join(threads[i], NULL);
    }
    
    // 验证计数正确
    IpStats stats;
    security_get_ip_stats(security, &ip, &stats);
    // 应等于成功连接的线程数（<=10）
    assert(stats.connection_count <= 10);
}
```

#### TC-SEC-02: IP 封禁竞态

```c
// 测试场景：一个线程封禁，另一个线程检测封禁状态
// 验证目标：封禁状态更新的原子性
// 预期结果：无竞态条件

void test_security_block_race(void) {
    Security *security = security_create_default();
    IpAddress ip;
    security_string_to_ip("10.0.0.1", &ip);
    
    // 封禁线程
    pthread_t block_thread;
    pthread_create(&block_thread, NULL, block_loop, security);
    
    // 检测线程
    pthread_t check_threads[5];
    for (int i = 0; i < 5; i++) {
        pthread_create(&check_threads[i], NULL, check_blocked_loop, security);
    }
    
    // 运行一段时间
    sleep(3);
    
    // 停止并验证
}
```

#### TC-SEC-03: 速率计数器竞态

```c
// 测试场景：多线程同时更新同一 IP 的请求计数
// 验证目标：速率计数器的正确性
// 预期结果：无竞态，计数准确

void test_security_rate_counter_race(void) {
    SecurityConfig config = {
        .max_requests_per_second = 1000,  // 高限制便于测试
        .shard_count = 16
    };
    Security *security = security_create(&config);
    
    IpAddress ip;
    security_string_to_ip("172.16.0.1", &ip);
    
    // 100 个线程同时发送请求
    pthread_t threads[100];
    for (int i = 0; i < 100; i++) {
        pthread_create(&threads[i], NULL, send_request, security);
    }
    
    // 等待完成
    for (int i = 0; i < 100; i++) {
        pthread_join(threads[i], NULL);
    }
    
    // 验证计数
    IpStats stats;
    security_get_ip_stats(security, &ip, &stats);
    assert(stats.request_count == 100);  // 应精确等于线程数
}
```

#### TC-SEC-04: 分片清理竞态

```c
// 测试场景：清理线程遍历分片，Worker 线程同时操作
// 验证目标：多分片锁顺序，无死锁
// 预期结果：无死锁，无竞态

void test_security_cleanup_concurrent(void) {
    Security *security = security_create_default();
    
    // 创建大量 IP 条目
    for (int i = 0; i < 100; i++) {
        char ip_str[32];
        snprintf(ip_str, sizeof(ip_str), "192.168.%d.%d", i / 10, i % 10);
        IpAddress ip;
        security_string_to_ip(ip_str, &ip);
        security_add_connection(security, &ip);
    }
    
    // 清理线程
    pthread_t cleanup_thread;
    pthread_create(&cleanup_thread, NULL, cleanup_loop, security);
    
    // Worker 线程（操作不同 IP，可能在不同分片）
    pthread_t worker_threads[10];
    for (int i = 0; i < 10; i++) {
        pthread_create(&worker_threads[i], NULL, random_ip_operation, security);
    }
    
    // 运行一段时间
    sleep(5);
    
    // 验证无死锁（如果能在超时前完成）
}
```

---

## 4. Timer 堆并发测试

### 4.1 模块分析

**竞态风险点**:

| 数据结构 | 操作 | 风险等级 | 说明 |
|---------|------|---------|------|
| `timers[]` 数组 | add/remove | **高** | 无锁保护，堆操作竞态 |
| `size` 计数 | add/pop | **高** | 无锁保护 |
| `next_id++` | add | **高** | ID 生成竞态 |
| `Timer.heap_index` | remove/sift | **高** | 索引更新竞态 |
| 堆调整 `sift_up/down` | add/remove | **高** | 无锁保护 |

**当前状态**: TimerHeap **未实现锁保护**，设计用于单 EventLoop 管理。

### 4.2 测试用例

#### TC-TIMER-01: 多 Worker 添加定时器

```c
// 测试场景：多线程同时添加定时器
// 验证目标：无数据竞争（预期 TSan 报警）
// 设计验证：确认 TimerHeap 为单线程设计

void test_timer_concurrent_add(void) {
    TimerHeap *heap = timer_heap_create(256);
    
    // 10 个线程同时添加定时器
    pthread_t threads[10];
    for (int i = 0; i < 10; i++) {
        pthread_create(&threads[i], NULL, add_timer_loop, heap);
    }
    
    // 等待完成
    for (int i = 0; i < 10; i++) {
        pthread_join(threads[i], NULL);
    }
    
    // 验证堆状态（可能已损坏）
    // TSan 应检测到竞争
}
```

#### TC-TIMER-02: 定时器删除竞态

```c
// 测试场景：一个线程添加，另一个线程删除
// 验证目标：remove 操作的安全性
// 预期结果：TSan 检测到竞争

void test_timer_remove_race(void) {
    TimerHeap *heap = timer_heap_create(256);
    
    volatile bool running = true;
    
    // 添加线程
    pthread_t add_thread;
    pthread_create(&add_thread, NULL, add_timer_infinite, heap);
    
    // 删除线程
    pthread_t remove_thread;
    pthread_create(&remove_thread, NULL, remove_timer_loop, heap);
    
    // 运行一段时间
    sleep(3);
    running = false;
    
    pthread_join(add_thread, NULL);
    pthread_join(remove_thread, NULL);
}
```

#### TC-TIMER-03: 定时器触发竞态

```c
// 测试场景：多线程同时 pop 触发定时器
// 验证目标：pop 操作的安全性
// 预期结果：TSan 检测到竞争

void test_timer_pop_race(void) {
    TimerHeap *heap = timer_heap_create(256);
    
    // 预添加定时器
    for (int i = 0; i < 100; i++) {
        timer_heap_add(heap, i * 10, dummy_callback, NULL, false);
    }
    
    // 多线程同时 pop
    pthread_t threads[5];
    for (int i = 0; i < 5; i++) {
        pthread_create(&threads[i], NULL, pop_timer_loop, heap);
    }
    
    // 等待完成
    for (int i = 0; i < 5; i++) {
        pthread_join(threads[i], NULL);
    }
}
```

---

## 5. ThreadSanitizer 配置

### 5.1 CMake TSan 配置

```cmake
# cmake/ThreadSanitizer.cmake
option(ENABLE_TSAN "Enable ThreadSanitizer for race detection" OFF)

if(ENABLE_TSAN AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    # TSan 编译选项
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
    
    # TSan 限制选项
    set(TSAN_OPTIONS "halt_on_error=1:history_size=7:die_on_race=1")
    
    message(STATUS "ThreadSanitizer enabled")
    message(STATUS "TSan options: ${TSAN_OPTIONS}")
endif()
```

### 5.2 运行命令

```bash
# 构建 TSan 版本
cmake -B build-tsan -DENABLE_TSAN=ON -DENABLE_COVERAGE=OFF
cmake --build build-tsan

# 运行竞态测试
cd build-tsan
export TSAN_OPTIONS="halt_on_error=1:history_size=7:die_on_race=1:verbosity=1"

# 运行 ConnectionPool 竞态测试
./test_race_connection_pool

# 运行 Security 竞态测试
./test_race_security

# 运行 Timer 竞态测试
./test_race_timer

# 运行所有竞态测试
ctest -L race -V
```

### 5.3 报告解读

**TSan 报告示例**:
```
==================
WARNING: ThreadSanitizer: data race (pid=12345)
  Read of size 4 at 0x7b0000000000 by thread T1:
    #0 connection_pool_get src/connection_pool.c:192

  Previous write of size 4 at 0x7b0000000000 by thread T2:
    #0 connection_pool_release src/connection_pool.c:256

  Location is global 'pool.free_count' at 0x7b0000000000
==================
```

**解读要点**:
1. `Read/Write` 类型 - 确定操作类型
2. `Location` - 定位竞态变量
3. `Thread T1/T2` - 确定竞争线程
4. `#0 function:file:line` - 定位代码位置

**处理方式**:
- 如果是设计允许（单 Worker 使用）：记录为已知设计约束
- 如果需要修复：添加互斥锁或原子操作
- 如果是误报：使用 `TSAN_IGNORE` 标记

---

## 6. 测试代码实现

### 6.1 文件结构

```
test/integration/c_core/
    test_race_connection_pool.c  # ConnectionPool 竞态测试
    test_race_security.c         # Security 竞态测试
    test_race_timer.c            # Timer 竞态测试
```

### 6.2 测试框架

```c
// 竞态测试通用框架
#define RACE_TEST_ITERATIONS 1000
#define RACE_THREAD_COUNT 4
#define RACE_DURATION_SEC 5

// 测试结果结构
typedef struct {
    int iterations_completed;
    int race_detected;      // TSan 检测到的竞争数
    int consistency_errors; // 数据不一致错误
    int deadlock_detected;  // 死锁检测
} RaceTestResult;

// 线程参数结构
typedef struct {
    void *target;           // 测试目标对象
    int thread_id;
    volatile bool *running;
    RaceTestResult *result;
} ThreadArg;
```

---

## 7. 预期测试结果

### 7.1 ConnectionPool

| 测试 | 预期 TSan 结果 | 处理方式 |
|-----|---------------|---------|
| TC-POOL-01 | **报警**（数据竞争） | 已知设计：单 Worker 使用 |
| TC-POOL-02 | **报警**（数据竞争） | 已知设计 |
| TC-POOL-03 | **报警**（数据竞争） | 已知设计 |

### 7.2 Security

| 测试 | 预期 TSan 结果 | 处理方式 |
|-----|---------------|---------|
| TC-SEC-01 | **无报警** | 已有锁保护 |
| TC-SEC-02 | **无报警** | 已有锁保护 |
| TC-SEC-03 | **无报警** | 已有锁保护 |
| TC-SEC-04 | **无报警** | 已有锁保护，验证无死锁 |

### 7.3 Timer

| 测试 | 预期 TSan 结果 | 处理方式 |
|-----|---------------|---------|
| TC-TIMER-01 | **报警**（数据竞争） | 已知设计：单 EventLoop 使用 |
| TC-TIMER-02 | **报警**（数据竞争） | 已知设计 |
| TC-TIMER-03 | **报警**（数据竞争） | 已知设计 |

---

## 8. 结论与建议

### 8.1 设计验证

ConnectionPool 和 Timer 的竞态是**设计预期**：
- ConnectionPool: Worker 内部私有，不跨进程共享
- Timer: EventLoop 内部私有，不跨进程共享

Security 已正确实现分片锁，竞态测试应通过。

### 8.2 后续工作

1. 添加 TSan 自动化测试到 CI
2. 为 ConnectionPool/Timer 添加文档说明单线程使用约束
3. 考虑为需要共享的场景提供加锁版本 API

### 8.3 测试覆盖率

- ConnectionPool: 3 个竞态测试用例
- Security: 4 个竞态测试用例
- Timer: 3 个竞态测试用例
- 总计: 10 个竞态测试用例