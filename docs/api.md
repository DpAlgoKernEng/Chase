# Chase HTTP Server API 参考

**版本**: v0.1.0  
**更新日期**: 2026-04-24

---

## 目录

- [Phase 1: 核心框架](#phase-1-核心框架)
  - [EventLoop](#eventloop)
  - [Timer](#timer)
  - [Buffer](#buffer)
  - [Socket](#socket)
  - [Error](#error)
  - [Connection](#connection)
  - [ConnectionPool](#connectionpool)
  - [HttpParser](#httpparser)
  - [Router](#router)
  - [Handler](#handler)
  - [Response](#response)
  - [Mime](#mime)
  - [FileServe](#fileserve)
  - [Server](#server)
- [Phase 2: 多进程架构](#phase-2-多进程架构)
  - [Master](#master)
  - [Worker](#worker)
- [Phase 4: SSL/TLS + 虚拟主机](#phase-4-ssltls--虚拟主机)
  - [SSL](#ssl)
  - [VHost](#vhost)
  - [Config](#config)
- [Phase 5: 安全防护 + 日志系统](#phase-5-安全防护--日志系统)
  - [Security](#security)
  - [Logger](#logger)

---

## Phase 1: 核心框架

### EventLoop

I/O 事件循环，支持 epoll/kqueue/poll 多后端。

**头文件**: `include/eventloop.h`

#### 事件类型常量

```c
#define EV_READ    0x01   // 可读事件
#define EV_WRITE   0x02   // 可写事件
#define EV_ERROR   0x04   // 错误事件
#define EV_CLOSE   0x08   // 关闭事件
```

#### 回调函数类型

```c
typedef void (*EventCallback)(int fd, uint32_t events, void *user_data);
```

#### API

```c
// 创建事件循环
EventLoop *eventloop_create(int max_events);

// 销毁事件循环
void eventloop_destroy(EventLoop *loop);

// 添加 I/O 事件监听
int eventloop_add(EventLoop *loop, int fd, uint32_t events,
                  EventCallback cb, void *user_data);

// 修改 I/O 事件监听
int eventloop_modify(EventLoop *loop, int fd, uint32_t events);

// 移除 I/O 事件监听
int eventloop_remove(EventLoop *loop, int fd);

// 运行事件循环（阻塞）
void eventloop_run(EventLoop *loop);

// 停止事件循环
void eventloop_stop(EventLoop *loop);

// 单次事件轮询（非阻塞）
int eventloop_poll(EventLoop *loop, int timeout_ms);
```

#### 使用示例

```c
#include "eventloop.h"

void on_read(int fd, uint32_t events, void *user_data) {
    char buf[1024];
    int n = read(fd, buf, sizeof(buf));
    // 处理数据...
}

EventLoop *loop = eventloop_create(1024);
eventloop_add(loop, client_fd, EV_READ, on_read, NULL);
eventloop_run(loop);
eventloop_destroy(loop);
```

---

### Timer

定时器管理，基于最小堆实现高效超时调度。

**头文件**: `include/timer.h`

#### 回调函数类型

```c
typedef void (*TimerCallback)(void *user_data);
```

#### API

```c
// 创建定时器堆
TimerHeap *timer_heap_create(int capacity);

// 销毁定时器堆
void timer_heap_destroy(TimerHeap *heap);

// 创建并添加定时器
Timer *timer_heap_add(TimerHeap *heap, uint64_t timeout_ms,
                      TimerCallback cb, void *user_data, bool periodic);

// 移除定时器
int timer_heap_remove(TimerHeap *heap, Timer *timer);

// 获取堆顶定时器（最小过期时间）
Timer *timer_heap_peek(TimerHeap *heap);

// 弹出堆顶定时器
Timer *timer_heap_pop(TimerHeap *heap);

// 释放定时器
void timer_free(Timer *timer);

// 获取定时器过期时间
uint64_t timer_get_expire_time(Timer *timer);

// 获取定时器 ID
uint64_t timer_get_id(Timer *timer);

// 检查堆是否为空
bool timer_heap_is_empty(TimerHeap *heap);

// 获取堆大小
int timer_heap_size(TimerHeap *heap);
```

#### 使用示例

```c
#include "timer.h"

void on_timeout(void *user_data) {
    printf("Timer fired!\n");
}

TimerHeap *heap = timer_heap_create(100);

// 单次定时器（5秒后触发）
Timer *timer = timer_heap_add(heap, 5000, on_timeout, NULL, false);

// 周期性定时器（每1秒触发）
Timer *periodic = timer_heap_add(heap, 1000, on_timeout, NULL, true);

timer_heap_destroy(heap);
```

---

### Buffer

环形缓冲区，支持固定容量和自动扩容模式。

**头文件**: `include/buffer.h`

#### 缓冲区模式

```c
typedef enum {
    BUFFER_MODE_FIXED,   // 固定容量（安全优先）
    BUFFER_MODE_AUTO     // 自动扩容（灵活性优先）
} BufferMode;
```

#### 默认配置常量

```c
#define BUFFER_DEFAULT_READ_CAP   (16 * 1024)    // 16KB
#define BUFFER_DEFAULT_WRITE_CAP  (64 * 1024)    // 64KB
#define BUFFER_DEFAULT_MAX_CAP    (1024 * 1024)  // 1MB
```

#### API

```c
// 创建缓冲区
Buffer *buffer_create(size_t capacity);

// 创建缓冲区（扩展参数）
Buffer *buffer_create_ex(size_t capacity, BufferMode mode, size_t max_cap);

// 销毁缓冲区
void buffer_destroy(Buffer *buf);

// 写入数据到缓冲区
int buffer_write(Buffer *buf, const char *data, size_t len);

// 从缓冲区读取数据
int buffer_read(Buffer *buf, char *data, size_t len);

// 获取可读数据量
size_t buffer_available(Buffer *buf);

// 获取缓冲区容量
size_t buffer_capacity(Buffer *buf);

// 获取剩余可写空间
size_t buffer_remaining(Buffer *buf);

// 检查缓冲区是否为空
bool buffer_is_empty(Buffer *buf);

// 检查缓冲区是否已满
bool buffer_is_full(Buffer *buf);

// 清空缓冲区
void buffer_clear(Buffer *buf);

// 获取缓冲区数据指针（不移动索引）
const char *buffer_peek(Buffer *buf, size_t *len);

// 跳过指定字节数
size_t buffer_skip(Buffer *buf, size_t len);
```

---

### Socket

Socket 操作封装，提供统一的 socket 创建和配置接口。

**头文件**: `include/socket.h`

#### Socket 选项配置

```c
typedef struct SocketOptions {
    bool nonblock;       // 是否设置非阻塞
    bool reuseaddr;      // 是否设置 SO_REUSEADDR
    bool reuseport;      // 是否设置 SO_REUSEPORT
    bool tcp_nodelay;    // 是否设置 TCP_NODELAY
    int backlog;         // listen backlog
} SocketOptions;

#define SOCKET_OPTIONS_DEFAULT { \
    .nonblock = true, \
    .reuseaddr = true, \
    .reuseport = true, \
    .tcp_nodelay = true, \
    .backlog = 128 \
}
```

#### API

```c
// 创建服务端监听 socket
int socket_create_server(int port, const char *bind_addr, const SocketOptions *options);

// 创建服务端监听 socket（使用默认选项）
int socket_create_server_default(int port, const char *bind_addr, int backlog);

// 设置 socket 非阻塞
int socket_set_nonblock(int fd);

// 设置 SO_REUSEADDR
int socket_set_reuseaddr(int fd);

// 设置 SO_REUSEPORT
int socket_set_reuseport(int fd);

// 设置 TCP_NODELAY
int socket_set_tcp_nodelay(int fd);

// 设置 TCP_CORK（Linux only）
int socket_set_tcp_cork(int fd, bool enable);

// 设置发送缓冲区大小
int socket_set_send_buffer(int fd, int size);

// 设置接收缓冲区大小
int socket_set_recv_buffer(int fd, int size);

// 关闭 socket
void socket_close(int fd);

// 检查 SO_REUSEPORT 是否支持
bool socket_has_reuseport(void);
```

---

### Error

错误码和 HTTP 状态码定义。

**头文件**: `include/error.h`

#### 错误码枚举

```c
typedef enum {
    ERR_OK = 0,
    ERR_SOCKET_FAILED,
    ERR_BIND_FAILED,
    ERR_LISTEN_FAILED,
    ERR_ACCEPT_FAILED,
    ERR_RECV_FAILED,
    ERR_SEND_FAILED,
    ERR_SSL_INIT_FAILED,
    ERR_SSL_HANDSHAKE_FAILED,
    ERR_PARSE_FAILED,
    ERR_MEMORY_FAILED,
    ERR_CONFIG_FAILED,
    ERR_NOT_FOUND,
    ERR_FORBIDDEN,
    ERR_INTERNAL,
    ERR_RATE_LIMITED,
    ERR_TOO_MANY_CONNECTIONS,
    ERR_BUFFER_OVERFLOW,
    ERR_TIMER_NOT_FOUND
} ErrorCode;
```

#### HTTP 状态码枚举

```c
typedef enum {
    HTTP_STATUS_CONTINUE           = 100,
    HTTP_STATUS_OK                 = 200,
    HTTP_STATUS_CREATED            = 201,
    HTTP_STATUS_NO_CONTENT         = 204,
    HTTP_STATUS_PARTIAL_CONTENT    = 206,
    HTTP_STATUS_BAD_REQUEST        = 400,
    HTTP_STATUS_UNAUTHORIZED       = 401,
    HTTP_STATUS_FORBIDDEN          = 403,
    HTTP_STATUS_NOT_FOUND          = 404,
    HTTP_STATUS_METHOD_NOT_ALLOWED = 405,
    HTTP_STATUS_PAYLOAD_TOO_LARGE  = 413,
    HTTP_STATUS_URI_TOO_LONG       = 414,
    HTTP_STATUS_RANGE_NOT_SATISFIABLE = 416,
    HTTP_STATUS_INTERNAL_ERROR     = 500,
    HTTP_STATUS_NOT_IMPLEMENTED    = 501,
    HTTP_STATUS_SERVICE_UNAVAIL    = 503,
    HTTP_STATUS_GATEWAY_TIMEOUT    = 504
} HttpStatus;
```

#### API

```c
// 获取错误码描述
const char *error_get_description(ErrorCode code);

// 错误码转换为 HTTP 状态码
HttpStatus error_to_http_status(ErrorCode code);

// 获取 HTTP 状态码描述
const char *http_status_get_description(HttpStatus status);
```

---

### Connection

TCP 连接管理，处理连接的读/写/状态管理。

**头文件**: `include/connection.h`

#### 连接状态枚举

```c
typedef enum {
    CONN_STATE_CONNECTING,
    CONN_STATE_SSL_HANDSHAKING,
    CONN_STATE_READING,
    CONN_STATE_PROCESSING,
    CONN_STATE_WRITING,
    CONN_STATE_CLOSING,
    CONN_STATE_CLOSED
} ConnState;
```

#### 回调函数类型

```c
typedef void (*ConnectionCloseCallback)(int fd, void *user_data);
```

#### API

```c
// 创建连接
Connection *connection_create(int fd,
                              ConnectionCloseCallback on_close,
                              void *close_user_data);

// 创建连接（扩展参数）
Connection *connection_create_ex(int fd,
                                  ConnectionCloseCallback on_close,
                                  void *close_user_data,
                                  size_t read_buf_cap,
                                  size_t write_buf_cap,
                                  BufferMode mode);

// 销毁连接
void connection_destroy(Connection *conn);

// 从连接读取数据
int connection_read(Connection *conn);

// 向连接写入数据
int connection_write(Connection *conn);

// 关闭连接
void connection_close(Connection *conn);

// 设置连接状态
void connection_set_state(Connection *conn, ConnState state);

// 获取连接状态
ConnState connection_get_state(Connection *conn);

// 获取连接的文件描述符
int connection_get_fd(Connection *conn);

// 获取读缓冲区
Buffer *connection_get_read_buffer(Connection *conn);

// 获取写缓冲区
Buffer *connection_get_write_buffer(Connection *conn);

// 获取/设置用户数据
void *connection_get_user_data(Connection *conn);
void connection_set_user_data(Connection *conn, void *user_data);

// 重置连接
void connection_reset(Connection *conn);

// 从池初始化连接
int connection_init_from_pool(Connection *conn, int fd,
                               ConnectionCloseCallback on_close,
                               void *close_user_data);

// 解除 fd 关联
void connection_dissociate_fd(Connection *conn);
```

---

### ConnectionPool

连接池管理，预分配和复用 Connection 对象。

**头文件**: `include/connection_pool.h`

#### 池统计信息结构

```c
typedef struct PoolStats {
    int base_capacity;      // 预分配容量
    int free_count;         // 空闲连接数
    int active_count;       // 活跃连接数
    int temp_allocated;     // 临时分配的连接数
    int lazy_release_count; // 惰性释放队列中的连接数
    float utilization;      // 利用率
} PoolStats;
```

#### API

```c
// 创建连接池
ConnectionPool *connection_pool_create(int base_capacity);

// 销毁连接池
void connection_pool_destroy(ConnectionPool *pool);

// 从池获取 Connection
Connection *connection_pool_get(ConnectionPool *pool);

// 释放 Connection 到池
void connection_pool_release(ConnectionPool *pool, Connection *conn);

// 惰性释放检查（定时器调用）
void connection_pool_lazy_release_check(ConnectionPool *pool);

// 获取池统计信息
PoolStats connection_pool_get_stats(ConnectionPool *pool);

// 检查是否需要扩容
int connection_pool_should_expand(ConnectionPool *pool);

// 获取基础容量
int connection_pool_get_base_capacity(ConnectionPool *pool);

// 获取空闲连接数
int connection_pool_get_free_count(ConnectionPool *pool);
```

---

### HttpParser

HTTP/1.1 请求解析器，增量式状态机实现。

**头文件**: `include/http_parser.h`

#### HTTP 方法枚举

```c
typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE,
    HTTP_HEAD,
    HTTP_OPTIONS,
    HTTP_PATCH
} HttpMethod;
```

#### 解析结果枚举

```c
typedef enum {
    PARSE_OK,          // 解析成功
    PARSE_NEED_MORE,   // 需要更多数据
    PARSE_ERROR,       // 解析错误
    PARSE_COMPLETE     // 解析完成
} ParseResult;
```

#### Phase 5: 解压结果枚举

```c
typedef enum {
    DECOMPRESS_OK,             // 解压成功
    DECOMPRESS_NOT_NEEDED,     // 无需解压
    DECOMPRESS_ERROR,          // 解压错误
    DECOMPRESS_ZIP_BOMB,       // Zip Bomb 检测
    DECOMPRESS_SIZE_EXCEEDED   // 解压后大小超限
} DecompressResult;
```

#### HTTP 请求结构

```c
typedef struct HttpRequest {
    HttpMethod method;
    char *path;
    char *query;
    char *version;
    HttpHeader *headers;
    int header_count;
    int header_capacity;
    char *body;
    size_t body_length;
    size_t content_length;
    bool is_chunked;            // Phase 3
    size_t chunk_body_capacity; // Phase 3
    bool needs_decompression;   // Phase 5
    const char *content_encoding;
    DecompressResult decompress_result;
    size_t original_body_size;
} HttpRequest;
```

#### API

```c
// 创建 HTTP 解析器
HttpParser *http_parser_create(void);

// 销毁 HTTP 解析器
void http_parser_destroy(HttpParser *parser);

// 创建 HTTP 请求
HttpRequest *http_request_create(void);

// 销毁 HTTP 请求
void http_request_destroy(HttpRequest *req);

// 解析 HTTP 数据
ParseResult http_parser_parse(HttpParser *parser, HttpRequest *req,
                              const char *data, size_t len, size_t *consumed);

// 获取请求头
HttpHeader *http_request_get_header(HttpRequest *req, const char *name);

// 获取请求头值
const char *http_request_get_header_value(HttpRequest *req, const char *name);

// 重置解析器（用于 Keep-Alive）
void http_parser_reset(HttpParser *parser);

/* Phase 5: Gzip/Deflate 解压 API */

// 设置解压配置
int http_parser_set_decompress_config(HttpParser *parser, const DecompressConfig *config);

// 解压请求 body
DecompressResult http_request_decompress_body(HttpRequest *req, HttpParser *parser);

// 检测 Zip Bomb
bool http_detect_zip_bomb(size_t original_size, size_t decompressed_size, double max_ratio);

// 获取 Content-Encoding
const char *http_request_get_content_encoding(HttpRequest *req);

// 检查是否需要解压
bool http_request_needs_decompression(HttpRequest *req);
```

---

### Router

URL 路由器，支持精确匹配、前缀匹配和正则匹配。

**头文件**: `include/router.h`

#### 路由匹配类型

```c
typedef enum {
    ROUTER_MATCH_EXACT,   // 精确匹配
    ROUTER_MATCH_PREFIX,  // 前缀匹配
    ROUTER_MATCH_REGEX    // 正则匹配
} RouteMatchType;
```

#### HTTP 方法掩码

```c
#define METHOD_GET    (1 << HTTP_GET)
#define METHOD_POST   (1 << HTTP_POST)
#define METHOD_PUT    (1 << HTTP_PUT)
#define METHOD_DELETE (1 << HTTP_DELETE)
#define METHOD_HEAD   (1 << HTTP_HEAD)
#define METHOD_ALL    0xFF
```

#### 优先级常量

```c
#define PRIORITY_HIGH    100
#define PRIORITY_NORMAL  50
#define PRIORITY_LOW     10
```

#### Phase 5: 冲突处理策略

```c
typedef enum {
    ROUTER_CONFLICT_WARN,    // 警告但允许添加
    ROUTER_CONFLICT_ERROR,   // 拒绝添加冲突路由
    ROUTER_CONFLICT_OVERRIDE // 覆盖已有路由
} RouterConflictPolicy;
```

#### Phase 5: 正则匹配结果

```c
typedef struct RegexMatchResult {
    char **groups;       // 捕获组字符串数组
    int group_count;     // 捕获组数量
    int *group_starts;   // 每组起始位置
    int *group_ends;     // 每组结束位置
} RegexMatchResult;
```

#### API

```c
// 创建路由器
Router *router_create(void);

// 销毁路由器
void router_destroy(Router *router);

// 添加路由
int router_add_route(Router *router, Route *route);

// 添加路由（扩展参数）
int router_add_route_ex(Router *router, Route *route, int priority);

// 匹配路由
Route *router_match(Router *router, const char *path, HttpMethod method);

// 创建 Route
Route *route_create(RouteMatchType type, const char *pattern,
                    RouteHandler handler, void *user_data);

// 销毁 Route
void route_destroy(Route *route);

/* Phase 5: 正则匹配 API */

// 添加正则路由
int router_add_regex_route(Router *router, const char *pattern,
                           RouteHandler handler, void *user_data,
                           int methods, int priority);

// 匹配路由（带正则捕获组）
Route *router_match_ex(Router *router, const char *path, HttpMethod method,
                       RegexMatchResult *result);

// 检测路由冲突
int router_detect_conflicts(Router *router, Route *route);

// 设置冲突处理策略
void router_set_conflict_policy(Router *router, RouterConflictPolicy policy);

// 释放正则匹配结果
void regex_match_result_free(RegexMatchResult *result);
```

---

### Handler

预置 HTTP 请求处理器。

**头文件**: `include/handler.h`

#### 处理器函数签名

```c
typedef void (*RequestHandler)(HttpRequest *req, HttpResponse *resp, void *user_data);
```

#### API

```c
// 静态文件处理器（user_data: FileServe* 指针）
void handler_static_file(HttpRequest *req, HttpResponse *resp, void *user_data);

// JSON API 处理器（user_data: const char* JSON 字符串）
void handler_json_api(HttpRequest *req, HttpResponse *resp, void *user_data);

// 404 Not Found 处理器
void handler_404(HttpRequest *req, HttpResponse *resp, void *user_data);

// 500 Internal Error 处理器
void handler_500(HttpRequest *req, HttpResponse *resp, void *user_data);

// 简单文本处理器（user_data: const char* 文本内容）
void handler_text(HttpRequest *req, HttpResponse *resp, void *user_data);
```

---

### Response

HTTP 响应构建器。

**头文件**: `include/response.h`

#### 发送结果状态

```c
typedef enum {
    RESPONSE_SEND_COMPLETE,   // 完全发送完成
    RESPONSE_SEND_PARTIAL,    // 部分发送，需要继续
    RESPONSE_SEND_ERROR       // 发送错误
} ResponseSendStatus;
```

#### 发送结果结构

```c
typedef struct {
    ResponseSendStatus status;
    size_t bytes_sent;
    size_t total_bytes;
} ResponseSendResult;
```

#### API

```c
// 创建 HTTP 响应
HttpResponse *response_create(HttpStatus status);

// 销毁 HTTP 响应
void response_destroy(HttpResponse *resp);

// 设置响应头
void response_set_header(HttpResponse *resp, const char *key, const char *value);

// 设置响应体
void response_set_body(HttpResponse *resp, const char *body, size_t len);

// 设置 JSON 响应体
void response_set_body_json(HttpResponse *resp, const char *json);

// 设置 HTML 响应体
void response_set_body_html(HttpResponse *resp, const char *html, size_t len);

// 设置/获取响应状态码
void response_set_status(HttpResponse *resp, HttpStatus status);
HttpStatus response_get_status(HttpResponse *resp);

// 构建完整 HTTP 响应字符串
int response_build(HttpResponse *resp, char *buf, size_t buf_size);

// 发送响应到 socket fd
int response_send(HttpResponse *resp, int fd);

// 发送响应（带详细状态）
ResponseSendResult response_send_ex(HttpResponse *resp, int fd);

// 获取待发送的剩余数据
const char *response_get_pending(HttpResponse *resp, size_t *offset, size_t *len);

// 发送剩余数据
int response_send_remaining(HttpResponse *resp, int fd, size_t offset, size_t len);
```

---

### Mime

MIME 类型注册表。

**头文件**: `include/mime.h`

#### 常用 MIME 类型常量

```c
#define MIME_TEXT_HTML       "text/html"
#define MIME_TEXT_CSS        "text/css"
#define MIME_TEXT_JS         "text/javascript"
#define MIME_TEXT_JSON       "application/json"
#define MIME_TEXT_PLAIN      "text/plain"
#define MIME_IMAGE_JPEG      "image/jpeg"
#define MIME_IMAGE_PNG       "image/png"
#define MIME_IMAGE_GIF       "image/gif"
#define MIME_IMAGE_SVG       "image/svg+xml"
#define MIME_VIDEO_MP4       "video/mp4"
#define MIME_AUDIO_MP3       "audio/mpeg"
#define MIME_APP_PDF         "application/pdf"
#define MIME_APP_ZIP         "application/zip"
#define MIME_APP_OCTET       "application/octet-stream"
```

#### MIME 类型结构

```c
typedef struct {
    const char *type;    // MIME 类型字符串
    const char *charset; // 字符集（可选）
} MimeType;
```

#### API

```c
// 创建 MIME 注册表
MimeRegistry *mime_registry_create(void);

// 销毁 MIME 注册表
void mime_registry_destroy(MimeRegistry *registry);

// 根据文件扩展名获取 MIME 类型
const char *mime_registry_get_type(MimeRegistry *registry, const char *extension);

// 根据文件路径获取 MIME 类型
MimeType mime_registry_get_type_from_path(MimeRegistry *registry, const char *path);

// 添加自定义 MIME 类型映射
int mime_registry_add_type(MimeRegistry *registry, const char *extension, const char *mime_type);

// 根据文件路径获取 MIME 类型（使用默认映射）
const char *mime_get_type_by_path(const char *path);
```

---

### FileServe

静态文件服务。

**头文件**: `include/fileserve.h`

#### 文件服务结果枚举

```c
typedef enum {
    FILESERVE_OK,               // 成功
    FILESERVE_NOT_FOUND,        // 文件不存在
    FILESERVE_FORBIDDEN,        // 权限拒绝
    FILESERVE_INTERNAL_ERROR,   // 内部错误
    FILESERVE_RANGE_INVALID     // Range 请求无效
} FileServeResult;
```

#### Range 请求信息结构

```c
typedef struct {
    bool has_range;       // 是否有 Range 请求
    uint64_t start;       // 起始位置
    uint64_t end;         // 结束位置
    uint64_t total_size;  // 文件总大小
} RangeInfo;
```

#### 文件信息结构

```c
typedef struct {
    char *path;           // 文件路径
    uint64_t size;        // 文件大小
    MimeType mime;        // MIME 类型
    bool is_readable;     // 是否可读
} FileInfo;
```

#### API

```c
// 创建文件服务模块
FileServe *fileserve_create(const char *root_dir);

// 销毁文件服务模块
void fileserve_destroy(FileServe *fs);

// 设置根目录
int fileserve_set_root_dir(FileServe *fs, const char *root_dir);

// 获取根目录
const char *fileserve_get_root_dir(FileServe *fs);

// 解析并验证请求路径（防止路径穿越）
FileServeResult fileserve_resolve_path(FileServe *fs, const char *request_path,
                                        char *resolved_path, size_t max_len);

// 获取文件信息
FileServeResult fileserve_get_file_info(FileServe *fs, const char *path, FileInfo *info);

// 解析 Range 请求头
int fileserve_parse_range(const char *range_header, uint64_t file_size, RangeInfo *range_info);

// 发送文件（使用 sendfile 零拷贝）
FileServeResult fileserve_send_file(FileServe *fs, int fd, const char *path,
                                     RangeInfo *range, uint64_t *bytes_sent);

// 读取文件内容
FileServeResult fileserve_read_file(FileServe *fs, const char *path,
                                     uint64_t offset, size_t length,
                                     FileReadCallback callback, void *user_data);

// 检查路径是否安全
bool fileserve_is_path_safe(const char *resolved_path, const char *root_dir);

// 添加自定义 MIME 类型映射
int fileserve_add_mime_type(FileServe *fs, const char *extension, const char *mime_type);
```

---

### Server

HTTP 服务器封装层。

**头文件**: `include/server.h`

#### Server 配置结构

```c
typedef struct ServerConfig {
    int port;                    // 监听端口
    int max_connections;         // 最大连接数
    int backlog;                 // listen backlog
    const char *bind_addr;       // 绑定地址
    bool reuseport;              // 是否启用 SO_REUSEPORT
    Router *router;              // 外部传入的 Router
    size_t read_buf_cap;         // 读缓冲区容量
    size_t write_buf_cap;        // 写缓冲区容量
    
    // Phase 3: Keep-Alive
    int connection_timeout_ms;   // 连接空闲超时
    int keepalive_timeout_ms;    // Keep-Alive 超时
    int max_keepalive_requests;  // 单连接最大请求数
    
    // Phase 5: Security 和 Logger
    Security *security;          // Security 实例
    Logger *logger;              // Logger 实例
} ServerConfig;
```

#### API

```c
// 创建 Server
Server *server_create(const ServerConfig *config);

// 销毁 Server
void server_destroy(Server *server);

// 运行 Server（阻塞）
int server_run(Server *server);

// 停止 Server
void server_stop(Server *server);

// 获取 Server 的监听 fd
int server_get_fd(Server *server);

// 获取 Server 的 EventLoop
EventLoop *server_get_eventloop(Server *server);

// 获取 Server 的 Router
Router *server_get_router(Server *server);

// 获取 Server 的 Security 实例
Security *server_get_security(Server *server);

// 获取 Server 的 Logger 实例
Logger *server_get_logger(Server *server);
```

---

## Phase 2: 多进程架构

### Master

Master 进程管理，监控和重启 Worker 进程。

**头文件**: `include/master.h`

#### Worker 进程状态枚举

```c
typedef enum {
    WORKER_STATE_IDLE,      // 空闲
    WORKER_STATE_RUNNING,   // 运行中
    WORKER_STATE_STOPPING,  // 正在停止
    WORKER_STATE_CRASHED,   // 已崩溃
    WORKER_STATE_STOPPED    // 已停止
} WorkerState;
```

#### Master 配置结构

```c
typedef struct MasterConfig {
    int worker_count;        // Worker 进程数量
    int port;                // 监听端口
    int max_connections;     // 最大连接数
    int backlog;             // listen backlog
    bool reuseport;          // 是否启用 SO_REUSEPORT
    const char *bind_addr;   // 绑定地址
    void *user_data;         // 用户自定义数据
} MasterConfig;
```

#### Worker 信息结构

```c
typedef struct WorkerInfo {
    pid_t pid;                // 进程 ID
    int id;                   // Worker 序号
    WorkerState state;        // 当前状态
    uint64_t restart_count;   // 重启次数
    uint64_t crash_count;     // 崩溃次数
    time_t start_time;        // 启动时间
    time_t last_crash_time;   // 上次崩溃时间
} WorkerInfo;
```

#### API

```c
// 创建 Master 进程上下文
Master *master_create(const MasterConfig *config);

// 销毁 Master 进程上下文
void master_destroy(Master *master);

// 启动所有 Worker 进程
int master_start_workers(Master *master);

// 停止所有 Worker 进程（平滑关闭）
int master_stop_workers(Master *master, int timeout_ms);

// 重启指定 Worker 进程
int master_restart_worker(Master *master, int worker_id);

// 运行 Master 主循环（监控 Worker）
void master_run(Master *master);

// 停止 Master 主循环
void master_stop(Master *master);

// 获取 Worker 信息
const WorkerInfo *master_get_worker_info(Master *master, int worker_id);

// 获取 Worker 数量
int master_get_worker_count(Master *master);

// 检查 Worker 是否需要重启
bool master_worker_needs_restart(Master *master, int worker_id);

// 设置 Worker 重启策略
void master_set_restart_policy(Master *master, int max_restarts, int restart_delay_ms);

// 设置 Worker 入口函数
void master_set_worker_main(Master *master, WorkerMainFunc func);

// 创建 SO_REUSEPORT socket（兼容旧 API）
int create_reuseport_socket(int port, const char *bind_addr, int backlog);
```

---

### Worker

Worker 进程管理。

**头文件**: `include/worker.h`

#### Worker 配置结构

```c
typedef struct WorkerConfig {
    int worker_id;               // Worker 序号
    Server *server;              // Server 指针
    volatile sig_atomic_t running; // 运行标志
} WorkerConfig;
```

#### API

```c
// 创建 Worker 进程上下文
Worker *worker_create(const WorkerConfig *config);

// 销毁 Worker 进程上下文
void worker_destroy(Worker *worker);

// 运行 Worker 主循环
int worker_run(Worker *worker);

// 停止 Worker 主循环
void worker_stop(Worker *worker);

// 获取 Worker ID
int worker_get_id(Worker *worker);

// 获取 Worker 的 Server 指针
Server *worker_get_server(Worker *worker);
```

---

## Phase 4: SSL/TLS + 虚拟主机

### SSL

SSL/TLS 包装模块，支持 OpenSSL 1.1.1 和 3.x。

**头文件**: `include/ssl_wrap.h`

#### SSL 连接状态枚举

```c
typedef enum {
    SSL_STATE_INIT,           // 初始状态
    SSL_STATE_HANDSHAKING,    // SSL 握手进行中
    SSL_STATE_CONNECTED,      // SSL 连接已建立
    SSL_STATE_WANT_READ,      // 需要更多数据
    SSL_STATE_WANT_WRITE,     // 需要写缓冲区可用
    SSL_STATE_CLOSED,         // SSL 连接已关闭
    SSL_STATE_ERROR           // SSL 错误
} SslState;
```

#### SSL 配置结构

```c
typedef struct SslConfig {
    const char *cert_file;    // 证书文件路径
    const char *key_file;     // 私钥文件路径
    const char *ca_file;      // CA 证书文件路径
    bool verify_peer;         // 是否验证客户端证书
    int verify_depth;         // 证书链验证深度
    int session_timeout;      // 会话缓存超时
    bool enable_tickets;      // 是否启用 Session Ticket
} SslConfig;
```

#### API

```c
// 创建 SSL 服务器上下文
SslServerCtx *ssl_server_ctx_create(const SslConfig *config);

// 销毁 SSL 服务器上下文
void ssl_server_ctx_destroy(SslServerCtx *ctx);

// 创建 SSL 连接
SslConnection *ssl_connection_create(SslServerCtx *server_ctx, int fd);

// 销毁 SSL 连接
void ssl_connection_destroy(SslConnection *conn);

// 执行 SSL 握手
SslState ssl_handshake(SslConnection *conn);

// SSL 读数据
int ssl_read(SslConnection *conn, void *buf, size_t len);

// SSL 写数据
int ssl_write(SslConnection *conn, const void *buf, size_t len);

// 获取 SSL 连接状态
SslState ssl_get_state(SslConnection *conn);

// 获取 SSL 错误码
int ssl_get_error(SslConnection *conn);

// 获取 SSL 错误字符串
const char *ssl_get_error_string(int error_code);

// 关闭 SSL 连接
int ssl_shutdown(SslConnection *conn);

// 检查是否启用了 Session Ticket
bool ssl_is_session_ticket_enabled(SslServerCtx *server_ctx);

// 获取客户端证书验证结果
int ssl_get_peer_verify_result(SslConnection *conn);

// 获取客户端证书主题名称
char *ssl_get_peer_cert_subject(SslConnection *conn);
```

---

### VHost

虚拟主机模块。

**头文件**: `include/vhost.h`

#### 虚拟主机结构

```c
typedef struct VirtualHost {
    char *hostname;           // 域名
    Router *router;           // 路由器
    SslServerCtx *ssl_ctx;    // SSL 上下文
    bool is_wildcard;         // 是否为通配符域名
    int priority;             // 匹配优先级
} VirtualHost;
```

#### API

```c
// 创建虚拟主机管理器
VHostManager *vhost_manager_create(void);

// 销毁虚拟主机管理器
void vhost_manager_destroy(VHostManager *manager);

// 创建虚拟主机
VirtualHost *vhost_create(const char *hostname, Router *router, SslServerCtx *ssl_ctx);

// 销毁虚拟主机
void vhost_destroy(VirtualHost *vhost);

// 添加虚拟主机到管理器
int vhost_manager_add(VHostManager *manager, VirtualHost *vhost);

// 根据域名匹配虚拟主机
VirtualHost *vhost_manager_match(VHostManager *manager, const char *hostname);

// 设置默认虚拟主机
int vhost_manager_set_default(VHostManager *manager, VirtualHost *vhost);

// 获取默认虚拟主机
VirtualHost *vhost_manager_get_default(VHostManager *manager);

// 获取虚拟主机数量
int vhost_manager_count(VHostManager *manager);

// 检查域名是否为通配符格式
bool vhost_is_wildcard(const char *hostname);

// 检查域名是否匹配通配符
bool vhost_wildcard_match(const char *wildcard, const char *hostname);
```

---

### Config

HTTP 服务器配置模块。

**头文件**: `include/config.h`

#### Phase 5: 配置版本结构

```c
typedef struct ConfigVersion {
    uint64_t version;       // 版本号
    uint64_t checksum;      // 配置内容 checksum
    uint64_t timestamp;     // 更新时间戳
    char *source_file;      // 来源文件路径
} ConfigVersion;
```

#### Phase 5: 配置更新策略

```c
typedef enum {
    CONFIG_UPDATE_ATOMIC,   // 原子更新
    CONFIG_UPDATE_GRADUAL   // 渐进更新
} ConfigUpdatePolicy;
```

#### HTTP 配置结构

```c
typedef struct HttpConfig {
    // 基础配置
    int port;
    char *bind_address;
    int max_connections;
    int backlog;
    bool reuseport;
    
    // 缓冲区配置
    int read_buffer_capacity;
    int write_buffer_capacity;
    
    // Keep-Alive 配置
    int connection_timeout_ms;
    int keepalive_timeout_ms;
    int max_keepalive_requests;
    
    // SSL/TLS 配置
    bool ssl_enabled;
    SslConfig ssl_config;
    
    // 虚拟主机
    VHostManager *vhost_manager;
    
    // Phase 5: 热更新
    ConfigVersion version;
    ConfigUpdatePolicy update_policy;
    bool hot_update_enabled;
} HttpConfig;
```

#### API

```c
// 创建默认配置
HttpConfig *http_config_create_default(void);

// 从 JSON 文件加载配置
HttpConfig *http_config_load_from_file(const char *file_path, const ConfigLoadOptions *options);

// 销毁配置
void http_config_destroy(HttpConfig *config);

// 验证配置有效性
int http_config_validate(HttpConfig *config);

// 获取配置验证错误字符串
const char *http_config_get_error_string(int error_code);

// 将 HttpConfig 转换为 ServerConfig
ServerConfig http_config_to_server_config(HttpConfig *http_config);

// 合并两个配置
int http_config_merge(HttpConfig *dst, HttpConfig *src);

/* Phase 5: 热更新 API */

// 启用配置热更新
int http_config_enable_hot_update(HttpConfig *config, ConfigUpdatePolicy policy);

// 执行配置热更新
int http_config_hot_update(HttpConfig *config, const char *file_path, ConfigUpdateResult *result);

// 获取配置版本号
uint64_t http_config_get_version(HttpConfig *config);

// 计算配置 checksum
uint64_t http_config_calculate_checksum(HttpConfig *config);

// 检查配置文件是否已变更
bool http_config_has_changed(HttpConfig *config, const char *file_path);

// 配置回滚
int http_config_rollback(HttpConfig *config);
```

---

## Phase 5: 安全防护 + 日志系统

### Security

安全模块，提供 IP 连接限制、速率限制和 Slowloris 检测。

**头文件**: `include/security.h`

#### Security 结果码

```c
typedef enum {
    SECURITY_OK,              // 允许操作
    SECURITY_BLOCKED_IP,      // IP 已被封禁
    SECURITY_RATE_LIMITED,    // 超过速率限制
    SECURITY_TOO_MANY_CONN,   // 连接数过多
    SECURITY_SLOWLORIS,       // Slowloris 检测
    SECURITY_INTERNAL_ERROR   // 内部错误
} SecurityResult;
```

#### Security 配置结构

```c
typedef struct SecurityConfig {
    int max_connections_per_ip;   // 单 IP 最大连接数
    int max_requests_per_second;  // 每秒请求数限制
    int min_request_rate;         // Slowloris 最小字节/秒
    int slowloris_timeout_ms;     // Slowloris 检测超时
    int block_duration_ms;        // IP 封禁持续时间
    int shard_count;              // 分片哈希表分片数
} SecurityConfig;
```

#### 默认配置值

```c
#define SECURITY_DEFAULT_MAX_CONN_PER_IP      10
#define SECURITY_DEFAULT_MAX_REQ_PER_SEC      100
#define SECURITY_DEFAULT_MIN_REQ_RATE         50      // 50 bytes/sec
#define SECURITY_DEFAULT_SLOWLORIS_TIMEOUT    30000   // 30 seconds
#define SECURITY_DEFAULT_BLOCK_DURATION       60000   // 60 seconds
#define SECURITY_DEFAULT_SHARD_COUNT          16
```

#### IP 地址结构

```c
typedef struct IpAddress {
    uint8_t data[16];      // IPv4/IPv6 地址
    bool is_ipv6;          // 是否 IPv6
} IpAddress;
```

#### API

```c
// 创建 Security 模块
Security *security_create(const SecurityConfig *config);

// 销毁 Security 模块
void security_destroy(Security *security);

// 检查连接是否允许
SecurityResult security_check_connection(Security *security, const IpAddress *ip);

// 记录新连接
SecurityResult security_add_connection(Security *security, const IpAddress *ip);

// 移除连接记录
void security_remove_connection(Security *security, const IpAddress *ip);

// 检查请求速率
SecurityResult security_check_request_rate(Security *security, const IpAddress *ip,
                                           size_t bytes_received);

// 封禁 IP
int security_block_ip(Security *security, const IpAddress *ip, int duration_ms);

// 解封 IP
void security_unblock_ip(Security *security, const IpAddress *ip);

// 检查 IP 是否被封禁
bool security_is_blocked(Security *security, const IpAddress *ip);

// 获取 IP 统计信息
int security_get_ip_stats(Security *security, const IpAddress *ip, IpStats *stats);

// 从 sockaddr 解析 IP 地址
int security_parse_ip(const struct sockaddr *addr, IpAddress *ip);

// IP 地址转字符串
int security_ip_to_string(const IpAddress *ip, char *buffer, size_t size);

// 字符串转 IP 地址
int security_string_to_ip(const char *str, IpAddress *ip);

// 清理过期条目
void security_cleanup(Security *security);

// 获取统计摘要
void security_get_summary(Security *security, int *total_tracked, int *total_blocked);
```

---

### Logger

日志模块，支持异步 Ring Buffer 和安全审计。

**头文件**: `include/logger.h`

#### 日志级别枚举

```c
typedef enum {
    LOG_DEBUG,     // 调试信息
    LOG_INFO,      // 一般信息
    LOG_WARN,      // 警告
    LOG_ERROR,     // 错误
    LOG_SECURITY   // 安全事件（审计）
} LogLevel;
```

#### 日志格式枚举

```c
typedef enum {
    LOG_FORMAT_TEXT,   // 文本格式
    LOG_FORMAT_JSON    // JSON 格式
} LogFormat;
```

#### 日志配置结构

```c
typedef struct LoggerConfig {
    const char *log_file;       // 日志文件路径
    const char *audit_file;     // 审计日志文件路径
    LogLevel min_level;         // 最小日志级别
    LogFormat format;           // 日志格式
    int ring_buffer_size;       // Ring Buffer 大小
    int flush_interval_ms;      // 刷新间隔
    bool enable_stdout;         // 同时输出到 stdout
} LoggerConfig;
```

#### 默认配置值

```c
#define LOGGER_DEFAULT_RING_BUFFER_SIZE   (64 * 1024)  // 64KB
#define LOGGER_DEFAULT_FLUSH_INTERVAL     1000         // 1秒
#define LOGGER_DEFAULT_MIN_LEVEL          LOG_INFO
```

#### API

```c
// 创建 Logger
Logger *logger_create(const LoggerConfig *config);

// 销毁 Logger
void logger_destroy(Logger *logger);

// 记录日志
void logger_log(Logger *logger, LogLevel level, const char *format, ...);

// 记录日志（va_list 版本）
void logger_log_v(Logger *logger, LogLevel level, const char *format, va_list args);

// 记录请求（含延迟）
void logger_log_request(Logger *logger, const RequestLogContext *ctx);

// 记录安全审计事件
void logger_log_security(Logger *logger, const SecurityLogContext *ctx);

// 记录路径穿越尝试
void logger_log_path_traversal(Logger *logger, const char *ip,
                               const char *attempted_path,
                               const char *resolved_path);

// 记录速率限制触发
void logger_log_rate_limit(Logger *logger, const char *ip,
                           const char *limit_type,
                           int current_count, int limit_value);

// 设置/获取最小日志级别
void logger_set_level(Logger *logger, LogLevel level);
LogLevel logger_get_level(Logger *logger);

// 强制刷新缓冲区
void logger_flush(Logger *logger);

// 获取日志级别名称
const char *logger_level_name(LogLevel level);

// 格式化时间戳
int logger_format_timestamp(struct timespec *ts, char *buffer, size_t size);

// 获取当前时间戳（毫秒）
uint64_t logger_get_current_ms(void);
```

---

## 附录

### 模块依赖关系

| 模块 | 依赖 |
|------|------|
| EventLoop | 无 |
| Timer | 无 |
| Buffer | 无 |
| Socket | 无 |
| Error | 无 |
| Connection | Buffer |
| ConnectionPool | Connection |
| HttpParser | zlib (Phase 5) |
| Router | HttpParser |
| Handler | HttpParser, Response, FileServe |
| Response | Error |
| Mime | 无 |
| FileServe | Mime, Error |
| Server | EventLoop, Connection, HttpParser, Router, Socket, Response, Handler, Security, Logger |
| Master | Worker, Socket |
| Worker | Server |
| SSL | OpenSSL |
| VHost | Router, SSL |
| Config | Server, SSL, VHost |
| Security | Timer, Error |
| Logger | Timer, Error |

### 文件列表

| 模块 | 头文件 | 源文件 |
|------|--------|--------|
| EventLoop | include/eventloop.h | src/eventloop.c |
| Timer | include/timer.h | src/timer.c |
| Buffer | include/buffer.h | src/buffer.c |
| Socket | include/socket.h | src/socket.c |
| Error | include/error.h | src/error.c |
| Connection | include/connection.h | src/connection.c |
| ConnectionPool | include/connection_pool.h | src/connection_pool.c |
| HttpParser | include/http_parser.h | src/http_parser.c |
| Router | include/router.h | src/router.c |
| Handler | include/handler.h | src/handler.c |
| Response | include/response.h | src/response.c |
| Mime | include/mime.h | src/mime.c |
| FileServe | include/fileserve.h | src/fileserve.c |
| Server | include/server.h | src/server.c |
| Master | include/master.h | src/master.c |
| Worker | include/worker.h | src/worker.c |
| SSL | include/ssl_wrap.h | src/ssl_wrap.c |
| VHost | include/vhost.h | src/vhost.c |
| Config | include/config.h | src/config.c |
| Security | include/security.h | src/security.c |
| Logger | include/logger.h | src/logger.c |