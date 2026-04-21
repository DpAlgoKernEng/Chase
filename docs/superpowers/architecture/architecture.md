# Chase HTTP Server - 生产级架构设计

## 概述

Chase 是一个高性能、生产级的 HTTP 服务器，采用 **SO_REUSEPORT + 多进程架构**，实现零 IPC 开销的连接分发。

## 架构总览

```
Master 进程（管理角色）
│
│ 职责：
│   - 启动 Worker 进程
│   - 监控 Worker 健康状态
│   - 处理信号（SIGINT/SIGTERM/SIGHUP）
│   - 重启崩溃的 Worker
│   - 平滑关闭
│   - 不监听端口、不处理连接
│
├────────────────────────────┬────────────────────────────┬────────────────────────────
│                            │                            │
Worker 进程 1               Worker 进程 2               Worker 进程 N
│                            │                            │
│ socket() + SO_REUSEPORT    │ socket() + SO_REUSEPORT    │ socket() + SO_REUSEPORT
│ bind(:8080)                │ bind(:8080)                │ bind(:8080)
│ listen()                   │ listen()                   │ listen()
│                            │                            │                            │
│ EventLoop                  │ EventLoop                  │ EventLoop
│   ├── epoll/kqueue         │   ├── epoll/kqueue         │   ├── epoll/kqueue
│   ├── server_fd (accept)   │   ├── server_fd (accept)   │   ├── server_fd (accept)
│   └── client_fds (I/O)     │   └── client_fds (I/O)     │   └── client_fds (I/O)
│                            │                            │                            │
│ accept() → 处理请求        │ accept() → 处理请求        │ accept() → 处理请求
│                            │                            │                            │
└────────────────────────────┴────────────────────────────┴────────────────────────────┘
                    完全独立，零 IPC 开销，内核负载均衡
```

## 核心设计原则

### 1. SO_REUSEPORT 零开销分发

传统架构需要 Master 进程 accept 后通过 IPC 分发给 Worker：

```
传统架构（高开销）：
Master: accept() → 选择 Worker → sendmsg(SCM_RIGHTS) ~100μs → Worker
                                                      ↑
                                              IPC 是性能瓶颈
```

SO_REUSEPORT 架构让每个 Worker 直接 accept，内核自动负载均衡：

```
SO_REUSEPORT 架构（零开销）：
Worker 1: accept() ← 内核分发
Worker 2: accept() ← 内核分发
Worker N: accept() ← 内核分发
          ↑
   无 IPC，内核自动均衡
```

### 2. 进程隔离与容错

| 特性 | 多进程架构 | 多线程架构 |
|------|-----------|-----------|
| 崩溃影响 | 单 Worker 崩溃，其他继续 | 线程崩溃可能影响整个进程 |
| 内存隔离 | 完全隔离 | 共享，需锁保护 |
| 调试 | 简单，独立进程 | 复杂，多线程竞争 |
| 重启 | 单个 Worker 可重启 | 整个进程重启 |

### 3. Master 职责最小化

Master **不处理任何连接**，仅作为管理角色：

| Master 职责 | Worker 职责 |
|-------------|-------------|
| 启动 Worker 进程 | 监听端口 |
| 监控 Worker 状态 | Accept 连接 |
| 重启崩溃 Worker | 处理 HTTP 请求 |
| 处理信号 | 连接 I/O |
| 平滑关闭 | 业务逻辑 |

## 进程通信设计

### 最小化 IPC

Worker 完全独立运行，IPC 仅用于管理目的：

```
Master                          Worker 进程
    │                                │
    │ 启动时传递配置（环境变量/参数）   │
    │ ─────────────────────────────>│
    │                                │
    │ 信号通知（SIGTERM）             │
    │ ─────────────────────────────>│ 停止运行
    │                                │
    │ waitpid() 监控                 │
    │ <───────────────────────────── │ 退出状态
    │                                │
    │ 崩溃检测 → fork 重启            │
    │ ─────────────────────────────>│ 新 Worker 启动
```

**不涉及连接处理的 IPC**，避免性能瓶颈。

### 信号处理矩阵

| 信号 | 接收者 | 行为 |
|------|--------|------|
| SIGINT | Master | 平滑关闭所有 Worker |
| SIGTERM | Master | 平滑关闭所有 Worker |
| SIGTERM | Worker | 停止 accept，处理完连接后退出 |
| SIGHUP | Master | 重启 Worker（reload 配置） |
| SIGCHLD | Master | 检测 Worker 状态变化 |

## Worker 内部架构

### EventLoop 事件驱动

```
Worker 进程
│
│ EventLoop
│   │
│   │ epoll_fd = epoll_create1(EPOLL_CLOEXEC)
│   │
│   │ 监听事件：
│   │   ├── server_fd (EPOLLIN) → accept 新连接
│   │   └── client_fds (EPOLLIN/EPOLLOUT) → 处理连接 I/O
│   │
│   │ 事件循环：
│   │   while (running) {
│   │       n = epoll_wait(events, MAX_EVENTS, timeout);
│   │       for (i = 0; i < n; i++) {
│   │           dispatch_event(events[i]);
│   │       }
│   │   }
│   │
│   └──────────────────────────────────────────────────────
│
│ 连接处理流程：
│   │
│   │ on_accept():
│   │     client_fd = accept(server_fd)
│   │     set_nonblock(client_fd)
│   │     epoll_add(client_fd, EPOLLIN, on_read)
│   │
│   │ on_read():
│   │     read(client_fd, buffer)
│   │     http_parse(buffer) → HttpRequest
│   │     route_match(request) → Route
│   │     handler(request) → HttpResponse
│   │     epoll_modify(client_fd, EPOLLOUT)
│   │
│   │ on_write():
│   │     write(client_fd, response)
│   │     if done: close(client_fd) or keep-alive
│   │
│   └──────────────────────────────────────────────────────
```

### 连接状态管理

```
连接生命周期：
    │
    │ accept() → 新连接
    │     │
    │     ↓
    │ READING: 等待 HTTP 请求
    │     │
    │     ↓ on_read()
    │ PROCESSING: 解析 + 路由匹配 + 生成响应
    │     │
    │     ↓
    │ WRITING: 发送响应
    │     │
    │     ↓ on_write() 完成
    │     │
    │     ├──────────────┬──────────────┐
    │     ↓              ↓              ↓
    │ KEEP_ALIVE     CLOSE         ERROR
    │ (等待新请求)    (关闭连接)    (关闭连接)
    │     │              │              │
    │     ↓              ↓              ↓
    │ READING         释放资源       释放资源
```

## 平台兼容性

### Linux vs macOS

| 特性 | Linux | macOS |
|------|-------|-------|
| SO_REUSEPORT | ✅ 支持（内核 3.9+） | ✅ 支持 |
| epoll | ✅ 原生支持 | ❌ 使用 kqueue |
| eventfd | ✅ 原生支持 | ❌ 使用 pipe |
| 内核负载均衡 | Round-Robin | Round-Robin |

### EventLoop 后端抽象

```c
// Linux: epoll
struct epoll_event ev;
epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
n = epoll_wait(epoll_fd, events, max_events, timeout);

// macOS: kqueue
struct kevent ev;
EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
kevent(kqueue_fd, &ev, 1, NULL, 0, NULL);
n = kevent(kqueue_fd, NULL, 0, events, max_events, &timeout);
```

现有 `eventloop.c` 已实现跨平台抽象，Worker 可直接使用。

## 性能预期

### 基准测试目标

| 平台 | 配置 | 预期性能 |
|------|------|----------|
| macOS | 4 Workers, Keep-Alive | 30k-50k req/s |
| Linux | 4 Workers, Keep-Alive | 50k-100k req/s |
| Linux | 8 Workers, Keep-Alive | 100k+ req/s |

### 性能对比

| 架构 | macOS 性能 | 说明 |
|------|------------|------|
| 单线程（baseline） | ~27k req/s | 参考 benchmark |
| 多线程 + SCM_RIGHTS | ~3k req/s | IPC 开销过高 |
| **SO_REUSEPORT 多进程** | **30k-50k req/s** | 预期 |

### 性能优化路径

| 优化项 | 预期提升 | 复杂度 |
|--------|----------|--------|
| HTTP Keep-Alive | 2-3x | 中 |
| 响应预构造 | 1.2x | 低 |
| writev 批量写 | 1.2x | 低 |
| 内存池 | 1.3x | 中 |
| 连接池 | 1.5x | 中 |

## 配置设计

### 服务器配置

```c
typedef struct {
    int port;                    // 监听端口（默认 8080）
    int worker_count;            // Worker 数量（默认 4）
    int max_events;              // 每个 Worker 最大事件数（默认 1024）
    int backlog;                 // listen backlog（默认 SOMAXCONN）
    int keep_alive_timeout;      // Keep-Alive 超时（秒，默认 5）
    bool reuseport;              // 启用 SO_REUSEPORT（默认 true）
} ServerConfig;
```

### Worker 配置

```c
typedef struct {
    int worker_id;               // Worker ID（0-N）
    ServerConfig *server;        // 服务器配置引用
    int notify_fd;               // Master 通知 fd（可选）
} WorkerConfig;
```

### 命令行参数

```bash
# 启动服务器
./chase_server --port 8080 --workers 4

# 指定配置文件
./chase_server --config /etc/chase/server.conf

# 前台运行（不 daemonize）
./chase_server --port 8080 --foreground

# 显示帮助
./chase_server --help
```

## 进程生命周期

### 启动流程

```
启动阶段：
    │
    │ main()
    │     │
    │     ↓
    │ 解析配置（命令行/配置文件）
    │     │
    │     ↓
    │ 创建 Master 进程
    │     │
    │     ↓
    │ Master 初始化
    │     │
    │     │ 注册信号处理器
    │     │ 创建必要资源
    │     │
    │     ↓
    │ 启动 Workers
    │     │
    │     │ for (i = 0; i < worker_count; i++) {
    │     │     pid = fork();
    │     │     if (pid == 0) {
    │     │         worker_run(config);
    │     │         exit(0);
    │     │     }
    │     │     workers[i] = pid;
    │     │ }
    │     │
    │     ↓
    │ Master 监控循环
    │     │
    │     │ while (running) {
    │     │     pid = waitpid(-1, &status, 0);
    │     │     if (pid > 0) {
    │     │         // 检测崩溃，重启
    │     │         restart_worker(pid);
    │     │     }
    │     │ }
```

### 运行阶段

```
运行阶段：
    │
    │ Master                        Workers
    │     │                            │
    │     │ waitpid() 等待             │ eventloop_run()
    │     │                            │
    │     │                            │ epoll_wait()
    │     │                            │     │
    │     │                            │     ├─ accept() → 新连接
    │     │                            │     └─ read/write → I/O
    │     │                            │
    │     │ 检测 Worker 状态           │
    │     │                            │
    │     │ Worker 崩溃？              │
    │     │     │                      │
    │     │     ├─ Yes → fork 重启     │ 新 Worker
    │     │     │                      │
    │     │     └─ No → 继续等待        │
```

### 关闭流程

```
关闭阶段（Ctrl+C 或 SIGTERM）：
    │
    │ Master 收到 SIGTERM
    │     │
    │     ↓
    │ 设置 running = false
    │     │
    │     ↓
    │ 通知所有 Worker 停止
    │     │
    │     │ for (i = 0; i < worker_count; i++) {
    │     │     kill(workers[i], SIGTERM);
    │     │ }
    │     │
    │     ↓
    │ 等待 Worker 退出
    │     │
    │     │ Worker 收到 SIGTERM：
    │     │     │
    │     │     ↓
    │     │ 停止 accept（从 epoll 移除 server_fd）
    │     │     │
    │     │     ↓
    │     │ 处理现有连接（直到全部完成或超时）
    │     │     │
    │     │     ↓
    │     │ 关闭所有连接
    │     │     │
    │     │     ↓
    │     │ exit(0)
    │     │
    │     ↓
    │ Master waitpid() 等待所有 Worker
    │     │
    │     ↓
    │ 所有 Worker 退出
    │     │
    │     ↓
    │ Master 清理资源
    │     │
    │     ↓
    │ exit(0)
```

## 错误处理与容错

### Worker 崩溃恢复

```
Worker 崩溃检测：
    │
    │ Master: waitpid() 返回 Worker PID
    │     │
    │     ↓
    │ 检查退出状态
    │     │
    │     ├─ 正常退出（exit(0)） → 记录日志
    │     │
    │     ├─ 信号终止（SIGKILL/SIGSEGV） → 记录错误，重启
    │     │
    │     ↓
    │ 重启 Worker
    │     │
    │     │ pid = fork();
    │     │ if (pid == 0) {
    │     │     worker_run(config);
    │     │     exit(0);
    │     │ }
    │     │ workers[slot] = pid;
    │     │
    │     ↓
    │ 继续监控
```

### 资源限制处理

| 场景 | 处理方式 |
|------|----------|
| fd 数量超限 | 检查 errno，临时拒绝连接 |
| 内存不足 | Worker 退出，Master 重启 |
| Worker 数量超限 | 达到上限后不再重启 |

### 日志策略

```
日志级别：
    - ERROR:   Worker 崩溃、资源耗尽、系统错误
    - WARN:    连接超时、配置异常
    - INFO:    Worker 启动/停止、连接统计
    - DEBUG:   请求详情、事件循环状态

日志输出：
    - Worker:  各进程独立日志文件（可选）
    - Master:  管理日志（进程状态、信号处理）
    - stdout:  前台模式输出
```

## 参考资料

- [SO_REUSEPORT Linux Documentation](https://lwn.net/Articles/542629/)
- [nginx Architecture](https://www.nginx.com/blog/inside-nginx-how-we-designed-performance-scale/)
- [HAProxy Architecture](https://www.haproxy.org/)
- [The C10K Problem](http://www.kegel.com/c10k.html)