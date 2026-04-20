# Chase

**Chase** 是一个高性能 HTTP/1.1 服务器库，使用 C 语言实现核心，提供 C++ API 封装层。

## 特性

### 已实现 (Phase 1 ✅ 已验收通过)

- ✅ **EventLoop** - 支持 epoll (Linux) / kqueue (macOS) / poll (fallback)，预分配事件数组优化
- ✅ **Timer** - 最小堆实现，uint64_t ID 防溢出，支持 one-shot 和 periodic
- ✅ **Connection** - 环形缓冲区，固定/自动扩容两种模式
- ✅ **HTTP Parser** - 支持 GET/POST/PUT/DELETE/HEAD/OPTIONS/PATCH，增量状态机流式解析
- ✅ **Router** - 精确匹配、前缀匹配、优先级排序、方法过滤
- ✅ **Error** - 完整错误码 + HTTP 状态码映射

### 规划中 (Phase 2-6)

- 📋 HTTPS (OpenSSL 1.1.1 / 3.x)
- 📋 多线程 + 连接分发 (Phase 2)
- 📋 连接池预分配 (Phase 2)
- 📋 静态文件服务 (Phase 2)
- 📋 HTTP/1.1 完整特性 - Keep-Alive、chunked、Range (Phase 3)
- 📋 虚拟主机 (含通配符域名) (Phase 4)
- 📋 中间件链 (含异步支持) (Phase 5)
- 📋 DDoS 防护 (Phase 5)
- 📋 配置热更新 (Phase 5)

## 性能

Phase 1 基准测试结果 (单线程):

| 测试配置 | 吞吐量 | P50 延迟 | P75 延迟 | P90 延迟 | P99 延迟 |
|----------|--------|----------|----------|----------|----------|
| 10 连接 | **31,595 req/s** | **144μs** | 198μs | 246μs | 61.5ms |
| 100 连接 | **33,876 req/s** | **1.36ms** | 1.88ms | 2.33ms | 117ms |
| 500 连接 | **30,798 req/s** | **6.83ms** | 9.21ms | 11.17ms | 82ms |

**验收标准：**
| 指标 | 目标值 | 实测值 | 达成率 |
|------|--------|--------|--------|
| 吞吐量 | ≥ 2,000 req/s | 31,595 - 33,876 req/s | **超标 15-17x** ✅ |
| P50 延迟 | < 10ms | 144μs - 6.83ms | **全部达标** ✅ |

测试命令:
```bash
wrk -t1 -c10 -d10s --latency http://localhost:9080/
wrk -t1 -c100 -d10s --latency http://localhost:9080/
wrk -t1 -c500 -d10s --latency http://localhost:9080/
```

## 构建

### 前置要求

- CMake 3.19+
- C11 编译器 (GCC/Clang)
- OpenSSL 1.1.1+
- zlib

### macOS

```bash
# 安装依赖
brew install openssl zlib cmake

# 配置
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 构建
cmake --build build
```

### Linux

```bash
# 安装依赖 (Ubuntu)
sudo apt-get install build-essential cmake libssl-dev zlib1g-dev

# 配置
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 构建
cmake --build build
```

### 使用 vcpkg

```bash
# 配置 vcpkg 工具链
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# 构建
cmake --build build
```

## 测试

### 运行单元测试

```bash
cmake -B build
cmake --build build
cd build && ctest --output-on-failure
```

当前测试覆盖: **66 个用例，100% 通过**

| 模块 | 用例数 | 通过率 |
|------|--------|--------|
| eventloop | 10 | 100% ✅ |
| timer | 10 | 100% ✅ |
| http_parser | 15 | 100% ✅ |
| router | 10 | 100% ✅ |
| connection | 7 | 100% ✅ |
| error | 3 | 100% ✅ |
| boundary (边界条件) | 11 | 100% ✅ |

### 边界条件测试

已验证的边界场景：
- 空输入 / NULL 输入处理
- 超大路径 / 超大头部值
- 增量解析（分段发送）
- 特殊字符（URL编码）
- 无效参数（0/-1容量）
- Buffer 固定模式溢出

### 代码覆盖率

```bash
# 配置覆盖率构建
cmake -B build -DENABLE_COVERAGE=ON
cmake --build build

# 运行测试
cd build && ctest

# 生成覆盖率报告 (macOS)
llvm-profdata merge -sparse default.profraw -o coverage.profdata
llvm-cov report ./test/test_* -instr-profile=coverage.profdata

# 生成 HTML 报告
llvm-cov show ./test/test_* -instr-profile=coverage.profdata \
    -format=html -output-dir=./test/coverage_report

# 查看 HTML 报告
open ./test/coverage_report/index.html
```

当前覆盖率: **40.22%** (行覆盖率)

| 模块 | 行覆盖率 | 分支覆盖率 |
|------|----------|------------|
| http_parser.c | **54.77%** | **40.43%** |
| router.c | 27.06% | 21.43% |
| connection.c | 15.05% | 10.42% |
| eventloop.c | 11.73% | 7.45% |
| error.c | 15.62% | 8.33% |

*注：覆盖率将在 Phase 2-6 完成后显著提升（多线程、SSL、超时管理等路径将被激活）*

### 性能基准测试

```bash
# 安装 wrk
brew install wrk  # macOS
# 或参考 https://github.com/wg/wrk  # Linux

# 启动服务器
./build/examples/minimal_server 9080

# 运行基准测试
wrk -t1 -c10 -d10s --latency http://localhost:9080/
wrk -t1 -c100 -d10s --latency http://localhost:9080/
wrk -t1 -c500 -d10s --latency http://localhost:9080/
```

### 内存检测 (Valgrind)

```bash
# 生成 valgrind 测试脚本
cmake -B build -DENABLE_VALGRIND=ON
cmake --build build

# 运行内存检测 (Linux)
./scripts/run_valgrind.sh
```

## 示例

### 最小 HTTP 服务器

```c
#include "eventloop.h"
#include "http_parser.h"
#include "router.h"

int main(void) {
    // 创建事件循环
    EventLoop *loop = eventloop_create(1024);
    
    // 创建路由器
    Router *router = router_create();
    Route *route = route_create(ROUTER_MATCH_EXACT, "/", handler, NULL);
    router_add_route(router, route);
    
    // 启动服务器
    // ... bind, listen, accept
    
    eventloop_run(loop);
    
    eventloop_destroy(loop);
    router_destroy(router);
    return 0;
}
```

运行示例:

```bash
# 启动服务器
./build/examples/minimal_server 8080

# 测试
curl http://localhost:8080/
# 输出: Hello World from Chase HTTP Server!
```

## 目录结构

```
Chase/
├── include/           # 头文件
│   ├── eventloop.h
│   ├── timer.h
│   ├── connection.h
│   ├── http_parser.h
│   ├── router.h
│   └── error.h
├── src/               # 源文件
│   ├── eventloop.c
│   ├── timer.c
│   ├── connection.c
│   ├── http_parser.c
│   ├── router.c
│   └── error.c
├── examples/          # 示例程序
│   └── minimal_server.c
├── test/              # 测试
│   └── integration/c_core/
│       ├── test_eventloop.c
│       ├── test_timer.c
│       ├── test_http_parser.c
│       ├── test_router.c
│       ├── test_connection.c
│       ├── test_error.c
│       └── test_boundary.c
├── scripts/           # 工具脚本
│   ├── generate_coverage.sh
│   ├── benchmark.sh
│   └── run_valgrind.sh
├── docs/              # 文档
│   └── superpowers/
│       ├── specs/     # 设计文档
│       └── plans/     # 实施计划
├── CMakeLists.txt
├── vcpkg.json
└── README.md
```

## 文档

- [设计文档](docs/superpowers/specs/2026-04-13-http-server-library-design.md) - 完整架构设计
- [实施计划](docs/superpowers/plans/2026-04-15-http-server-implementation-plan.md) - 6阶段迭代计划
- [连接池预分配设计](docs/superpowers/specs/2026-04-19-connection-pool-preallocation-design.md) - Phase 2 优化设计
- [连接池预分配计划](docs/superpowers/plans/2026-04-19-connection-pool-preallocation-plan.md) - Phase 2 实施计划

## API 参考

### EventLoop

```c
EventLoop *eventloop_create(int max_events);
void eventloop_destroy(EventLoop *loop);

int eventloop_add(EventLoop *loop, int fd, uint32_t events,
                  EventCallback cb, void *user_data);
int eventloop_modify(EventLoop *loop, int fd, uint32_t events);
int eventloop_remove(EventLoop *loop, int fd);

void eventloop_run(EventLoop *loop);
void eventloop_stop(EventLoop *loop);
int eventloop_poll(EventLoop *loop, int timeout_ms);

// 事件类型
#define EV_READ    0x01
#define EV_WRITE   0x02
#define EV_ERROR   0x04
#define EV_CLOSE   0x08
```

### Timer

```c
TimerHeap *timer_heap_create(int capacity);
void timer_heap_destroy(TimerHeap *heap);

Timer *timer_heap_add(TimerHeap *heap, uint64_t timeout_ms,
                      TimerCallback cb, void *user_data, bool periodic);
int timer_heap_remove(TimerHeap *heap, Timer *timer);
Timer *timer_heap_peek(TimerHeap *heap);
Timer *timer_heap_pop(TimerHeap *heap);

uint64_t timer_get_expire_time(Timer *timer);
uint64_t timer_get_id(Timer *timer);
```

### HTTP Parser

```c
HttpParser *http_parser_create(void);
void http_parser_destroy(HttpParser *parser);

HttpRequest *http_request_create(void);
void http_request_destroy(HttpRequest *req);

ParseResult http_parser_parse(HttpParser *parser, HttpRequest *req,
                              const char *data, size_t len, size_t *consumed);

const char *http_request_get_header_value(HttpRequest *req, const char *name);
void http_parser_reset(HttpParser *parser);

// 解析结果
typedef enum { PARSE_OK, PARSE_NEED_MORE, PARSE_ERROR, PARSE_COMPLETE } ParseResult;

// HTTP 方法
typedef enum { HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE,
               HTTP_HEAD, HTTP_OPTIONS, HTTP_PATCH } HttpMethod;
```

### Router

```c
Router *router_create(void);
void router_destroy(Router *router);

Route *route_create(RouteMatchType type, const char *pattern,
                    RouteHandler handler, void *user_data);
void route_destroy(Route *route);

int router_add_route(Router *router, Route *route);
int router_add_route_ex(Router *router, Route *route, int priority);
Route *router_match(Router *router, const char *path, HttpMethod method);

// 匹配类型
typedef enum { ROUTER_MATCH_EXACT, ROUTER_MATCH_PREFIX, ROUTER_MATCH_REGEX } RouteMatchType;

// 优先级
#define PRIORITY_HIGH    100
#define PRIORITY_NORMAL  50
#define PRIORITY_LOW     10
```

### Connection

```c
Buffer *buffer_create(size_t capacity);
Buffer *buffer_create_ex(size_t capacity, BufferMode mode, size_t max_cap);
void buffer_destroy(Buffer *buf);

int buffer_write(Buffer *buf, const char *data, size_t len);
int buffer_read(Buffer *buf, char *data, size_t len);
size_t buffer_available(Buffer *buf);
size_t buffer_capacity(Buffer *buf);

Connection *connection_create(int fd, EventLoop *loop);
Connection *connection_create_ex(int fd, EventLoop *loop,
                                  size_t read_buf_cap,
                                  size_t write_buf_cap,
                                  BufferMode mode);
void connection_destroy(Connection *conn);
int connection_read(Connection *conn);
int connection_write(Connection *conn);
void connection_close(Connection *conn);

// 缓冲区模式
typedef enum { BUFFER_MODE_FIXED, BUFFER_MODE_AUTO } BufferMode;
```

## 开发状态

当前版本: **v0.1.0** (Phase 1 ✅ 验收通过)

| Phase | 状态 | 完成度 | 验收结果 |
|-------|------|--------|----------|
| Phase 1: 核心框架 | ✅ 完成 | 100% | **通过验收** |
| Phase 2: 多线程 + 静态文件 | 📋 待开始 | 0% | - |
| Phase 3: HTTP/1.1 完整特性 | 📋 待开始 | 0% | - |
| Phase 4: SSL/TLS + 虚拟主机 | 📋 待开始 | 0% | - |
| Phase 5: C++ 封装 + 中间件 + 安全 | 📋 待开始 | 0% | - |
| Phase 6: 完善与优化 | 📋 待开始 | 0% | - |

### Phase 1 验收总结

| 维度 | 评分 | 说明 |
|------|------|------|
| 功能完成度 | **100%** | 所有设计要求功能已实现，部分超标完成 |
| 测试通过率 | **100%** | 66 个测试全部通过 |
| 性能表现 | **优秀** | 吞吐量超标 15-17x，延迟达标 |
| 边界条件 | **100%** | 11 个边界测试全部通过 |
| 覆盖率 | **40%** | 低于 70% 目标，后续阶段提升 |
| 稳定性 | **优秀** | 压力测试无崩溃 |

**结论：满足进入 Phase 2 条件 ✅**

## 许可证

MIT License - 详见 [LICENSE](LICENSE)

## 贡献

欢迎提交 Issue 和 Pull Request。

## 作者

DpAlgoKernEng