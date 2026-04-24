# Chase HTTP Server Examples

本目录包含两个示例程序，展示 Chase HTTP 服务器库的使用方式。

---

## 目录

1. [minimal_server - 最小 HTTP 服务器](#minimal_server---最小-http-服务器)
2. [production_server - 生产级服务器](#production_server---生产级服务器)
3. [SSL 配置示例](#ssl-配置示例)
4. [Security 配置示例](#security-配置示例)
5. [完整运行流程](#完整运行流程)

---

## minimal_server - 最小 HTTP 服务器

### 功能说明

`minimal_server.c` 是一个最小化的 HTTP 服务器示例，直接使用核心模块：

- **EventLoop**: 跨平台事件循环（epoll/kqueue/poll）
- **HttpParser**: HTTP/1.1 增量解析器
- **Router**: URL 路由匹配

**架构特点**:
- 单进程、单线程
- 非阻塞 I/O
- 手动管理连接生命周期
- 适合学习和理解内部工作原理

**适用场景**:
- 学习 HTTP 服务器内部机制
- 快速原型开发
- 单机低并发场景

### 编译步骤

```bash
# 1. 确保依赖已安装
vcpkg install

# 2. 创建构建目录
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 3. 编译
cmake --build build

# 编译产物位于: build/examples/minimal_server
```

### 运行命令

```bash
# 默认端口 8080
./build/examples/minimal_server

# 指定端口
./build/examples/minimal_server 9090

# 输出示例:
# Chase HTTP Server - Minimal Example
# Starting server on port 8080...
# Server socket created (fd=4)
# Server running. Press Ctrl+C to stop.
```

### 测试请求示例

```bash
# 使用 curl 测试
curl http://localhost:8080/
# 输出: Hello World from Chase HTTP Server!

# 使用 wrk 基准测试
wrk -t1 -c10 -d10s --latency http://localhost:8080/

# 预期性能（单 Worker）:
# Requests/sec: ~33,000
# Latency P50: ~112 microseconds
```

### 代码结构说明

```
minimal_server.c 结构概览:

┌─────────────────────────────────────────────────────┐
│  main()                                             │
│  ├── create_server_socket()  创建监听 socket        │
│  │   ├── socket() + bind() + listen()               │
│  │   ├── SO_REUSEADDR                               │
│  │   └── O_NONBLOCK                                 │
│  ├── eventloop_create()     创建事件循环            │
│  ├── eventloop_add()        注册 accept 回调        │
│  └── eventloop_run()        运行主循环              │
│                                                     │
│  on_accept()                 处理新连接             │
│  ├── accept()                                       │
│  ├── fcntl(O_NONBLOCK)                             │
│  └── eventloop_add(fd, on_read)                    │
│                                                     │
│  on_read()                   处理请求               │
│  ├── read()                 读取数据                │
│  ├── http_parser_parse()    解析 HTTP              │
│  ├── router_match()         路由匹配                │
│  ├── build_response()       构建响应                │
│  └── write()                发送响应                │
│  ├── close()                关闭连接                │
└─────────────────────────────────────────────────────┘
```

**关键组件说明**:

| 组件 | 文件 | 功能 |
|------|------|------|
| `EventLoop` | `eventloop.h` | 监听 socket 事件，分发到回调函数 |
| `HttpParser` | `http_parser.h` | 增量解析 HTTP 请求，支持 chunked |
| `Router` | `router.h` | 精确匹配、前缀匹配、正则匹配 |
| `HttpRequest` | `http_parser.h` | 解析后的请求结构（method、path、headers、body） |

**代码要点**:

```c
/* 创建事件循环 */
EventLoop *loop = eventloop_create(1024);  // max_events

/* 注册监听 socket */
eventloop_add(loop, server_fd, EV_READ, on_accept, loop);

/* 运行事件循环 */
eventloop_run(loop);  // 阻塞，直到 eventloop_stop() 被调用
```

---

## production_server - 生产级服务器

### 功能说明

`production_server.c` 是一个生产级 HTTP 服务器示例，使用完整的服务器封装：

- **Master/Worker 多进程架构**
- **SO_REUSEPORT 内核级负载均衡**
- **自动 Worker 崩溃恢复**
- **完整路由系统**

**架构特点**:
- Master 进程管理多个 Worker
- 每个 Worker 独立绑定同一端口（SO_REUSEPORT）
- Worker 崩溃自动重启
- 零 IPC 开销，内核负责连接分发

**适用场景**:
- 生产环境部署
- 高并发场景
- 需要高可用性

### 配置参数详解

#### 命令行参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `argv[1]` | `8080` | 监听端口 |
| `argv[2]` | `4` | Worker 进程数量 |

```bash
# 基本用法
./build/examples/production_server 8080 4

# 高并发配置
./build/examples/production_server 8080 8
```

#### MasterConfig 结构

```c
typedef struct MasterConfig {
    int worker_count;           // Worker 进程数量（建议: CPU 核数）
    int port;                   // 监听端口
    int max_connections;        // 最大连接数（默认: 1024）
    int backlog;                // listen backlog（默认: 1024）
    bool reuseport;             // 启用 SO_REUSEPORT（默认: true）
    const char *bind_addr;      // 绑定地址（NULL = 0.0.0.0）
    void *user_data;            // 用户自定义数据
} MasterConfig;
```

#### ServerConfig 结构

```c
typedef struct ServerConfig {
    int port;                   // 监听端口
    int max_connections;        // 最大连接数
    int backlog;                // listen backlog
    const char *bind_addr;      // 绑定地址
    bool reuseport;             // SO_REUSEPORT 开关
    Router *router;             // 路由器实例
    size_t read_buf_cap;        // 读缓冲区容量（0 = 默认）
    size_t write_buf_cap;       // 写缓冲区容量（0 = 默认）

    // Keep-Alive 配置
    int connection_timeout_ms;  // 连接空闲超时（默认: 60000）
    int keepalive_timeout_ms;   // Keep-Alive 超时（默认: 5000）
    int max_keepalive_requests; // 单连接最大请求数（默认: 100）

    // Phase 5 模块
    Security *security;         // Security 实例（可选）
    Logger *logger;             // Logger 实例（可选）
} ServerConfig;
```

### 编译步骤

```bash
# 与 minimal_server 相同的编译步骤
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 编译产物位于: build/examples/production_server
```

### 运行命令

```bash
# 默认配置（4 Workers，端口 8080）
./build/examples/production_server

# 自定义配置
./build/examples/production_server 9090 8

# 输出示例:
# === Chase HTTP Server - Production ===
# Port: 8080, Workers: 4
# Architecture: Master/Worker + SO_REUSEPORT
#
# [Worker 0] Starting (pid=12345)
# [Worker 1] Starting (pid=12346)
# [Worker 2] Starting (pid=12347)
# [Worker 3] Starting (pid=12348)
```

### 测试请求示例

```bash
# 主页
curl http://localhost:8080/

# API 接口
curl http://localhost:8080/api
# 输出: {"status":"ok","version":"2.0","arch":"master/worker"}

# 健康检查
curl http://localhost:8080/health
# 输出: {"status":"healthy"}

# 使用 wrk 基准测试（多 Worker）
wrk -t4 -c100 -d30s --latency http://localhost:8080/

# 预期性能:
# Requests/sec: ~30,000
# Latency P50: ~1.5ms
```

### 配置文件示例

生产环境推荐使用 JSON 配置文件（通过 config.h 模块加载）:

```json
{
  "port": 8080,
  "bind_address": "0.0.0.0",
  "max_connections": 1024,
  "backlog": 1024,
  "reuseport": true,

  "read_buffer_capacity": 8192,
  "write_buffer_capacity": 16384,

  "connection_timeout_ms": 60000,
  "keepalive_timeout_ms": 5000,
  "max_keepalive_requests": 100,

  "ssl_enabled": false,

  "security": {
    "max_connections_per_ip": 10,
    "max_requests_per_second": 100,
    "min_request_rate": 50,
    "block_duration_ms": 60000
  },

  "logger": {
    "log_file": "/var/log/chase/server.log",
    "min_level": "INFO",
    "format": "text",
    "enable_stdout": false
  }
}
```

---

## SSL 配置示例

### 证书生成步骤

**1. 生成私钥**

```bash
# 生成 RSA 2048 位私钥
openssl genrsa -out server.key 2048

# 或使用 ECC（更高效）
openssl ecparam -genkey -name prime256v1 -out server.key
```

**2. 生成证书签名请求（CSR）**

```bash
openssl req -new -key server.key -out server.csr \
  -subj "/C=CN/ST=Beijing/L=Beijing/O=MyOrg/CN=localhost"
```

**3. 生成自签名证书（开发测试用）**

```bash
openssl x509 -req -days 365 -in server.csr \
  -signkey server.key -out server.crt

# 或一步生成
openssl req -x509 -newkey rsa:2048 -keyout server.key \
  -out server.crt -days 365 -nodes \
  -subj "/C=CN/ST=Beijing/L=Beijing/O=MyOrg/CN=localhost"
```

**4. 使用现有测试证书**

项目已提供测试证书：

```bash
# 测试证书位置
test/certs/test.crt   # 证书
test/certs/test.key   # 私钥
```

### SslConfig 结构

```c
typedef struct SslConfig {
    const char *cert_file;    // 证书文件路径
    const char *key_file;     // 私钥文件路径
    const char *ca_file;      // CA 证书文件路径（可选，用于客户端验证）
    bool verify_peer;         // 是否验证客户端证书
    int verify_depth;         // 证书链验证深度（默认: 1）
    int session_timeout;      // 会话缓存超时（秒，默认: 300）
    bool enable_tickets;      // 是否启用 TLS 1.3 Session Ticket
} SslConfig;
```

### SSL 配置示例

```c
#include "ssl_wrap.h"

SslConfig ssl_config = {
    .cert_file = "/path/to/server.crt",
    .key_file = "/path/to/server.key",
    .ca_file = NULL,           // 不验证客户端
    .verify_peer = false,
    .verify_depth = 1,
    .session_timeout = 300,
    .enable_tickets = true     // TLS 1.3 Session Ticket
};

SslServerCtx *ssl_ctx = ssl_server_ctx_create(&ssl_config);
if (!ssl_ctx) {
    fprintf(stderr, "Failed to create SSL context\n");
    return 1;
}
```

### JSON 配置格式

```json
{
  "ssl_enabled": true,
  "ssl_config": {
    "cert_file": "/etc/ssl/certs/server.crt",
    "key_file": "/etc/ssl/private/server.key",
    "ca_file": null,
    "verify_peer": false,
    "session_timeout": 300,
    "enable_tickets": true
  }
}
```

### HTTPS 测试

```bash
# 启动 HTTPS 服务器
./build/examples/production_server_ssl 8443 4

# 测试 HTTPS
curl -k https://localhost:8443/  # -k 跳过证书验证（自签名）

# 使用 wrk 测试 HTTPS 性能
wrk -t4 -c100 -d30s --latency https://localhost:8443/
```

---

## Security 配置示例

### 功能说明

Security 模块提供以下防护能力：

| 功能 | 默认值 | 说明 |
|------|--------|------|
| 单 IP 连接数限制 | 10 | 防止单 IP 占用过多连接 |
| 请求速率限制 | 100 req/s | 防止 DDoS 攻击 |
| Slowloris 检测 | 50 bytes/sec | 检测慢速攻击 |
| IP 封禁 | 60 秒 | 超限 IP 自动封禁 |
| 分片哈希表 | 16 shards | 降低锁竞争 |

### SecurityConfig 结构

```c
typedef struct SecurityConfig {
    int max_connections_per_ip;     // 单 IP 最大并发连接数
    int max_requests_per_second;    // 单 IP 每秒最大请求数
    int min_request_rate;           // Slowloris 最小字节/秒
    int slowloris_timeout_ms;       // Slowloris 检测超时
    int block_duration_ms;          // IP 封禁持续时间
    int shard_count;                // 分片哈希表分片数
} SecurityConfig;

// 默认值宏定义
#define SECURITY_DEFAULT_MAX_CONN_PER_IP      10
#define SECURITY_DEFAULT_MAX_REQ_PER_SEC      100
#define SECURITY_DEFAULT_MIN_REQ_RATE         50      // bytes/sec
#define SECURITY_DEFAULT_SLOWLORIS_TIMEOUT    30000   // ms
#define SECURITY_DEFAULT_BLOCK_DURATION       60000   // ms
#define SECURITY_DEFAULT_SHARD_COUNT          16
```

### 启用 Security 模块

```c
#include "security.h"
#include "server.h"

// 创建 Security 配置
SecurityConfig sec_config = {
    .max_connections_per_ip = 10,      // 单 IP 最大 10 个连接
    .max_requests_per_second = 100,    // 单 IP 最大 100 req/s
    .min_request_rate = 50,            // 最小 50 bytes/sec
    .slowloris_timeout_ms = 30000,     // 30 秒检测超时
    .block_duration_ms = 60000,        // 封禁 60 秒
    .shard_count = 16                  // 16 分片
};

// 创建 Security 实例
Security *security = security_create(&sec_config);
if (!security) {
    fprintf(stderr, "Failed to create security module\n");
    return 1;
}

// 配置 Server 使用 Security
ServerConfig server_config = {
    .port = 8080,
    .max_connections = 1024,
    .reuseport = true,
    .router = router,
    .security = security,    // 启用 Security
    .logger = NULL           // 可选：启用 Logger
};

Server *server = server_create(&server_config);
```

### 参数调优建议

#### 高并发场景

```c
SecurityConfig sec_config = {
    .max_connections_per_ip = 50,      // 适当放宽连接限制
    .max_requests_per_second = 500,    // 提高速率上限
    .min_request_rate = 20,            // 降低 Slowloris 检测阈值
    .slowloris_timeout_ms = 60000,     // 延长检测超时
    .block_duration_ms = 30000,        // 缩短封禁时间
    .shard_count = 32                  // 增加分片数
};
```

#### 高安全场景

```c
SecurityConfig sec_config = {
    .max_connections_per_ip = 5,       // 严格限制连接数
    .max_requests_per_second = 50,     // 严格限制请求速率
    .min_request_rate = 100,           // 提高 Slowloris 检测阈值
    .slowloris_timeout_ms = 15000,     // 快速检测
    .block_duration_ms = 300000,       // 延长封禁时间（5分钟）
    .shard_count = 16                  // 标准分片数
};
```

#### API 服务场景

```c
SecurityConfig sec_config = {
    .max_connections_per_ip = 20,      // 允许中等连接数
    .max_requests_per_second = 200,    // 允许较高请求速率
    .min_request_rate = 30,            // 标准检测阈值
    .slowloris_timeout_ms = 20000,     // 中等超时
    .block_duration_ms = 60000,        // 标准封禁时间
    .shard_count = 16
};
```

### JSON 配置格式

```json
{
  "security": {
    "max_connections_per_ip": 10,
    "max_requests_per_second": 100,
    "min_request_rate": 50,
    "slowloris_timeout_ms": 30000,
    "block_duration_ms": 60000,
    "shard_count": 16
  }
}
```

---

## Logger 配置示例

### 功能说明

Logger 模块提供异步日志记录能力：

| 功能 | 说明 |
|------|------|
| Ring Buffer | 异步非阻塞写入（默认 64KB） |
| 日志级别 | DEBUG / INFO / WARN / ERROR / SECURITY |
| 请求日志 | 记录请求延迟、状态码、路径 |
| 安全审计 | 路径穿越、速率限制触发 |
| 格式支持 | 文本格式 / JSON 格式 |

### LoggerConfig 结构

```c
typedef struct LoggerConfig {
    const char *log_file;       // 日志文件路径
    const char *audit_file;     // 审计日志文件路径（可选）
    LogLevel min_level;         // 最小日志级别
    LogFormat format;           // 日志格式（LOG_FORMAT_TEXT / LOG_FORMAT_JSON）
    int ring_buffer_size;       // Ring Buffer 大小（字节）
    int flush_interval_ms;      // 刷新间隔（毫秒）
    bool enable_stdout;         // 同时输出到 stdout
} LoggerConfig;

// 日志级别枚举
typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_SECURITY    // 安全审计
} LogLevel;

// 默认值
#define LOGGER_DEFAULT_RING_BUFFER_SIZE   (64 * 1024)  // 64KB
#define LOGGER_DEFAULT_FLUSH_INTERVAL     1000          // 1秒
#define LOGGER_DEFAULT_MIN_LEVEL          LOG_INFO
```

### 启用 Logger 模块

```c
#include "logger.h"

LoggerConfig log_config = {
    .log_file = "/var/log/chase/server.log",
    .audit_file = "/var/log/chase/audit.log",  // 可选
    .min_level = LOG_INFO,
    .format = LOG_FORMAT_TEXT,
    .ring_buffer_size = 64 * 1024,   // 64KB
    .flush_interval_ms = 1000,       // 1秒
    .enable_stdout = true            // 开发环境建议启用
};

Logger *logger = logger_create(&log_config);

// 配置 Server 使用 Logger
ServerConfig server_config = {
    .logger = logger    // 启用 Logger
};
```

---

## 完整运行流程

### 1. 启动服务器

```bash
# 编译项目
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 启动生产服务器
./build/examples/production_server 8080 4

# 或使用自定义配置启动
./build/examples/production_server 9090 8
```

### 2. 测试服务器

```bash
# 基本功能测试
curl http://localhost:8080/
curl http://localhost:8080/api
curl http://localhost:8080/health

# 性能基准测试
wrk -t4 -c100 -d30s --latency http://localhost:8080/

# 并发测试
wrk -t8 -c500 -d60s --latency http://localhost:8080/
```

### 3. 监控服务器

```bash
# 查看进程状态
ps aux | grep production_server

# 查看端口监听
lsof -i :8080

# 实时监控（如果启用 Logger）
tail -f /var/log/chase/server.log

# 安全审计日志
tail -f /var/log/chase/audit.log
```

### 4. 压力测试

```bash
# 高并发压力测试
wrk -t16 -c1000 -d120s --latency http://localhost:8080/

# 模拟 DDoS（观察 Security 模块响应）
wrk -t4 -c200 -d60s http://localhost:8080/

# Slowloris 检测测试（使用慢速请求工具）
# Security 模块会自动检测并封禁
```

### 5. 关闭服务器

```bash
# 平滑关闭（推荐）
# 发送 SIGTERM 或 SIGINT 信号
kill -TERM <master_pid>

# 或使用 Ctrl+C（发送 SIGINT）

# 强制关闭（不推荐，可能导致连接中断）
kill -KILL <master_pid>
```

### 6. 完整示例脚本

```bash
#!/bin/bash
# chase_server.sh - 完整运行脚本

PORT=8080
WORKERS=4
LOG_FILE="/var/log/chase/server.log"

echo "=== Chase HTTP Server Startup ==="

# 创建日志目录
mkdir -p /var/log/chase

# 启动服务器
./build/examples/production_server $PORT $WORKERS &
MASTER_PID=$!

echo "Master PID: $MASTER_PID"
echo "Server running on port $PORT"

# 等待启动完成
sleep 2

# 基本测试
echo "Testing endpoints..."
curl -s http://localhost:$PORT/ | head -1
curl -s http://localhost:$PORT/api
curl -s http://localhost:$PORT/health

echo ""
echo "Server ready. Press Ctrl+C to stop."

# 等待信号
trap "echo 'Stopping server...'; kill -TERM $MASTER_PID; exit 0" SIGINT SIGTERM

wait $MASTER_PID
```

---

## 架构对比

| 特性 | minimal_server | production_server |
|------|----------------|-------------------|
| 架构 | 单进程单线程 | Master/Worker 多进程 |
| 负载均衡 | 无 | SO_REUSEPORT 内核级 |
| 高可用 | 无 | Worker 崩溃自动恢复 |
| 路由 | 手动创建 | 完整 Router 系统 |
| Security | 无 | 可选集成 |
| Logger | 无 | 可选集成 |
| SSL/TLS | 无 | 可选集成 |
| 性能 | ~33k req/s | ~30k req/s（多核） |
| 适用场景 | 学习、原型 | 生产环境 |

---

## 相关文档

- [主 README](../README.md)
- [架构设计文档](../docs/superpowers/architecture/architecture.md)
- [API 文档](../docs/api.md)
- [安全配置指南](../docs/security.md)

---

## 常见问题

### Q1: Worker 进程崩溃后如何自动恢复？

Master 进程监控 Worker 状态，崩溃后自动重启。可通过 `master_set_restart_policy()` 配置重启策略：

```c
master_set_restart_policy(master, 10, 1000);  // 最多重启 10 次，延迟 1 秒
```

### Q2: 如何选择 Worker 数量？

建议 Worker 数量等于 CPU 核数：

```bash
# 获取 CPU 核数
nproc  # Linux
sysctl -n hw.ncpu  # macOS

# 启动服务器
./build/examples/production_server 8080 $(nproc)
```

### Q3: SO_REUSEPORT 有什么优势？

- 内核级负载均衡，无需 Master 分发连接
- 每个 Worker 独立 accept，避免锁竞争
- Worker 崩溃不影响其他 Worker 继续服务

### Q4: 如何启用 HTTPS？

参考 [SSL 配置示例](#ssl-配置示例)章节，生成证书并配置 `SslConfig`。

### Q5: Security 模块如何防止 DDoS？

- 单 IP 连接数限制：防止连接耗尽攻击
- 请求速率限制：防止请求洪泛
- Slowloris 检测：防止慢速攻击
- IP 自动封禁：超限 IP 临时封禁

---

## License

MIT License