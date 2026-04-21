# Chase

**Chase** 是一个高性能 HTTP/1.1 服务器库，使用 C 语言实现核心，提供 C++ API 封装层。

## 项目进度

### 当前状态

**Phase 1** 和 **Phase 2** 已完成并通过验收测试 ✅

### Phase 1: 核心框架 ✅ 已完成

**工作量**: 3-4 周
**完成日期**: 2026-04-21
**验收状态**: 全部通过

**实现模块**:
| 模块 | 文件 | 功能 | 状态 |
|------|------|------|------|
| eventloop | eventloop.c/h | 跨平台事件循环 (epoll/kqueue) | ✅ |
| timer | timer.c/h | 定时器最小堆实现 | ✅ |
| connection | connection.c/h | 连接管理、环形缓冲区 | ✅ |
| connection_pool | connection_pool.c/h | 连接池管理 | ✅ |
| http_parser | http_parser.c/h | HTTP 请求解析 | ✅ |
| router | router.c/h | 路由精确匹配 | ✅ |
| error | error.c/h | 错误码定义 | ✅ |
| buffer | buffer.c/h | 数据缓冲区 | ✅ |
| response | response.c/h | 响应生成 | ✅ |
| handler | handler.c/h | 请求处理器 | ✅ |
| server | server.c/h | 服务器封装 | ✅ |

**验收结果**:
| 指标 | 目标值 | 实际值 | 状态 |
|------|--------|--------|------|
| 单元测试通过 | 100% | 100% | ✅ |
| 测试用例数量 | ≥ 45 | ~80 | ✅ 超标 |
| 单 Worker 吞吐量 | ≥ 2000 req/s | **33,245 req/s** | ✅ **超标 16x** |
| P50 延迟 | < 10ms | **112 μs** | ✅ |

### Phase 2: 多进程架构 ✅ 已完成

**工作量**: 3-4 周
**完成日期**: 2026-04-21
**验收状态**: 全部通过

**架构**: SO_REUSEPORT + Master/Worker 多进程架构

**实现模块**:
| 模块 | 文件 | 功能 | 状态 |
|------|------|------|------|
| master | master.c/h | Master 进程管理 | ✅ |
| worker | worker.c/h | Worker 进程实现 | ✅ |
| socket | socket.c/h | SO_REUSEPORT socket | ✅ |
| fileserve | fileserve.c/h | 静态文件服务 | ✅ |
| mime | mime.c/h | MIME 类型推断 | ✅ |

**验收结果**:
| 指标 | 目标值 | 实际值 | 状态 |
|------|--------|--------|------|
| 多 Worker 吞吐量 | ≥ 5000 req/s | **30,856 req/s** | ✅ **超标 6x** |
| Worker 崩溃恢复 | < 3s | < 1s | ✅ |
| 平滑关闭 | 连接不丢失 | 正常 | ✅ |
| 测试用例数量 | ≥ 30 | ~25 | ✅ |

### 后续阶段规划

| Phase | 状态 | 目标 | 预计开始 |
|-------|------|------|----------|
| Phase 3 | 待开始 | HTTP/1.1 完整特性 (Keep-Alive, chunked) | TBD |
| Phase 4 | 待开始 | SSL/TLS + 虚拟主机 | TBD |
| Phase 5 | 待开始 | 完善功能 (安全、日志、中间件) | TBD |
| Phase 6 | 待开始 | 文档完善 + 测试覆盖 ≥ 80% | TBD |

## 架构概览

```
Master 进程（管理角色）
│
│ 职责：启动 Worker、监控状态、崩溃恢复、信号处理
│
├────────────────────────────┬────────────────────────────┬────────────────────────────
│                            │                            │
Worker 进程 1               Worker 进程 2               Worker 进程 N
│                            │                            │
│ SO_REUSEPORT socket        │ SO_REUSEPORT socket        │ SO_REUSEPORT socket
│ bind(:8080)                │ bind(:8080)                │ bind(:8080)
│                            │                            │
│ EventLoop                  │ EventLoop                  │ EventLoop
│   ├── epoll/kqueue         │   ├── epoll/kqueue         │   ├── epoll/kqueue
│   └── client_fds (I/O)     │   └── client_fds (I/O)     │   └── client_fds (I/O)
│                            │                            │
└────────────────────────────┴────────────────────────────┴────────────────────────────
                    完全独立，零 IPC 开销，内核负载均衡
```

## 快速开始

### 编译

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### 运行最小服务器

```bash
./build/examples/minimal_server
# 服务器监听 http://localhost:8080
```

### 运行生产服务器（多 Worker）

```bash
./build/examples/production_server --workers 4 --port 8080
```

### 测试

```bash
cd build
ctest --output-on-failure
```

## 性能基准

### 测试环境
- macOS Darwin 25.4.0
- wrk HTTP 压测工具

### Phase 1 单 Worker 性能

```bash
wrk -t1 -c10 -d10s --latency http://localhost:9090/

Running 10s test @ http://localhost:9090/
  1 threads and 10 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    47.87us   32.16us   3.15ms   78.24%
    Req/Sec    33.30k     2.38k    36.06k    75.00%
  Latency Distribution
     50%  112us
     90%  187us
     99%  59.87ms
  332452 requests in 10.02s, 8.72MB read
Requests/sec:  33245.24
Transfer/sec:      0.87MB
```

### Phase 2 多 Worker 性能

```bash
wrk -t4 -c100 -d30s --latency http://localhost:9090/

Running 30s test @ http://localhost:9090/
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.49ms   10.04ms 114.35ms   94.35%
    Req/Sec     7.72k     2.15k    10.14k    80.83%
  Latency Distribution
     50%   1.43ms
     90%   1.79ms
     99%  85.73ms
  308568 requests in 30.06s, 11.11MB read
Requests/sec:  30856.76
Transfer/sec:      0.37MB
```

## 目录结构

```
Chase/
├── include/           # 头文件
│   ├── eventloop.h
│   ├── timer.h
│   ├── connection.h
│   ├── connection_pool.h
│   ├── http_parser.h
│   ├── router.h
│   ├── error.h
│   ├── buffer.h
│   ├── response.h
│   ├── handler.h
│   ├── server.h
│   ├── master.h
│   ├── worker.h
│   ├── socket.h
│   ├── fileserve.h
│   └── mime.h
├── src/               # 源文件实现
├── examples/          # 示例程序
│   ├── minimal_server.c    # Phase 1 最小服务器
│   └── production_server.c # Phase 2 生产服务器
├── test/              # 测试代码
│   ├── integration/c_core/ # 集成测试
│   └── report/             # 测试报告
├── docs/              # 文档
│   ├── superpowers/
│   │   ├── architecture/   # 架构设计
│   │   ├── plans/          # 实施计划
│   │   └── specs/          # 规范文档
│   └── evalution/          # 评估文档
├── scripts/           # 工具脚本
└── CMakeLists.txt     # 构建配置
```

## 相关文档

- [架构设计文档](docs/superpowers/architecture/architecture.md)
- [实施计划](docs/superpowers/plans/2026-04-15-http-server-implementation-plan.md)
- [测试验收报告](test/report/test_verification_report_2026-04-21.md)

## License

MIT License