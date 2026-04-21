# 模块架构重构设计文档

**日期**: 2026-04-21
**作者**: minghui.liu
**状态**: 已批准

---

## 一、背景

当前 Chase HTTP Server 项目存在以下架构问题：

1. **Connection 模块被池管理污染** - 连接池管理字段暴露在 Connection 公共接口中
2. **Worker 职责过重** - 承担了 EventLoop、Router、Socket、Accept 处理等多种职责
3. **production_server.c 重复逻辑** - 连接处理、响应构建等逻辑在 example 中重复实现
4. **Connection 依赖 EventLoop** - 直接依赖导致模块耦合
5. **缺少 Handler/Response 抽象** - 用户需要手写 snprintf 构建响应

---

## 二、目标架构

```
Application Layer
    │
    ▼
Process Layer (master, worker)
    │
    ▼
Server Layer (server)
    │
    ▼
Handler Layer (handler, response)
    │
    ▼
Core Layer (eventloop, timer, buffer, socket,
            connection, connection_pool, router,
            http_parser, mime, error)
```

---

## 三、新增模块

### 3.1 Server 模块

**文件**: `include/server.h`, `src/server.c`
**职责**: HTTP 服务器封装层，管理完整服务器生命周期
**Layer**: Server Layer

**接口设计**:

```c
typedef struct ServerConfig {
    int port;
    int max_connections;
    int backlog;
    const char *bind_addr;
    bool reuseport;
    Router *router;
    size_t read_buf_cap;
    size_t write_buf_cap;
} ServerConfig;

typedef struct Server Server;

Server *server_create(const ServerConfig *config);
void server_destroy(Server *server);
int server_run(Server *server);
void server_stop(Server *server);
int server_get_fd(Server *server);
```

**内部实现**:
- 创建 EventLoop
- 创建监听 Socket（支持 SO_REUSEPORT）
- 处理 Accept → 创建 Connection → 注册读事件
- 读事件触发 → HTTP 解析 → Router 匹配 → Handler 调用 → Response 发送

---

### 3.2 Response 模块

**文件**: `include/response.h`, `src/response.c`
**职责**: HTTP 响应构建器，标准化响应格式
**Layer**: Handler Layer

**接口设计**:

```c
typedef struct HttpResponse HttpResponse;

HttpResponse *response_create(int status);
void response_destroy(HttpResponse *resp);

void response_set_header(HttpResponse *resp, const char *key, const char *value);
void response_set_body(HttpResponse *resp, const char *body, size_t len);
void response_set_body_json(HttpResponse *resp, const char *json);

int response_build(HttpResponse *resp, char *buf, size_t buf_size);
int response_send(HttpResponse *resp, int fd);
```

---

### 3.3 Handler 模块

**文件**: `include/handler.h`, `src/handler.c`
**职责**: 预置 Handler 函数，简化请求处理
**Layer**: Handler Layer

**接口设计**:

```c
typedef void (*RequestHandler)(HttpRequest *req, HttpResponse *resp, void *user_data);

RequestHandler handler_static_file(FileServe *fs);
RequestHandler handler_json_api(const char *json_response);
RequestHandler handler_404(void);
```

---

## 四、现有模块修改

### 4.1 Connection 模块

**修改内容**:
- 移除池管理 API（共 8 个函数）
- 移除 `#include "eventloop.h"` 依赖
- 用 `ConnectionCloseCallback` 回调替代 `EventLoop *loop` 字段

**移除的 API**:
```c
connection_get_next / connection_set_next
connection_get_prev / connection_set_prev
connection_get_release_time / connection_set_release_time
connection_is_temp_allocated / connection_set_temp_allocated
```

**新增回调类型**:
```c
typedef void (*ConnectionCloseCallback)(int fd, void *user_data);
```

**修改后的核心接口**:
```c
Connection *connection_create(int fd,
                              ConnectionCloseCallback on_close,
                              void *close_user_data);
void connection_init_from_pool(Connection *conn, int fd,
                               ConnectionCloseCallback on_close,
                               void *close_user_data);
```

---

### 4.2 Connection Pool 模块

**修改内容**:
- 内部新增 `PoolEntry` 结构体管理池字段
- Connection 接口保持不变

**内部结构**:
```c
typedef struct PoolEntry {
    Connection *conn;
    PoolEntry *next;
    PoolEntry *prev;
    uint64_t release_time;
    int is_temp_allocated;
} PoolEntry;
```

---

### 4.3 Worker 模块

**修改内容**:
- 从"全功能编排"变为"进程生命周期管理"
- Server 创建逻辑移到 Server 模块

**修改后的接口**:
```c
typedef struct WorkerConfig {
    int worker_id;
    Server *server;
    volatile sig_atomic_t running;
} WorkerConfig;

Worker *worker_create(const WorkerConfig *config);
void worker_destroy(Worker *worker);
int worker_run(Worker *worker);
void worker_stop(Worker *worker);
```

**保留职责**: 信号处理（SIGINT、SIGTERM、SIGPIPE）

---

### 4.4 production_server.c

**修改内容**:
- 使用 Server 模块
- 消除重复的连接处理、响应构建逻辑

---

## 五、文件头注释规范

每个代码文件开头添加标准职责注释：

```c
/**
 * @file    module_name.c
 * @brief   模块职责简短描述
 *
 * @details 详细说明
 *          - 核心功能 1
 *          - 核心功能 2
 *
 * @layer   Layer名称 (Process/Server/Handler/Core)
 *
 * @depends 依赖模块列表
 * @usedby  被哪些模块使用
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */
```

---

## 六、文件清单

### 新增文件（6 个）

| 文件 | 职责 |
|------|------|
| `include/server.h` | Server 模块头文件 |
| `src/server.c` | Server 模块实现 |
| `include/response.h` | Response 模块头文件 |
| `src/response.c` | Response 模块实现 |
| `include/handler.h` | Handler 模块头文件 |
| `src/handler.c` | Handler 模块实现 |

### 修改文件（7 个）

| 文件 | 修改内容 |
|------|----------|
| `include/connection.h` | 移除池管理 API |
| `src/connection.c` | 移除池管理字段，改用回调 |
| `src/connection_pool.c` | 内部新增 PoolEntry |
| `include/worker.h` | 接口改为接收 Server |
| `src/worker.c` | 移除 Server 创建逻辑 |
| `examples/production_server.c` | 改用 Server 模块 |

### 所有文件添加头注释

现有所有 `.c` 和 `.h` 文件添加标准职责注释。

---

## 七、依赖关系

```
                        master.c
                          │
                          ▼
                        worker.c
                          │
                          ▼
                        server.c (新增)
                          │
          ┌───────────────┼───────────────┐
          │               │               │
          ▼               ▼               ▼
      eventloop.c     router.c      handler.c (新增)
          │               │               │
          │               │               ▼
          │               │         response.c (新增)
          │               │               │
          ▼               ▼               ▼
        timer.c       connection.c    fileserve.c
          │               │               │
          ▼               ▼               ▼
        buffer.c    connection_pool.c  mime.c


    socket.c  ← 零依赖，被 server, worker, connection 使用
    error.c   ← 零依赖，被 http_parser, fileserve, handler 使用
    http_parser.c ← 零依赖，被 server, handler 使用
```