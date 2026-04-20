# Chase

**Chase** 是一个高性能 HTTP/1.1 服务器库，使用 C 语言实现核心，提供 C++ API 封装层。

## 特性

### 已实现 (Phase 1)

- ✅ **EventLoop** - 支持 epoll (Linux) / kqueue (macOS) / poll (fallback)
- ✅ **Timer** - 最小堆实现，uint64_t ID 防溢出
- ✅ **Connection** - 环形缓冲区，固定/自动扩容模式
- ✅ **HTTP Parser** - 支持 GET/POST/PUT/DELETE/HEAD/OPTIONS/PATCH
- ✅ **Router** - 精确匹配、前缀匹配、优先级排序
- ✅ **Error** - 完整错误码 + HTTP 状态码映射

### 规划中

- 📋 HTTPS (OpenSSL 1.1.1 / 3.x)
- 📋 多线程 + 连接分发
- 📋 虚拟主机 (含通配符域名)
- 📋 静态文件服务 (sendfile 零拷贝)
- 📋 中间件链 (含异步支持)
- 📋 DDoS 防护
- 📋 配置热更新

## 性能

Phase 1 基准测试结果 (单线程):

| 指标 | 目标值 | 实际值 |
|------|--------|--------|
| 吞吐量 | ≥ 2,000 req/s | **32,392 req/s** |
| P50 延迟 | < 10ms | **0.146ms** |
| P99 延迟 | - | 65ms |

测试命令:
```bash
wrk -t1 -c10 -d10s --latency http://localhost:9080/
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

当前测试覆盖: **55 个用例，100% 通过**

| 模块 | 用例数 |
|------|--------|
| eventloop | 10 |
| timer | 10 |
| http_parser | 15 |
| router | 10 |
| connection | 7 |
| error | 3 |

### 代码覆盖率

```bash
# 配置覆盖率构建
cmake -B build -DENABLE_COVERAGE=ON
cmake --build build

# 运行测试
cd build && ctest

# 生成覆盖率报告 (macOS)
llvm-profdata merge -sparse ./test/default.profraw -o coverage.profdata
llvm-cov report -object test/test_* -instr-profile=coverage.profdata

# 生成 HTML 报告
llvm-cov show -object test/test_* -instr-profile=coverage.profdata \
    -format=html -output-dir=coverage_report

# 查看 HTML 报告
open coverage_report/index.html
```

目标覆盖率: ≥ 70%

### 性能基准测试

```bash
# 安装 wrk
brew install wrk  # macOS
# 或参考 https://github.com/wg/wrk  # Linux

# 运行基准测试
./scripts/benchmark.sh
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
├── scripts/           # 工具脚本
│   ├── generate_coverage.sh
│   └── benchmark.sh
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

## 开发状态

当前版本: **v0.1.0** (Phase 1 完成)

| Phase | 状态 | 完成度 |
|-------|------|--------|
| Phase 1: 核心框架 | ✅ 完成 | 100% |
| Phase 2: 多线程 + 静态文件 | 📋 待开始 | 0% |
| Phase 3: HTTP/1.1 完整特性 | 📋 待开始 | 0% |
| Phase 4: SSL/TLS + 虚拟主机 | 📋 待开始 | 0% |
| Phase 5: C++ 封装 + 中间件 + 安全 | 📋 待开始 | 0% |
| Phase 6: 完善与优化 | 📋 待开始 | 0% |

## 许可证

MIT License - 详见 [LICENSE](LICENSE)

## 贡献

欢迎提交 Issue 和 Pull Request。

## 作者

DpAlgoKernEng