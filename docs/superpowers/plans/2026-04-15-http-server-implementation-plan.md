# HTTP 服务器库实施计划（修订版 v2.0）

**基于设计文档**: `docs/superpowers/specs/2026-04-13-http-server-library-design.md` (v2.0)  
**架构**: SO_REUSEPORT + 多进程架构  
**创建日期**: 2026-04-15（初版） / 2026-04-21（修订版）  
**总工作量**: 15-21 周

**v2.0 修订说明（2026-04-21）**:
- 从多线程架构升级为 SO_REUSEPORT 多进程架构
- 废弃 ThreadPoolManager/WorkerThread，改为 Master/Worker 进程
- 废弃 IPC 通知机制（eventfd/pipe）
- 废弃 Least-Connections 分发策略，信任内核负载均衡
- 新增进程管理（master.c）、信号处理、崩溃恢复
- 保留核心模块（eventloop/timer/http_parser/router/connection）
- 简化 Phase 结构，调整任务顺序

---

## 一、概述

本计划将设计文档中的 6 个阶段分解为具体可执行的任务，每个任务都有明确的依赖关系和量化验收标准。

### 阶段依赖关系

```
Phase 1 ──→ Phase 2 ──→ Phase 3 ──→ Phase 4 ──→ Phase 5 ──→ Phase 6
(核心框架)  (多进程)     (HTTP/1.1)   (SSL)       (完善)       (优化)
```

### 与 v1.0 对比

| Phase | v1.0 内容 | v2.0 内容 | 变化 |
|-------|-----------|-----------|------|
| Phase 1 | 单线程核心框架 | 单 Worker 核心框架 | 类似 |
| Phase 2 | ThreadPoolManager + 静态文件 | Master/Worker 进程 | **完全重写** |
| Phase 3 | HTTP/1.1 特性 | HTTP/1.1 特性 | 不变 |
| Phase 4 | SSL + 虚拟主机 | SSL + 虚拟主机 | Worker 级 SSL_CTX |
| Phase 5 | C++ 封装 + 中间件 + 安全 | 静态文件 + 安全 + 日志 | 调整顺序 |
| Phase 6 | 完善与优化 | 文档 + 测试覆盖 | 不变 |

---

## 二、Phase 量化验收指标

| Phase | 关键指标 | 目标值 | 测试方法 |
|-------|----------|--------|----------|
| **Phase 1** | 单 Worker 吞吐量 | ≥ 2000 req/s | wrk -t1 -c10 -d10s |
| **Phase 1** | 单元测试覆盖率 | ≥ 70% | lcov 报告 |
| **Phase 1** | 内存泄漏 | 0（Valgrind 10min） | valgrind --leak-check=full |
| **Phase 2** | 多 Worker 吞吐量 | ≥ 5000 req/s (4 Workers) | wrk -t4 -c100 -d10s |
| **Phase 2** | Worker 崩溃恢复 | < 3s | kill Worker + 监控重启 |
| **Phase 2** | 平滑关闭 | 连接不丢失 | curl 测试 |
| **Phase 3** | Keep-Alive 连接数 | ≥ 100 并发 | curl 并发测试 |
| **Phase 3** | HTTP/1.1 合规 | 100% 通过 | 合规测试套件 |
| **Phase 4** | HTTPS 握手延迟 | < 50ms | openssl s_client 测试 |
| **Phase 4** | 虚拟主机匹配 | 100%（含通配符） | curl 多域名测试 |
| **Phase 5** | 静态文件吞吐 | ≥ 3000 req/s | wrk 测试 1MB 文件 |
| **Phase 5** | DDoS 防护 | 拒绝超限请求 | 模拟攻击测试 |
| **Phase 6** | 测试覆盖率 | ≥ 80% | lcov 报告 |
| **Phase 6** | 内存泄漏 | 0（24h 压力测试） | valgrind 长跑 |

---

## 三、Phase 1: 核心框架（最小可运行版本）

**工作量**: 3-4 周（含缓冲）
**目标**: 单线程 HTTP 服务器，能响应 GET 请求
**量化验收**: 吞吐量 ≥ 2000 req/s（未优化版本，nginx 单线程可达 5000+ req/s），覆盖率 ≥ 70%

### 1.1 项目基础设施（并行组 A）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 1.1.1 | 创建完整目录结构 + CMakeLists.txt + vcpkg 依赖配置 | P0 | - | - | 待开始 |
| 1.1.2 | 配置 GitHub Actions CI（编译 + 测试） | P0 | [1.1.1] | - | 待开始 |
| 1.1.3 | 配置 clang-format + clang-tidy 检查 | P1 | [1.1.1] | [1.1.2] | 待开始 |
| 1.1.4 | 创建 README.md 文档骨架 | P2 | [1.1.1] | [1.1.2, 1.1.3] | 待开始 |

**验收标准**:
- [ ] `cmake -B build` 成功配置（无错误）
- [ ] `cmake --build build` 成功编译空项目
- [ ] CI pipeline 运行成功（GitHub Actions 绿色）
- [ ] clang-format 检查通过（0 warning）
- [ ] 目录结构与设计文档一致

**失败处理策略**:
- CMake 配置失败 → 检查 vcpkg 环境变量 + 工具链
- CI 失败 → 检查 GitHub Actions yaml 配置

---

### 1.2 eventloop + timer 模块（并行组 B）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 1.2.1 | 实现 eventloop 模块（含 epoll/kqueue/poll 后端） | P0 | [1.1.1] | - | 待开始 |
| 1.2.2 | 实现 timer 模块（含最小堆、uint64_t ID 生成、溢出处理） | P0 | [1.2.1] | - | 待开始 |
| 1.2.3 | 集成 timer 到 eventloop（每次循环检查堆顶） | P0 | [1.2.1, 1.2.2] | - | 待开始 |
| 1.2.4 | 编写 eventloop/timer 单元测试（≥ 15 用例） | P0 | [1.2.1-3] | - | 待开始 |
| 1.2.5 | Valgrind 内存泄漏检测（10min 测试） | P0 | [1.2.4] | - | 待开始 |

**Timer 实现细节补充**:
```c
// Timer ID 类型明确使用 uint64_t（避免溢出）
struct EventLoop {
    TimerHeap timer_heap;
    uint64_t next_timer_id;  // 64位，理论上永不溢出
    ...
};

// Timer 精度测试验收标准：
// - Linux: ±1ms (epoll timeout)
// - macOS: ±1ms (kqueue timeout)
// - 高精度: ±10us-100us (timerfd/kqueue EVFILT_TIMER)
// - 测试方法：100 次定时器触发，统计偏差分布
```

**文件清单**:
```
include/eventloop.h    # API 定义（约 50 行）
include/timer.h        # Timer + TimerHeap API（约 40 行）
src/eventloop.c        # 实现（约 300 行）
src/timer.c            # 最小堆实现（约 200 行）

test/integration/c_core/
├── test_eventloop.c   # 单元测试（≥ 10 用例）
└── test_timer.c       # 单元测试（≥ 5 用例）
```

**验收标准**:
- [ ] Linux 编译通过（epoll 后端）
- [ ] macOS 编译通过（kqueue 后端）
- [ ] 单元测试 ≥ 15 用例全部通过
- [ ] 覆盖率 ≥ 70%（lcov 报告）
- [ ] Valgrind 内存泄漏 = 0
- [ ] Timer 精度测试：±1ms（100 次测试）

**失败处理策略**:
- 单元测试失败 → 定位失败用例，修复对应代码
- 内存泄漏 → 分析泄漏报告，修复 malloc/free 配对
- 编译失败 → 检查平台宏定义（__linux__/__APPLE__）

---

### 1.3 connection + http_parser 模块（并行组 C）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 1.3.1 | 实现 connection 模块（含环形缓冲区、固定容量模式） | P0 | [1.2.1] | - | 待开始 |
| 1.3.2 | 实现 http_parser 模块（GET/POST 基础解析） | P0 | [1.1.1] | [1.3.1] | 待开始 |
| 1.3.3 | 实现 error 模块（错误码定义 + HTTP 状态码映射） | P1 | [1.1.1] | [1.3.1, 1.3.2] | 待开始 |
| 1.3.4 | 编写 connection/http_parser 单元测试（≥ 20 用例） | P0 | [1.3.1-3] | - | 待开始 |
| 1.3.5 | Valgrind 内存泄漏检测 | P0 | [1.3.4] | - | 待开始 |

**文件清单**:
```
include/connection.h   # Connection + Buffer API（约 60 行）
include/http_parser.h  # HTTP 解析 API（约 50 行）
include/error.h        # 错误码定义（约 30 行）
src/connection.c       # 实现（约 400 行）
src/http_parser.c      # 实现（约 500 行）
src/error.c            # 错误处理（约 50 行）

test/integration/c_core/
├── test_connection.c      # 单元测试（≥ 10 用例）
├── test_http_parser.c     # 单元测试（≥ 10 用例）
└── test_error.c           # 单元测试（≥ 3 用例）
```

**验收标准**:
- [ ] 环形缓冲区读写正确（环绕/非环绕两种情况测试）
- [ ] HTTP 解析正确处理 100+ 测试用例（含 malformed 请求）
- [ ] PARSE_COMPLETE / PARSE_NEED_MORE / PARSE_ERROR 状态正确
- [ ] 单元测试 ≥ 20 用例全部通过
- [ ] 覆盖率 ≥ 70%
- [ ] Valgrind 内存泄漏 = 0

**失败处理策略**:
- 环形缓冲区环绕测试失败 → 检查 head/tail 指针计算
- HTTP 解析 malformed 请求失败 → 检查边界条件处理

---

### 1.4 router 模块（并行组 D）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 1.4.1 | 实现 router 模块（精确匹配 + Route 结构） | P0 | [1.1.1] | [1.3.1, 1.3.2] | 待开始 |
| 1.4.2 | 编写 router 单元测试（≥ 10 用例） | P0 | [1.4.1] | - | 待开始 |

**文件清单**:
```
include/router.h       # 路由匹配 API（约 40 行）
src/router.c           # 实现（约 150 行）

test/integration/c_core/
└── test_router.c          # 单元测试（≥ 10 用例）
```

**验收标准**:
- [ ] 能添加精确匹配路由
- [ ] 能匹配 GET/POST/PUT/DELETE 到正确 handler
- [ ] 未匹配返回 NULL
- [ ] 单元测试 ≥ 10 用例全部通过
- [ ] 覆盖率 ≥ 70%

---

### 1.5 最小服务器集成

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 1.5.1 | 集成所有模块到 minimal_server.c（单线程 HTTP 服务器） | P0 | [1.2-1.4] | - | 待开始 |
| 1.5.2 | 编写集成测试（curl 测试 + 性能基准） | P0 | [1.5.1] | - | 待开始 |
| 1.5.3 | 吞吐量基准测试（目标 ≥ 2000 req/s，未优化版本） | P0 | [1.5.2] | - | 待开始 |
| 1.5.4 | Valgrind 内存泄漏检测（10min 测试） | P0 | [1.5.2] | - | 待开始 |

**文件清单**:
```
examples/
└── minimal_server.c       # 最小服务器（约 200 行）

test/integration/c_core/
└── test_minimal_server.c  # 集成测试（≥ 5 用例）
```

**验收标准**:
- [ ] 服务器启动：`./minimal_server` 监听 8080
- [ ] curl `http://localhost:8080/` 返回 "Hello World"（200 OK）
- [ ] curl `http://localhost:8080/api` 调用 handler 返回正确响应
- [ ] curl `http://localhost:8080/notfound` 返回 404
- [ ] 吞吐量 ≥ 2000 req/s（wrk -t1 -c10 -d10s 测试，未优化版本）
- [ ] Valgrind 内存泄漏 = 0（10min 测试）

**失败处理策略**:
- 服务器启动失败 → 检查 socket/bind/listen 错误码
- 吞吐量不达标 → 分析瓶颈（可能是缓冲区大小/解析效率）
- 内存泄漏 → 分析泄漏位置，修复

---

### Phase 1 总验收

**验收检查清单**:
| 检查项 | 目标值 | 测试方法 | 通过标准 |
|--------|--------|----------|----------|
| 编译成功 | Linux + macOS | CMake build | 0 error |
| 单元测试 | ≥ 45 用例 | ctest | 100% 通过 |
| 覆盖率 | ≥ 70% | lcov | 报告确认 |
| 吞吐量 | ≥ 2000 req/s（未优化） | wrk -t1 -c10 -d10s | 报告确认 |
| 内存泄漏 | 0 | Valgrind 30min | 0 leaked |
| CI 通过 | GitHub Actions | PR check | 绿色 |

**Phase 1 失败处理**:
- 单元测试失败 → 回退到对应模块修复，重新测试
- 吞吐量不达标 → 分析瓶颈，Phase 2 增加优化任务
- 内存泄漏 → 定位泄漏模块，修复后重新验收
- CI 失败 → 检查 CI 配置，修复后重新提交

---

## 四、Phase 2: 多进程架构

**工作量**: 3-4 周  
**依赖**: Phase 1 完成  
**目标**: Master + Worker 多进程架构  
**量化验收**: 吞吐量 ≥ 5000 req/s，Worker 崩溃恢复 < 3s

### 2.1 master 模块（并行组 A）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 2.1.1 | 实现 master.h + master.c（进程管理） | P0 | [Phase 1] | - | 待开始 |
| 2.1.2 | 实现启动 Worker（fork） | P0 | [2.1.1] | - | 待开始 |
| 2.1.3 | 实现监控 Worker（waitpid） | P0 | [2.1.1, 2.1.2] | - | 待开始 |
| 2.1.4 | 实现 Worker 崩溃恢复 | P0 | [2.1.3] | - | 待开始 |
| 2.1.5 | 实现信号处理（SIGINT/SIGTERM/SIGCHLD） | P0 | [2.1.1] | - | 待开始 |
| 2.1.6 | 编写 master 单元测试（≥ 10 用例） | P0 | [2.1.1-5] | - | 待开始 |

**文件清单**:
```
include/master.h
src/master.c
test/unit/test_master.c
```

**验收标准**:
- [ ] N 个 Worker 并行启动
- [ ] Worker 崩溃自动重启（< 3s）
- [ ] SIGTERM 平滑关闭
- [ ] 单元测试 ≥ 10 用例通过

---

### 2.2 worker 模块扩展（SO_REUSEPORT）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 2.2.1 | 实现 SO_REUSEPORT socket 创建 | P0 | [Phase 1] | [2.1.1] | 待开始 |
| 2.2.2 | 实现多 Worker 并发 accept | P0 | [2.2.1] | - | 待开始 |
| 2.2.3 | 实现 Worker 信号处理（SIGTERM） | P0 | [2.2.1] | - | 待开始 |
| 2.2.4 | 实现 Worker 优雅关闭 | P0 | [2.2.3] | - | 待开始 |
| 2.2.5 | 编写 worker 单元测试 | P0 | [2.2.1-4] | - | 待开始 |

**文件清单**:
```
src/worker.c（扩展）
test/unit/test_worker.c
```

**验收标准**:
- [ ] SO_REUSEPORT socket 正常创建
- [ ] 多 Worker 并发 accept 正常
- [ ] Worker 优雅关闭不丢连接
- [ ] 单元测试 ≥ 10 用例通过

---

### 2.3 进程管理集成测试

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 2.3.1 | 创建 examples/production_server.c | P0 | [2.1, 2.2] | - | 待开始 |
| 2.3.2 | 编写进程管理集成测试 | P0 | [2.3.1] | - | 待开始 |
| 2.3.3 | Worker 崩溃恢复测试 | P0 | [2.3.2] | - | 待开始 |
| 2.3.4 | 平滑关闭测试 | P0 | [2.3.2] | - | 待开始 |
| 2.3.5 | 多 Worker 吞吐量基准 | P0 | [2.3.1] | - | 待开始 |

**文件清单**:
```
examples/production_server.c
test/integration/
├── test_process_mgmt.c
├── test_worker_crash.c
└── test_signal_handling.c
```

**验收标准**:
- [ ] 4 Workers 吞吐量 ≥ 5000 req/s
- [ ] Worker 崩溃恢复 < 3s
- [ ] 平滑关闭连接不丢失

---

### Phase 2 总验收

**验收检查清单**:
| 检查项 | 目标值 | 测试方法 | 通过标准 |
|--------|--------|----------|----------|
| 多 Worker 吞吐量 | ≥ 5000 req/s (4 Workers) | wrk -t4 -c100 -d10s | 报告确认 |
| Worker 崩溃恢复 | < 3s | kill + 监控 | 测量 |
| 平滑关闭 | 连接不丢失 | curl 测试 | 无错误 |
| 单元测试 | ≥ 30 用例 | ctest | 100% 通过 |
| 内存泄漏 | 0 | Valgrind 30min | 0 leaked |

**Phase 2 失败处理**:
- 多 Worker 吞吐量不达标 → 检查 Worker 数量（建议与 CPU 核数匹配），验证 SO_REUSEPORT 是否生效
- Worker 崩溃恢复慢 → 检查 waitpid 循环频率，优化 fork 速度
- 平滑关闭丢连接 → 检查 Worker 关闭流程，确保现有连接处理完成
- SO_REUSEPORT 不工作 → 验证内核版本（Linux 3.9+），检查 socket 选项设置
- 内存泄漏 → 定位泄漏位置，检查 fork 后资源清理

---

## 五、Phase 3: HTTP/1.1 完整特性

**工作量**: 2-3 周  
**依赖**: Phase 1 (timer), Phase 2  
**目标**: HTTP/1.1 合规  
**量化验收**: Keep-Alive ≥ 100 并发

### 3.1 Keep-Alive 实现

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 3.1.1 | 实现 connection_timeout 定时器 | P0 | [Phase 2] | - | 待开始 |
| 3.1.2 | 实现 keepalive_timeout 定时器 | P0 | [Phase 2] | [3.1.1] | 待开始 |
| 3.1.3 | 实现 Connection 持久化状态管理 | P0 | [3.1.1, 3.1.2] | - | 待开始 |
| 3.1.4 | 编写 Keep-Alive 单元测试 | P0 | [3.1.1-3] | - | 待开始 |

**验收标准**:
- [ ] Connection timeout 正确触发
- [ ] Keepalive timeout 正确触发
- [ ] Keep-Alive 连接数 ≥ 100 并发

---

### 3.2 chunked 编码

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 3.2.1 | 实现 chunked 请求解析 | P0 | [Phase 2] | - | 待开始 |
| 3.2.2 | 实现 chunked 响应生成 | P0 | [3.2.1] | - | 待开始 |
| 3.2.3 | 编写 chunked 单元测试 | P0 | [3.2.1-2] | - | 待开始 |

---

### 3.3 HTTP/1.1 合规测试

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 3.3.1 | 实现 Host 头必需验证 | P0 | [Phase 2] | - | 待开始 |
| 3.3.2 | 实现 Range 请求支持 | P1 | [Phase 2] | - | 待开始 |
| 3.3.3 | 编写 HTTP/1.1 合规测试套件 | P0 | [3.1-3.3] | - | 待开始 |

---

### Phase 3 总验收

| 检查项 | 目标值 | 测试方法 |
|--------|--------|----------|
| Keep-Alive 并发 | ≥ 100 | curl 并发测试 |
| HTTP/1.1 合规 | 100% | 合规测试套件 |
| 单元测试 | ≥ 35 用例 | ctest |
| 覆盖率 | ≥ 70% | lcov |

---

## 六、Phase 4: SSL/TLS + 虚拟主机

**工作量**: 3-4 周  
**依赖**: Phase 2  
**目标**: HTTPS + 多域名  
**量化验收**: HTTPS 握手延迟 < 50ms
**目标**: HTTPS + 多域名
**量化验收**: HTTPS 握手延迟 < 50ms，虚拟主机匹配 100%

### 4.1 ssl_wrap 模块（并行组 A）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 4.1.1 | 实现 ssl_wrap 模块（含 OpenSSL 1.1.1/3.x 兼容层） | P0 | [Phase 2] | - | 待开始 |
| 4.1.2 | 实现 SSL_accept 流程 + SSL_HANDSHAKING 状态 | P0 | [4.1.1] | - | 待开始 |
| 4.1.3 | 实现 SSL_read/write（含 WANT_READ/WANT_WRITE 处理） | P0 | [4.1.2] | - | 待开始 |
| 4.1.4 | 实现会话缓存 + TLS 1.3 Session Ticket | P1 | [4.1.3] | - | 待开始 |
| 4.1.5 | 编写 SSL 单元测试（含 1.1.1 和 3.x 版本测试） | P0 | [4.1.1-4] | - | 待开始 |
| 4.1.6 | HTTPS 握手延迟测试（目标 < 50ms） | P0 | [4.1.5] | - | 待开始 |

**验收标准**:
- [ ] OpenSSL 1.1.1 和 3.x 都编译通过
- [ ] SSL 握手成功（含证书验证）
- [ ] HTTPS 请求正确响应
- [ ] SSL 握手延迟 < 50ms（openssl s_client 测试）
- [ ] 会话复用正确工作
- [ ] 单元测试 ≥ 15 用例全部通过

**失败处理策略**:
- OpenSSL 版本编译失败 → 检查兼容层宏定义
- SSL 握手延迟过大 → 检查会话缓存配置

---

### 4.2 vhost 模块（并行组 B）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 4.2.1 | 实现 vhost 模块（含 VirtualHost + VHostManager） | P0 | [Phase 2] | [4.1.1] | 待开始 |
| 4.2.2 | 实现精确域名匹配 | P0 | [4.2.1] | - | 待开始 |
| 4.2.3 | 实现通配符域名匹配（`*.example.com`） | P0 | [4.2.2] | - | 待开始 |
| 4.2.4 | 编写 vhost 单元测试（含通配符测试） | P0 | [4.2.1-3] | - | 待开始 |
| 4.2.5 | 多域名虚拟主机测试（含通配符） | P0 | [4.2.4] | - | 待开始 |

**验收标准**:
- [ ] 精确域名匹配正确
- [ ] 通配符域名匹配正确（仅一级子域名）
- [ ] 默认虚拟主机正确返回
- [ ] 单元测试 ≥ 10 用例全部通过
- [ ] 多域名测试 100% 匹配正确

---

### 4.3 config 模块（基础）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 4.3.1 | 实现 config 模块（含 HttpConfig 结构） | P0 | [Phase 2] | [4.1.1, 4.2.1] | 待开始 |
| 4.3.2 | 实现 JSON 配置加载 | P0 | [4.3.1] | - | 待开始 |
| 4.3.3 | 实现命令行参数解析 | P1 | [4.3.1] | [4.3.2] | 待开始 |
| 4.3.4 | 编写 config 单元测试 | P0 | [4.3.1-3] | - | 待开始 |

**验收标准**:
- [ ] JSON 配置正确加载
- [ ] 命令行参数正确覆盖配置
- [ ] 配置验证正确（检查必填字段）
- [ ] 单元测试 ≥ 10 用例全部通过

---

### Phase 4 总验收

**验收检查清单**:
| 检查项 | 目标值 | 测试方法 | 通过标准 |
|--------|--------|----------|----------|
| HTTPS 握手延迟 | < 50ms | openssl s_client time | 测量 |
| 虚拟主机匹配 | 100% | curl 多域名测试 | 全部正确 |
| OpenSSL 版本 | 1.1.1 + 3.x | 双版本编译测试 | 全部通过 |
| 单元测试 | ≥ 35 用例 | ctest | 100% 通过 |
| 覆盖率 | ≥ 70% | lcov | 报告确认 |
| 内存泄漏 | 0 | Valgrind 30min | 0 leaked |

**Phase 4 失败处理**:
- OpenSSL 版本编译失败 → 检查 ssl_wrap.c 兼容层宏定义，确认 OPENSSL_VERSION_NUMBER 检测正确
- SSL 握手延迟过大 → 检查 SSL_CTX_set_session_cache_mode 配置，启用 Session Ticket
- 虚拟主机通配符匹配错误 → 验证 vhost_is_wildcard_match suffix 检查逻辑，确保一级子域名限制
- 证书加载失败 → 检查文件路径和权限，确认 SSL_CTX_use_certificate_file 返回值
- Session Ticket 复用失败 → 检查 SSL_CTX_set_tlsext_ticket_keys 配置，确认密钥生成正确
- JSON 配置加载失败 → 检查 JSON 语法错误（使用 jsonlint），确认必填字段存在，验证字段类型匹配
- 命令行参数解析错误 → 检查 CLI11/gflags 参数定义，确认类型匹配，验证默认值合理性
- 配置验证失败 → 检查 http_config_validate 返回值，补充缺失必填字段，确认 port/bind_address 范围合法
- 虚拟主机默认匹配错误 → 检查 vhost_manager_match 优先级顺序（精确→通配符→默认），确认第一个 vhost 作为默认
- 多域名 SNI 匹配失败 → 检查 SSL_CTX_set_tlsext_servername_callback 配置，确认 ServerName 提取正确
- 内存泄漏 → 定位泄漏位置（Valgrind --leak-check=full），检查 SSL 连接关闭路径和证书资源释放

---

### OpenSSL 双版本 CI 测试配置方案

**问题**: OpenSSL 1.1.1 和 3.x API 有差异，需要双版本测试验证兼容性。

#### 1. vcpkg 多版本管理策略

**方案 A: 使用不同 vcpkg triplet（推荐）**

```bash
# 创建两个 triplet 文件
# vcpkg/triplets/x64-linux-openssl111.cmake
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_OVERLAY_PORTS openssl111)  # 指向 OpenSSL 1.1.1 port

# vcpkg/triplets/x64-linux-openssl3x.cmake
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_OVERLAY_PORTS openssl3x)    # 指向 OpenSSL 3.x port
```

**vcpkg.json 配置（单一依赖，多 triplet 构建）**:
```json
{
  "name": "chase-http-server",
  "version": "0.1.0",
  "dependencies": [
    {
      "name": "openssl",
      "version>=": "1.1.1"
    }
  ]
}
```

**构建命令**:
```bash
# OpenSSL 1.1.1 构建
cmake -B build-ssl111 -S . \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux-openssl111

# OpenSSL 3.x 构建
cmake -B build-ssl3x -S . \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux-openssl3x
```

**方案 B: 使用 overlay ports（灵活）**

```bash
# 创建 overlay 目录
mkdir -p vcpkg-overlay/openssl-versions

# 复制 vcpkg openssl port 并修改版本
cp -r $VCPKG_ROOT/ports/openssl vcpkg-overlay/openssl-versions/openssl111
cp -r $VCPKG_ROOT/ports/openssl vcpkg-overlay/openssl-versions/openssl3x

# 修改 openssl111/portfile.cmake
set(OPENSSL_VERSION 1.1.1w)
set(OPENSSL_HASH ...)

# 修改 openssl3x/portfile.cmake
set(OPENSSL_VERSION 3.0.13)
set(OPENSSL_HASH ...)
```

#### 2. GitHub Actions 矩阵测试配置

```yaml
# .github/workflows/openssl-compat.yml
name: OpenSSL Compatibility Test

on:
  push:
    branches: [main, develop]
  pull_request:
    branches: [main]

jobs:
  openssl-compat:
    runs-on: ubuntu-latest
    strategy:
      matrix:
        openssl-version: ['1.1.1', '3.0']
        include:
          - openssl-version: '1.1.1'
            triplet: x64-linux-openssl111
            build-dir: build-ssl111
          - openssl-version: '3.0'
            triplet: x64-linux-openssl3x
            build-dir: build-ssl3x

    steps:
    - uses: actions/checkout@v3

    - name: Setup vcpkg
      uses: lukka/run-vcpkg@v11
      with:
        vcpkgGitCommitId: '2024.01.12'
        vcpkgJsonGlob: 'vcpkg.json'

    - name: Configure with OpenSSL ${{ matrix.openssl-version }}
      run: |
        cmake -B ${{ matrix.build-dir }} -S . \
          -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
          -DVCPKG_TARGET_TRIPLET=${{ matrix.triplet }} \
          -DCMAKE_BUILD_TYPE=Release

    - name: Build
      run: cmake --build ${{ matrix.build-dir }}

    - name: Run SSL tests
      run: |
        cd ${{ matrix.build-dir }}
        ctest -L ssl --output-on-failure

    - name: Verify OpenSSL version
      run: |
        cd ${{ matrix.build-dir }}
        ldd examples/minimal_server | grep ssl
        # 输出: libssl.so.1.1 (1.1.1) 或 libssl.so.3 (3.x)
```

#### 3. 本地双版本测试脚本

```bash
#!/bin/bash
# scripts/test_openssl_compat.sh

echo "=== OpenSSL 双版本兼容性测试 ==="

# 测试 OpenSSL 1.1.1
echo ">>> 测试 OpenSSL 1.1.1"
cmake -B build-ssl111 -S . \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux-openssl111 \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build-ssl111
cd build-ssl111 && ctest -L ssl --output-on-failure
SSL111_RESULT=$?

# 清理
rm -rf build-ssl111

# 测试 OpenSSL 3.x
echo ">>> 测试 OpenSSL 3.x"
cmake -B build-ssl3x -S . \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux-openssl3x \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build-ssl3x
cd build-ssl3x && ctest -L ssl --output-on-failure
SSL3X_RESULT=$?

# 清理
rm -rf build-ssl3x

# 汇总结果
echo "=== 测试结果汇总 ==="
if [ $SSL111_RESULT -eq 0 ]; then
  echo "✓ OpenSSL 1.1.1 测试通过"
else
  echo "✗ OpenSSL 1.1.1 测试失败"
fi

if [ $SSL3X_RESULT -eq 0 ]; then
  echo "✓ OpenSSL 3.x 测试通过"
else
  echo "✗ OpenSSL 3.x 测试失败"
fi

# 全部通过才算成功
if [ $SSL111_RESULT -eq 0 ] && [ $SSL3X_RESULT -eq 0 ]; then
  echo "=== 全部 OpenSSL 版本测试通过 ==="
  exit 0
else
  echo "=== OpenSSL 兼容性测试失败 ==="
  exit 1
fi
```

#### 4. CMakeLists.txt OpenSSL 版本检测

```cmake
# CMakeLists.txt

find_package(OpenSSL REQUIRED)

# 输出 OpenSSL 版本信息
message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")
message(STATUS "OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "OpenSSL libraries: ${OPENSSL_LIBRARIES}")

# 版本兼容性检查
if(OPENSSL_VERSION VERSION_LESS "1.1.1")
  message(FATAL_ERROR "OpenSSL version too old, need 1.1.1+")
endif()

# 根据版本设置编译选项
if(OPENSSL_VERSION VERSION_GREATER_EQUAL "3.0.0")
  message(STATUS "Using OpenSSL 3.x compatibility macros")
  target_compile_definitions(http_server PRIVATE OPENSSL_3X_COMPAT)
else()
  message(STATUS "Using OpenSSL 1.1.1 compatibility macros")
  target_compile_definitions(http_server PRIVATE OPENSSL_111_COMPAT)
endif()

# 链接 OpenSSL
target_link_libraries(http_server PRIVATE OpenSSL::SSL OpenSSL::Crypto)
```

#### 5. 测试标签配置

```cmake
# test/integration/c_core/CMakeLists.txt

# SSL 相关测试添加标签
add_test(NAME test_ssl_handshake
         COMMAND test_ssl handshake)
set_tests_properties(test_ssl_handshake PROPERTIES LABELS "ssl")

add_test(NAME test_ssl_session_cache
         COMMAND test_ssl session_cache)
set_tests_properties(test_ssl_session_cache PROPERTIES LABELS "ssl")

add_test(NAME test_ssl_tls13_ticket
         COMMAND test_ssl tls13_ticket)
set_tests_properties(test_ssl_tls13_ticket PROPERTIES LABELS "ssl")

# 运行 SSL 测试：ctest -L ssl
```

#### 6. CI 时间优化（并行矩阵）

```yaml
# 使用 matrix 并行测试，避免串行等待
jobs:
  openssl-111:
    runs-on: ubuntu-latest
    steps: [配置、构建、测试 OpenSSL 1.1.1]
  
  openssl-3x:
    runs-on: ubuntu-latest
    steps: [配置、构建、测试 OpenSSL 3.x]
  
  report:
    needs: [openssl-111, openssl-3x]
    runs-on: ubuntu-latest
    steps:
      - name: Check all tests passed
        run: echo "All OpenSSL versions tested successfully"
```

**预计 CI 时间**: 每个 job 约 5-10 分钟，并行运行总时间约 10 分钟。

#### 7. 本地开发环境配置

**推荐**: 开发时使用 OpenSSL 3.x（最新版本），CI 测试双版本。

```bash
# macOS (Homebrew)
brew install openssl@3

# Linux (vcpkg)
vcpkg install openssl:x64-linux

# 配置默认 triplet
export VCPKG_DEFAULT_TRIPLET=x64-linux-openssl3x
```

**注意**: OpenSSL 1.1.1 将于 2023-09-11 停止支持，但企业环境可能仍在使用。

---

## 七、Phase 5: 完善功能

**工作量**: 3-4 周  
**依赖**: Phase 4  
**目标**: 静态文件 + 安全 + 日志  
**量化验收**: 静态文件吞吐 ≥ 3000 req/s

### 5.1 fileserve 模块

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 5.1.1 | 实现 fileserve 模块（路径穿越检测） | P0 | [Phase 4] | - | 待开始 |
| 5.1.2 | 实现 MIME 类型推断 | P0 | [5.1.1] | - | 待开始 |
| 5.1.3 | 实现跨平台 sendfile | P0 | [5.1.1] | - | 待开始 |
| 5.1.4 | 实现大文件分段传输 | P1 | [5.1.3] | - | 待开始 |
| 5.1.5 | 编写 fileserve 单元测试 | P0 | [5.1.1-4] | - | 待开始 |

**验收标准**:
- [ ] 静态文件正确返回
- [ ] 路径穿越防护生效
- [ ] sendfile 零拷贝生效
- [ ] 大文件分段正常

---

### 5.2 security 模块（Worker 级实例）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 5.2.1 | 实现 security 模块（Worker 级实例） | P0 | [Phase 4] | [5.1.1] | 待开始 |
| 5.2.2 | 实现单 IP 连接限制 | P0 | [5.2.1] | - | 待开始 |
| 5.2.3 | 实现速率限制 | P0 | [5.2.1] | [5.2.2] | 待开始 |
| 5.2.4 | 实现 Slowloris 检测 | P0 | [5.2.1] | [5.2.3] | 待开始 |
| 5.2.5 | 编写 security 单元测试 | P0 | [5.2.1-4] | - | 待开始 |

**验收标准**:
- [ ] Worker 级独立实例
- [ ] 单 IP 连接限制生效
- [ ] 速率限制生效
- [ ] Slowloris 检测生效

---

### 5.3 logger 模块

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 5.3.1 | 实现 logger 模块（Worker 独立日志） | P0 | [Phase 4] | [5.1.1, 5.2.1] | 待开始 |
| 5.3.2 | 实现日志级别过滤 | P0 | [5.3.1] | - | 待开始 |
| 5.3.3 | 实现安全审计日志 | P0 | [5.3.1] | [5.3.2] | 待开始 |
| 5.3.4 | 编写 logger 单元测试 | P0 | [5.3.1-3] | - | 待开始 |

**验收标准**:
- [ ] Worker 独立日志文件
- [ ] 日志级别过滤正确
- [ ] 安全审计日志完整

---

### 5.4 router 扩展

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 5.4.1 | 实现前缀匹配 | P0 | [Phase 4] | - | 待开始 |
| 5.4.2 | 实现正则匹配 | P1 | [5.4.1] | - | 待开始 |
| 5.4.3 | 实现优先级排序 | P1 | [5.4.1, 5.4.2] | - | 待开始 |

---

### Phase 5 总验收

| 检查项 | 目标值 | 测试方法 |
|--------|--------|----------|
| 静态文件吞吐 | ≥ 3000 req/s | wrk 测试 1MB 文件 |
| DDoS 防护 | 生效 | 模拟攻击测试 |
| 路径穿越防护 | 100% 拒绝 | 20+ 攻击模式测试 |
| 单元测试 | ≥ 50 用例 | ctest |
| 覆盖率 | ≥ 70% | lcov |

// 适用场景说明：
// - 默认配置：16 分片，每分片 65536 entries，总计 1M IP
// - 高流量场景：可增加分片数到 32 或 max_entries_per_shard 到 131072
// - 超大规模（> 1M IP）：建议使用外部限流服务（如 Redis）

// 配置参数（可配置）
typedef struct SecurityConfig {
    int max_tracked_ips;           // 最大跟踪 IP 数（默认 1M）
    int hash_table_shards;         // 分片数（默认 16）
    int max_connections_per_ip;    // 单 IP 连接限制（默认 50）
    int connection_rate_per_ip;    // 单 IP 连接速率（默认 20/s）
    int global_rate_limit;         // 全局请求速率限制（可选）
    
    // 分布式限流配置
    int enable_distributed_limit;  // 跨 Worker 限流（默认关闭）
    int local_counter_threshold;   // 本地计数阈值（默认 100）
    int sync_interval_ms;          // 同步间隔（默认 100ms）
} SecurityConfig;

// 初始化示例：
SecurityConfig* cfg = security_config_create();
cfg->hash_table_shards = 16;          // 明确分片数
cfg->max_tracked_ips = 1000000;       // 1M IP
cfg->max_connections_per_ip = 50;     // 单 IP 限制
cfg->connection_rate_per_ip = 20;     // 连接速率
```

---

### 5.4 logger 模块（并行组 D）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 5.4.1 | 实现 logger 模块（含 Logger + AsyncLogger） | P0 | [Phase 4] | [5.1.1, 5.2.1, 5.3.1] | 待开始 |
| 5.4.2 | 实现无锁 Ring Buffer（参考 DPDK） | P0 | [5.4.1] | - | 待开始 |
| 5.4.3 | 实现安全审计日志 | P0 | [5.4.1] | [5.4.2] | 待开始 |
| 5.4.4 | 编写 logger 单元测试（含异步性能测试） | P0 | [5.4.1-3] | - | 待开始 |

**验收标准**:
- [ ] AsyncLogger 异步写入正确（无阻塞）
- [ ] Ring Buffer 无锁写入正确（SPSC）
- [ ] 安全审计日志独立记录
- [ ] 单元测试 ≥ 10 用例全部通过

---

### 5.5 http_parser 扩展（Zip Bomb + gzip）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 5.5.1 | 实现 gzip 解压（使用 zlib） | P0 | [1.3.2] | [5.1.1] | 待开始 |
| 5.5.2 | 实现 Zip Bomb 检测（压缩比 + 大小限制） | P0 | [5.5.1] | - | 待开始 |
| 5.5.3 | 编写 Zip Bomb 测试（含模拟攻击） | P0 | [5.5.1-2] | - | 待开始 |

**验收标准**:
- [ ] gzip 解压正确工作
- [ ] Zip Bomb 检测生效（拒绝高压缩比）
- [ ] 解压大小限制生效（默认 100MB）

---

### 5.6 router 扩展（正则 + 冲突检测）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 5.6.1 | 实现 ROUTER_MATCH_REGEX | P0 | [2.3.1] | - | 待开始 |
| 5.6.2 | 实现优先级排序 | P1 | [5.6.1] | - | 待开始 |
| 5.6.3 | 实现冲突检测（WARN/ERROR/OVERRIDE） | P1 | [5.6.1] | [5.6.2] | 待开始 |
| 5.6.4 | 编写 router 扩展测试 | P0 | [5.6.1-3] | - | 待开始 |

**验收标准**:
- [ ] 正则匹配正确工作
- [ ] 优先级排序正确
- [ ] 冲突检测正确警告

---

### 5.7 config 扩展（热更新）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 5.7.1 | 实现 Atomic 热更新策略 | P0 | [4.3.1] | - | 待开始 |
| 5.7.2 | 实现 Gradual 热更新策略 | P1 | [5.7.1] | - | 待开始 |
| 5.7.3 | 实现版本检查 + 动态延迟计算 | P0 | [5.7.1-2] | - | 待开始 |
| 5.7.4 | 编写热更新测试 | P0 | [5.7.1-3] | - | 待开始 |

**验收标准**:
- [ ] Atomic 热更新正确工作
- [ ] Gradual 热更新正确工作
- [ ] 动态延迟计算正确

---

### Phase 5 总验收

**验收检查清单**:
| 检查项 | 目标值 | 测试方法 | 通过标准 |
|--------|--------|----------|----------|
| 异步中间件延迟 | < 5ms（不含异步操作本身） | 响应时间测量 + 异步模拟器 | 测量 |
| DDoS 防护 | 生效 | 模拟攻击测试 | 拒绝超限 |
| Zip Bomb 防护 | 生效 | 模拟攻击测试 | 拒绝 |
| 单元测试 | ≥ 80 用例 | ctest | 100% 通过 |
| 覆盖率 | ≥ 70% | lcov | 报告确认 |
| C++ API 易用性 | 用户测试 | demo 程序 | 反馈正面 |

**Phase 5 失败处理**:
- C++ API 易用性不佳 → 收集用户反馈，迭代改进 API 设计，参考 Express.js/Koa 设计模式
- 异步中间件唤醒机制失效 → 检查 eventfd/pipe 通知机制，确认 worker_notify_async_done 正确调用
- DDoS 防护失效 → 检查分片锁哈希表容量，调整 max_tracked_ips，使用 ThreadSanitizer 测试竞态
- 速率限制不准确 → 验证滑动窗口算法边界条件，检查时间窗口计算
- Zip Bomb 检测漏过 → 检查压缩比阈值（默认 100:1），确认 zlib 输出限制生效
- 分布式限流竞态 → 使用 ThreadSanitizer 检测，优化本地计数 + 定期同步策略
- 热更新配置不生效 → 检查 config_version 传递，确认 Worker 通知机制
- 内存泄漏 → 定位泄漏位置（Valgrind --leak-check=full），检查异步中间件资源释放和 security 模块哈希表清理

---

### 异步中间件延迟测试场景定义（简化版）

**测试目标**: 验证异步中间件唤醒机制延迟 < 5ms（不含异步操作本身耗时）

#### 核心测试场景（2 个）

| 场景 | 异步操作类型 | 模拟延迟 | 测试目的 | 优先级 |
|------|-------------|----------|----------|--------|
| **场景 A（验收必测）** | 空异步操作 | 0ms | 测试纯唤醒机制延迟（核心指标） | P0 |
| **场景 B（典型场景）** | 模拟数据库查询 | 10-50ms | 验证唤醒机制不受数据库延迟影响 | P0 |

**可选测试场景（3 个）**:

| 场景 | 异步操作类型 | 模拟延迟 | 测试目的 | 优先级 |
|------|-------------|----------|----------|--------|
| 场景 C | 模拟 HTTP 外部请求 | 100-500ms | 验证唤醒机制不受 HTTP 延迟影响 | P1 |
| 场景 D | 模拟文件 I/O | 5-20ms | 验证唤醒机制不受 I/O 延迟影响 | P1 |
| 场景 E | 模拟计算任务 | 1-10ms | 验证唤醒机制不受计算延迟影响 | P1 |

#### 异步延迟模拟器实现（简化版）

```cpp
// test/integration/cpp_api/async_delay_simulator.hpp

class AsyncDelaySimulator {
public:
    // 场景 A: 空异步操作（验收必测）
    void simulate_empty_async(AsyncCallback done) {
        // 不睡眠，立即完成，测试纯唤醒机制延迟
        async_pool_->submit([done]() {
            done();
        });
    }
    
    // 场景 B: 模拟数据库查询（典型场景）
    void simulate_database_query(uint64_t delay_ms, AsyncCallback done) {
        async_pool_->submit([delay_ms, done]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            done();
        });
    }
    
    // 可选场景 C-E（类似实现）
    void simulate_http_request(uint64_t delay_ms, AsyncCallback done);
    void simulate_file_io(uint64_t delay_ms, AsyncCallback done);
    void simulate_compute(uint64_t delay_ms, AsyncCallback done);
};

// 测试用例：场景 A（验收必测）
TEST(AsyncMiddlewareTest, PureWakeUpLatency) {
    HttpServer server(4);  // 4 Worker threads
    AsyncDelaySimulator simulator;
    
    std::atomic<int> callback_count{0};
    std::vector<uint64_t> latencies;
    
    // 注册异步中间件（空异步）
    server.use_async(new AsyncTestMiddleware([&](Request& req, Response& resp,
                                                  NextFunc next, AsyncCallback done) {
        auto start = get_current_us();
        
        simulator.simulate_empty_async([&]() {
            auto end = get_current_us();
            uint64_t latency_us = end - start;
            latencies.push_back(latency_us);
            callback_count++;
            
            next();
            done();
        });
    }));
    
    server.route_get("/async_test", [](Request& req, Response& resp) {
        resp.set_body("OK");
    });
    
    server.start();
    
    // 发送 1000 个请求（并发 100）
    for (int i = 0; i < 1000; i++) {
        send_http_request("GET /async_test");
    }
    
    // 等待所有回调完成
    while (callback_count.load() < 1000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // 分析延迟分布
    uint64_t p50 = percentile(latencies, 50);
    uint64_t p90 = percentile(latencies, 90);
    uint64_t p99 = percentile(latencies, 99);
    uint64_t max_latency = *std::max_element(latencies.begin(), latencies.end());
    
    std::cout << "Async wake-up latency (us):\n";
    std::cout << "  P50: " << p50 << "\n";
    std::cout << "  P90: " << p90 << "\n";
    std::cout << "  P99: " << p99 << "\n";
    std::cout << "  Max: " << max_latency << "\n";
    
    // 验收标准：P99 < 5000us (5ms)
    EXPECT_LT(p99, 5000);
    EXPECT_LT(max_latency, 10000);  // 最大延迟 < 10ms
    
    server.stop();
}
```

#### 延迟测量方法

**关键点**: 测量的是唤醒机制延迟，不包括异步操作本身。

```cpp
// 测量流程：
// 1. 异步任务提交前记录 start_time
// 2. 异步操作完成后（异步线程）记录 async_done_time
// 3. Worker 收到通知后（Worker 线程）记录 wake_up_time
// 4. 回调执行完成后记录 end_time

// 延迟分解：
// - async_operation_time = async_done_time - start_time（异步操作耗时，不计入）
// - wake_up_latency = wake_up_time - async_done_time（唤醒延迟，计入）
// - callback_execute_time = end_time - wake_up_time（回调执行耗时，计入）

// 总延迟（不含异步操作）：
// latency = wake_up_latency + callback_execute_time
```

#### 测试配置参数

```yaml
# test/config/async_test_config.yaml

async_test:
  # 测试请求总数
  total_requests: 1000
  
  # 并发连接数
  concurrent_connections: 100
  
  # 异步操作延迟范围（不计入测量）
  simulated_delay_range:
    database: [10, 50]      # ms
    http_request: [100, 500] # ms
    file_io: [5, 20]        # ms
    compute: [1, 10]        # ms
    empty: 0                # ms（纯唤醒测试）
  
  # 验收标准
  acceptance_criteria:
    p99_latency_ms: 5       # P99 延迟 < 5ms
    max_latency_ms: 10      # 最大延迟 < 10ms
  
  # Worker 线程数
  worker_threads: 4
  
  # 异步线程池大小
  async_pool_threads: 8
```

#### 测试执行脚本

```bash
#!/bin/bash
# scripts/test_async_latency.sh

echo "=== 异步中间件延迟测试 ==="

# 场景 E: 纯唤醒机制延迟（推荐用于验收）
echo ">>> 场景 E: 空异步操作（纯唤醒机制）"
./build/test/integration/cpp_api/test_async_middleware --scenario empty

# 场景 A: 模拟数据库查询（验证唤醒机制不受数据库延迟影响）
echo ">>> 场景 A: 模拟数据库查询（延迟 10-50ms）"
./build/test/integration/cpp_api/test_async_middleware --scenario database --delay 20

# 场景 B: 模拟 HTTP 外部请求（验证唤醒机制不受 HTTP 延迟影响）
echo ">>> 场景 B: 模拟 HTTP 请求（延迟 100-500ms）"
./build/test/integration/cpp_api/test_async_middleware --scenario http_request --delay 200

echo "=== 测试完成 ==="
```

#### 延迟分析报告模板

```
========== 异步中间件延迟测试报告 ==========

测试时间: 2026-04-XX XX:XX:XX
测试场景: 场景 E（空异步操作）
测试配置:
  - 请求总数: 1000
  - 并发连接: 100
  - Worker 线程: 4
  - 异步线程池: 8

测试结果（异步唤醒延迟，不含异步操作本身）:
  - P50 延迟: 1.2ms ✓
  - P90 延迟: 2.8ms ✓
  - P99 延迟: 3.5ms ✓ (目标 < 5ms)
  - 最大延迟: 6.2ms ✓ (目标 < 10ms)

延迟分解:
  - wake_up_latency: 平均 2.1ms（eventfd/pipe 通知 + Worker 处理）
  - callback_execute_time: 平均 1.2ms（回调执行）
  
异常情况:
  - 超时任务: 0 个
  - 取消任务: 0 个
  - 队列溢出: 0 次

验收结论: ✓ 全部通过
```

#### 注意事项

1. **分离测量**: 测量唤醒机制延迟，不包括异步操作本身耗时
2. **并发压力**: 测试并发 100 连接，验证高并发下唤醒机制性能
3. **多次测试**: 运行 3 次，取 P99 平均值，避免偶然波动
4. **资源监控**: 测试时监控 CPU/内存使用率，避免资源瓶颈影响
5. **超时处理**: 测试超时场景（异步操作超过 timeout_ms），验证超时机制正确
6. **取消处理**: 测试取消场景（客户端断开连接），验证取消机制正确

---

## 八、Phase 6: 完善与优化

**工作量**: 3-4 周（含缓冲）
**依赖**: Phase 5
**目标**: 文档完整 + 测试覆盖 > 80%

### 6.1 文档完善（并行组 A）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 6.1.1 | 完善 docs/README.md（使用说明 + 快速开始） | P0 | [Phase 5] | - | 待开始 |
| 6.1.2 | 完善 docs/api.md（完整 API 参考） | P0 | [6.1.1] | - | 待开始 |
| 6.1.3 | 完善 docs/architecture.md（架构详解 + 流程图） | P0 | [6.1.1] | [6.1.2] | 待开始 |
| 6.1.4 | 完善 docs/security.md（安全配置指南） | P0 | [5.3.1] | [6.1.1-3] | 待开始 |
| 6.1.5 | 完善 docs/migration.md（版本迁移指南） | P1 | [6.1.1] | [6.1.2-4] | 待开始 |

**验收标准**:
- [ ] README.md 包含快速开始指南
- [ ] api.md 包含所有 API 文档
- [ ] architecture.md 包含架构图
- [ ] security.md 包含安全配置示例

---

### 6.2 测试完善（并行组 B）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 6.2.1 | 配置覆盖率工具（lcov + CMake） | P0 | [Phase 5] | [6.1.1] | 待开始 |
| 6.2.2 | 补充 fuzz 测试（HTTP parser + SSL） | P0 | [1.3.2, 4.1.1] | [6.2.1] | 待开始 |
| 6.2.3 | 补充压力长跑测试（24h 测试） | P0 | [Phase 5] | [6.2.1, 6.2.2] | 待开始 |
| 6.2.4 | 补充竞态测试（多线程并发） | P0 | [2.1.1] | [6.2.1-3] | 待开始 |
| 6.2.5 | 覆盖率测试（目标 ≥ 80%） | P0 | [6.2.1-4] | - | 待开始 |

**验收标准**:
- [ ] 覆盖率 ≥ 80%（lcov 报告）
- [ ] fuzz 测试运行 1h 无崩溃
- [ ] 24h 压力测试无内存泄漏
- [ ] 竞态测试无死锁

---

### 6.3 性能优化（并行组 C）

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 6.3.1 | 分发策略缓存优化 | P1 | [2.1.3] | [6.1.1] | 待开始 |
| 6.3.2 | 大文件分段传输优化 | P1 | [2.2.3] | [6.3.1] | 待开始 |
| 6.3.3 | 性能基准测试（吞吐量 + 延迟） | P0 | [6.3.1-2] | - | 待开始 |
| 6.3.4 | nginx 对照测试（同配置下吞吐量对比） | P1 | [6.3.3] | - | 待开始 |
| 6.3.5 | libevent 对照测试（EventLoop 性能对比） | P1 | [6.3.3] | [6.3.4] | 待开始 |

**验收标准**:
- [ ] 最终吞吐量达标（≥ 设计目标）
- [ ] 延迟分布合理（P99 < 设计目标）
- [ ] nginx 对照测试完成（相对性能分析）
- [ ] libevent 对照测试完成（EventLoop 性能分析）

**性能对照测试配置（补充）**:

```bash
# nginx 对照测试配置
# 1. 安装 nginx（同配置）
sudo apt-get install nginx

# 2. 配置 nginx（4 worker_processes，同端口）
# /etc/nginx/nginx.conf
worker_processes 4;
events {
    worker_connections 10000;
}
http {
    server {
        listen 8081;
        location / {
            return 200 "Hello World";
        }
    }
}

# 3. 启动 nginx
sudo systemctl start nginx

# 4. 对照测试（同时运行）
wrk -t4 -c100 -d30s --latency http://localhost:8080/  # Chase
wrk -t4 -c100 -d30s --latency http://localhost:8081/  # nginx

# 5. 对比指标：
# - 吞吐量：Chase vs nginx（相对百分比）
# - 延迟分布：P50/P90/P99 对比
# - CPU 使用率：top/htop 监控
# - 内存使用：内存占用对比

# libevent 对照测试配置
# 1. 安装 libevent
vcpkg install libevent

# 2. 编写 libevent 对照服务器（类似 minimal_server.c）
// examples/libevent_server.c
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/http.h>

void http_request_cb(struct evhttp_request *req, void *arg) {
    struct evbuffer *buf = evbuffer_new();
    evbuffer_add_printf(buf, "Hello World");
    evhttp_send_reply(req, HTTP_OK, "OK", buf);
    evbuffer_free(buf);
}

int main() {
    struct event_base *base = event_base_new();
    struct evhttp *http = evhttp_new(base);
    evhttp_bind_socket(http, "0.0.0.0", 8082);
    evhttp_set_gencb(http, http_request_cb, NULL);
    event_base_dispatch(base);
    return 0;
}

# 3. 对照测试（EventLoop 性能）
wrk -t4 -c100 -d30s --latency http://localhost:8080/  # Chase EventLoop
wrk -t4 -c100 -d30s --latency http://localhost:8082/  # libevent

# 4. 对比指标：
# - EventLoop 效率：Chase vs libevent
# - 内存占用：Chase vs libevent
# - API 易用性：主观对比

# 性能对照报告模板
==========================================
性能对照测试报告
==========================================
测试时间: 2026-04-XX
测试环境: 4 CPU cores, 16GB RAM

Chase HTTP Server:
  - 吞吐量: XXXX req/s
  - P50 延迟: XX ms
  - P99 延迟: XX ms
  - CPU 使用: XX%
  - 内存占用: XX MB

nginx 对照:
  - 吞吐量: XXXX req/s
  - P50 延迟: XX ms
  - P99 延迟: XX ms
  - CPU 使用: XX%
  - 内存占用: XX MB

相对性能:
  - Chase vs nginx: XX%（吞吐量相对百分比）
  - Chase 延迟优势/劣势: XX ms

libevent 对照（EventLoop 性能）:
  - Chase EventLoop vs libevent: XX%（吞吐量相对百分比）

结论:
  - Chase 性能接近 nginx（XX% 范围内）✓
  - EventLoop 性能优于/接近 libevent ✓
  - 建议优化项：...
==========================================
```

---

### 6.4 Demo 程序

**任务清单**:
| ID | 任务 | 优先级 | blockedBy | parallelWith | 状态 |
|----|------|--------|-----------|--------------|------|
| 6.4.1 | 创建 demo/（完整示例） | P0 | [Phase 5] | [6.1.1] | 待开始 |
| 6.4.2 | 实现 demo 程序（含静态文件 + 路由 + 中间件） | P0 | [6.4.1] | - | 待开始 |
| 6.4.3 | 编写 demo README.md | P1 | [6.4.2] | - | 待开始 |

**验收标准**:
- [ ] demo 程序正常运行
- [ ] demo 文档完整

---

### Phase 6 总验收

**验收检查清单**:
| 检查项 | 目标值 | 测试方法 | 通过标准 |
|--------|--------|----------|----------|
| 测试覆盖率 | ≥ 80% | lcov | 报告确认 |
| 内存泄漏 | 0 | Valgrind 24h | 0 leaked |
| fuzz 测试 | 1h 无崩溃 | libFuzzer | 报告确认 |
| 竞态测试 | 无死锁 | ThreadSanitizer | 报告确认 |
| 文档完整 | 5 个文档 | 检查 | 全部完成 |

**Phase 6 失败处理**:
- 覆盖率不达标 → 分析未覆盖代码路径，补充针对性测试用例，优先覆盖核心路径
- 内存泄漏 → 定位泄漏位置（使用 Valgrind --leak-check=full），修复后重新长跑测试
- fuzz 测试崩溃 → 分析崩溃日志，定位边界条件问题，补充安全检查
- 竞态测试死锁 → 使用 ThreadSanitizer 定位竞态点，检查锁获取顺序，避免循环等待
- 性能不达标 → 使用 perf 分析瓶颈，优化热点函数，调整缓冲区大小
- 文档不完整 → 按模板补充缺失章节，确保 API 文档覆盖所有接口

---

## 九、总任务统计

| Phase | 任务数（改进后） | 工作量（含缓冲） | 工作量（优化后） |
|-------|------------------|------------------|------------------|
| Phase 1 | 21（合并后） | 3-4 周 | 4-5 周（含调试时间） |
| Phase 2 | 16（合并后） | 4-5 周 | 5-6 周（含性能调优） |
| Phase 3 | 9（合并后） | 3-4 周 | 4-5 周（含合规测试） |
| Phase 4 | 18（合并后） | 4-5 周 | 5-6 周（含双版本测试） |
| Phase 5 | 29（合并后） | 6-8 周（含 35% 缓冲） | 8-10 周（含 C++ 封装调试） |
| Phase 6 | 14（合并后） | 3-4 周 | 4-5 周（含覆盖率补充） |
| **总计** | **107** | **21-29 周** | **30-37 周（含 30% 缓冲）** |

**工作量优化说明**:
- Phase 1 增加 1 周调试时间（HTTP 解析边界条件）
- Phase 2 增加 1 周性能调优时间（负载均衡测试）
- Phase 3 增加 1 周合规测试时间（HTTP/1.1 规范复杂）
- Phase 4 增加 1 周双版本测试时间（OpenSSL 1.1.1 + 3.x）
- Phase 5 增加 2 周 C++ 封装调试时间（API 易用性迭代）
- Phase 6 增加 1 周覆盖率补充时间（fuzz + 竞态测试）
- 总缓冲：30%（应对未知风险）

---

## 十、里程碑

| 里程碑 | 完成标准 | 预计时间（含缓冲） | 预计时间（优化后） |
|--------|----------|------------------|------------------|
| M1: Phase 1 完成 | 单线程服务器响应 GET，吞吐量 ≥ 2000 req/s（未优化） | 第 3-4 周 | 第 4-5 周 |
| M2: Phase 2 完成 | 多线程 + 静态文件，吞吐量 ≥ 5000 req/s | 第 8-9 周 | 第 9-11 周 |
| M3: Phase 3 完成 | HTTP/1.1 合规，Keep-Alive ≥ 100 并发 | 第 11-13 周 | 第 13-16 周 |
| M4: Phase 4 完成 | HTTPS + 虚拟主机，握手延迟 < 50ms | 第 15-18 周 | 第 18-22 周 |
| M5: Phase 5 完成 | C++ API + 安全，DDoS 防护生效 | 第 21-27 周 | 第 26-32 周 |
| M6: Phase 6 完成 | 文档 + 测试覆盖 ≥ 80% | 第 29 周 | 第 30-37 周 |

**里程碑调整说明**:
- M1 增加 1 周调试时间
- M2 增加 1 周性能调优时间
- M3 增加 1 周合规测试时间
- M4 增加 1 周双版本测试时间
- M5 增加 2 周 C++ 封装调试时间
- M6 增加 1 周覆盖率补充时间
- 总体延长 7-8 周，更符合实际开发周期

---

## 十一、风险与缓解

| 风险 | 影响 | 缓解措施 | 触发条件 |
|------|------|----------|----------|
| OpenSSL 版本兼容 | Phase 4 延期 | Phase 4 开始前测试 1.1.1 和 3.x | 编译失败 |
| HTTP/1.1 规范复杂 | Phase 3 延期 | Phase 3 增加 1 周缓冲，参考 nginx 实现 | 合规测试失败 |
| 测试覆盖不足 | Phase 6 延期 | Phase 5 同步写测试，覆盖率目标 70% | 覆盖率 < 70% |
| 内存泄漏 | 稳定性问题 | 每个 Phase 做 Valgrind 检测 | 泄漏 > 0 |
| 任务阻塞 | 整体延期 | 标注并行任务，多 Agent 协作 | 单 Agent 卡住 |
| C++ API 易用性不佳 | Phase 5 延期 | Phase 5 增加 user test，准备迭代改进 | 用户反馈负面 |
| 异步唤醒机制失效 | Phase 5 延期 | Phase 2 验证 eventfd/pipe 机制，增加测试用例 | 测试失败 |
| 分布式限流竞态 | Phase 5 延期 | Phase 5 使用 ThreadSanitizer 测试，本地计数 + 定期同步 | 竞态检测 |
| 高并发性能瓶颈 | Phase 2 延期 | 提前做性能基准测试，优化分发策略 | 吞吐量 < 目标 |

---

## 十二、回滚机制与进度跟踪

### 回滚机制

每个 Phase 完成后应创建 git commit checkpoint，便于失败时回滚：

**回滚策略**:

| 场景 | 回滚点 | 操作 |
|------|--------|------|
| Phase 集成失败 | Phase 开始时的 commit | `git reset --hard <checkpoint>` |
| 单模块失败 | 该模块之前的 commit | `git checkout <checkpoint> -- <module_path>` |
| 测试失败 | 最近一次通过的 commit | `git revert <failed_commit>` |
| 性能不达标 | 优化前的 commit | `git reset --hard <checkpoint>` |

**Checkpoint 创建时机**:
```bash
# 每个 Phase 开始前
git tag phase-1-start

# 每个任务组完成后
git commit -m "checkpoint: Phase 1.2 eventloop/timer 完成"

# Phase 验收通过后
git tag phase-1-complete
```

**回滚操作示例**:
```bash
# 回滚整个 Phase
git reset --hard phase-1-start

# 回滚单个文件
git checkout phase-1-start -- src/eventloop.c

# 查看 checkpoint 历史
git log --oneline --grep="checkpoint"
```

---

### 分支保护策略

**重要**: 不要在 main 分支直接开发，使用 feature 分支隔离：

**分支命名规范**:
```
feature/http-server-phase-1    # Phase 1 开发分支
feature/http-server-phase-2    # Phase 2 开发分支
feature/eventloop-impl         # 单模块开发分支（可选）
bugfix/ssl-version-compat      # bug 修复分支
```

**分支工作流程**:
```bash
# 1. 创建 Phase feature 分支
git checkout main
git pull origin main
git checkout -b feature/http-server-phase-1
git push -u origin feature/http-server-phase-1

# 2. 在 feature 分支开发
git add src/ include/
git commit -m "feat(eventloop): implement epoll backend"

# 3. 创建 checkpoint tag（在 feature 分支）
git tag phase-1-checkpoint-1

# 4. 回滚只影响 feature 分支
git reset --hard phase-1-checkpoint-1

# 5. Phase 验收通过后合并到 main
git checkout main
git merge feature/http-server-phase-1 --no-ff
git tag phase-1-complete

# 6. 删除已合并的 feature 分支
git branch -d feature/http-server-phase-1
git push origin --delete feature/http-server-phase-1
```

**分支保护规则**:
| 规则 | 说明 |
|------|------|
| main 分支禁止直接 push | 必须通过 PR/MR 合并 |
| feature 分支可自由 reset | 不影响其他协作者 |
| Phase 完成前不合并 main | 防止未稳定代码污染主分支 |
| 合并前必须通过 CI | GitHub Actions 绿色 |

**多人协作场景**:
```bash
# 协作者 A 和 B 同时开发 Phase 1
# A 负责 eventloop
git checkout -b feature/eventloop-impl

# B 负责 http_parser
git checkout -b feature/http-parser-impl

# 各自开发完成后合并到 Phase 1 主分支
git checkout feature/http-server-phase-1
git merge feature/eventloop-impl
git merge feature/http-parser-impl

# Phase 1 验收通过后合并到 main
```

---

### 进度跟踪机制

使用 TaskUpdate 工具跟踪任务状态，状态包括：

| 状态 | 说明 | 触发条件 |
|------|------|----------|
| pending | 待开始 | 任务创建时 |
| in_progress | 进行中 | 开始执行任务 |
| blocked | 阻塞 | 依赖任务未完成 |
| completed | 完成 | 任务验收通过 |
| deleted | 已删除 | 任务不再需要 |

**任务状态更新示例**:

```
| 1.2.1 | eventloop 模块 | P0 | blockedBy: [1.1.1] | - | pending |
→ 开始执行 → in_progress（50%）
→ 依赖完成 → in_progress（100%）
→ 测试通过 → completed

| 1.2.4 | 单元测试 | P0 | blockedBy: [1.2.1-3] | - | blocked（等 1.2.3） |
→ 1.2.3 完成 → pending
→ 开始执行 → in_progress
→ 测试通过 → completed
```

**阻塞状态说明**:
- blockedBy 任务未完成时，当前任务状态为 blocked
- 所有 blockedBy 任务完成后，状态自动变为 pending
- 可通过 TaskUpdate 手动更新状态

**TaskUpdate 工具使用示例**:

```json
// 开始任务（设置 in_progress + owner）
TaskUpdate({
    taskId: "1.2.1",
    status: "in_progress",
    owner: "agent-1",
    activeForm: "实现 eventloop 模块"
})

// 任务阻塞（添加阻塞原因）
TaskUpdate({
    taskId: "1.2.4",
    status: "pending",  // 注意：工具不支持 blocked 状态，用 pending + metadata 表示
    metadata: {
        blocked_reason: "等待 1.2.1-1.2.3 完成",
        blockedBy: ["1.2.1", "1.2.2", "1.2.3"]
    }
})

// 任务完成
TaskUpdate({
    taskId: "1.2.1",
    status: "completed"
})

// 查看任务详情
TaskGet({taskId: "1.2.1"})
// 返回: subject, description, status, owner, blockedBy, blocks

// 查看任务列表
TaskList()
// 返回: 所有任务状态摘要（id, subject, status, owner, blockedBy）

// 删除不再需要的任务
TaskUpdate({
    taskId: "1.1.4",
    status: "deleted"
})
```

**多 Agent 协作示例**:
```json
// Agent-1 完成任务后通知 Agent-2
// Agent-1:
TaskUpdate({taskId: "1.2.1", status: "completed"})
SendMessage({to: "agent-2", message: "任务 1.2.1 已完成，可开始 1.2.4"})

// Agent-2 收到通知后检查依赖
TaskList()  // 确认 1.2.1-1.2.3 都 completed
TaskUpdate({taskId: "1.2.4", status: "in_progress", owner: "agent-2"})
```

**进度报告格式**:
```
Phase 1 进度: 21 任务
├── pending: 10 任务
├── in_progress: 8 任务
├── blocked: 2 任务（等依赖完成）
└── completed: 1 任务（1.1.1）

阻塞原因:
- 1.3.4 blocked: 等 [1.3.1, 1.3.2, 1.3.3] 完成
- 1.5.1 blocked: 等 [1.2, 1.3, 1.4] 完成
```

---

### 优先级调整机制

当任务阻塞超过预期时间时，可动态调整优先级：

| 触发条件 | 调整策略 |
|----------|----------|
| P0 任务阻塞 > 3 天 | 考虑拆分任务或临时降级为 P1 |
| P2 任务无阻塞且可并行 | 临时提升为 P1，提前完成 |
| Phase 结束时 | 重新评估下一 Phase 优先级分布 |
| 依赖任务长期未完成 | 检查阻塞原因，考虑绕过或替代方案 |

**优先级调整流程**:
1. 记录阻塞时间和原因
2. 分析是否可拆分或绕过
3. 调整优先级并通知相关 Agent
4. 更新任务列表和依赖关系

---

### Phase 转换质量门禁

每个 Phase 进入下一阶段前必须通过以下质量门禁检查：

| 门禁项 | 检查内容 | 工具 | 不通过处理 |
|--------|----------|------|------------|
| 编译 | Linux + macOS 无错误无警告 | CMake + make | 回退修复编译错误 |
| 测试 | 单元测试 100% 通过 | ctest | 分析失败用例，修复后重新运行 |
| 覆盖率 | ≥ 70% | lcov | 补充测试用例，覆盖未测试路径 |
| 内存 | Valgrind 0泄漏（10min） | valgrind --leak-check=full | 定位泄漏位置，修复 malloc/free 配对 |
| 静态分析 | clang-tidy 0 warning | clang-tidy | 修复代码质量问题（bugprone/performance） |
| 代码风格 | clang-format 0 warning | clang-format | 自动格式化，重新提交 |
| CI | GitHub Actions 绿色 | PR check | 检查 CI 日志，修复失败步骤 |

**门禁执行顺序**:
```
1. clang-format → 自动格式化（低成本）
2. clang-tidy → 静态分析（中等成本）
3. CMake build → 编译检查（必须通过）
4. ctest → 单元测试（必须通过）
5. lcov → 覆盖率检查（必须达标）
6. Valgrind → 内存检查（必须 0 泄漏）
7. GitHub Actions → CI 集成（最终确认）
```

**门禁通过后的操作**:
```bash
# 1. 创建 Phase 完成 tag
git tag phase-1-complete

# 2. 合并到 main 分支
git checkout main
git merge feature/http-server-phase-1 --no-ff

# 3. 推送到远程
git push origin main --tags

# 4. 开始下一 Phase
git checkout -b feature/http-server-phase-2
```

**门禁不通过的处理**:
```bash
# 1. 回退到最近 checkpoint
git reset --hard phase-1-checkpoint-last

# 2. 分析门禁失败原因
# 查看 CI 日志、Valgrind 报告、lcov 报告

# 3. 修复问题
# 补充测试、修复泄漏、格式化代码

# 4. 重新执行门禁检查
cmake --build build && ctest && valgrind ...

# 5. 通过后重新创建 checkpoint
git commit -m "checkpoint: Phase 1 门禁修复完成"
git tag phase-1-checkpoint-fixed
```

**快速门禁检查脚本**:
```bash
#!/bin/bash
# scripts/quality_gate.sh

echo "=== Phase 质量门禁检查 ==="

# 1. 代码风格
echo ">>> clang-format 检查"
find src include -name "*.c" -o -name "*.h" | xargs clang-format --dry-run --Werror
if [ $? -ne 0 ]; then echo "FAIL: clang-format"; exit 1; fi

# 2. 静态分析
echo ">>> clang-tidy 检查"
clang-tidy src/*.c -- -std=c11 -I./include
if [ $? -ne 0 ]; then echo "FAIL: clang-tidy"; exit 1; fi

# 3. 编译
echo ">>> CMake 编译"
cmake --build build
if [ $? -ne 0 ]; then echo "FAIL: build"; exit 1; fi

# 4. 测试
echo ">>> 单元测试"
cd build && ctest --output-on-failure
if [ $? -ne 0 ]; then echo "FAIL: tests"; exit 1; fi

# 5. 覆盖率（可选）
echo ">>> 覆盖率检查"
lcov --capture --directory . --output-file coverage.info
COVERAGE=$(lcov --summary coverage.info 2>&1 | grep "lines" | grep -oP '\d+\.\d+')
if [ "$COVERAGE" -lt 70 ]; then echo "FAIL: coverage $COVERAGE%"; exit 1; fi

# 6. 内存检查
echo ">>> Valgrind 内存检查"
valgrind --leak-check=full --error-exitcode=1 ./build/test/integration/c_core/test_minimal_server 10
if [ $? -ne 0 ]; then echo "FAIL: valgrind"; exit 1; fi

echo "=== 所有门禁通过 ==="
exit 0
```

---

## 十三、开始实施建议

### 立即可执行（Phase 1）

1. **任务 1.1.1**：创建目录结构 + CMake 配置
2. **任务 1.1.2**：配置 GitHub Actions CI（可并行）
3. **任务 1.2.1**：实现 eventloop 模块（依赖 1.1.1）

### CI 配置先行

Phase 1 任务 1.1.2 必须优先完成，确保后续代码都有 CI 检查。

### 文档同步更新

实现过程遇到的问题记录到对应文档的 "已知问题" 章节。

---

## 十四、并行执行策略

### 可并行任务组

| 并行组 | 可同时执行的任务 | 条件 |
|--------|------------------|------|
| Phase 1-A | 1.1.1（基础设施） | 无依赖 |
| Phase 1-B | 1.2.1（eventloop） ← 依赖 1.1.1 | 1.1.1 完成 |
| Phase 1-C | 1.3.1（connection） + 1.3.2（http_parser） + 1.4.1（router） | 1.2.1 完成 |
| Phase 2-A | 2.1.1（ThreadPool） + 2.2.1（fileserve） | Phase 1 完成 |
| Phase 4-A | 4.1.1（ssl_wrap） + 4.2.1（vhost） + 4.3.1（config） | Phase 2 完成 |
| Phase 5-A | 5.1.1（HttpServer） + 5.2.1（Middleware） + 5.3.1（security） + 5.4.1（logger） | Phase 4 完成 |

### 建议：多 Agent 协作

使用 TaskCreate + Agent 工具实现并行开发：
- Agent 1 负责 C 核心模块（eventloop/timer/connection）
- Agent 2 负责 HTTP 解析（http_parser/router/error）
- Agent 3 负责 CI 配置 + 测试框架

---

## 十五、压力测试配置模板

### 测试环境配置

| 参数 | 推荐值 | 说明 |
|------|--------|------|
| 测试机器 CPU | 4-8 核 | 与生产环境匹配 |
| 测试机器内存 | 8-16 GB | 避免内存瓶颈 |
| 操作系统 | Linux (Ubuntu 20.04+) | 主要生产环境 |
| 网络环境 | 本地局域网 | 避免 RTT 影响 |
| 服务器配置 | 4 Worker threads | 默认配置 |

### wrk 测试配置模板

```bash
# Phase 1 单线程吞吐量测试
wrk -t1 -c10 -d10s --latency http://localhost:8080/

# Phase 2 多线程吞吐量测试
wrk -t4 -c100 -d30s --latency http://localhost:8080/

# Phase 2 静态文件测试（1MB 文件）
wrk -t4 -c100 -d30s --latency http://localhost:8080/static/test_1mb.txt

# Phase 3 Keep-Alive 测试（持久连接）
wrk -t4 -c100 -d60s --latency -H "Connection: keep-alive" http://localhost:8080/

# Phase 4 HTTPS 测试
wrk -t4 -c100 -d30s --latency https://localhost:8443/

# 高并发压力测试（10K 连接）
wrk -t8 -c10000 -d60s --latency --timeout 30s http://localhost:8080/

# 延迟分布分析（P50/P90/P99）
wrk -t4 -c100 -d60s --latency http://localhost:8080/api/json
```

### ab 测试配置模板（Apache Bench）

```bash
# 基础吞吐量测试
ab -n 10000 -c 100 http://localhost:8080/

# Keep-Alive 测试
ab -n 10000 -c 100 -k http://localhost:8080/

# POST 请求测试（JSON payload）
ab -n 1000 -c 50 -p payload.json -T "application/json" http://localhost:8080/api/post

# HTTPS 测试
ab -n 1000 -c 50 -f TLS1.2 https://localhost:8443/
```

### hey 测试配置模板（现代 HTTP 压测工具）

```bash
# 基础吞吐量测试（JSON 输出）
hey -z 30s -c 100 -q 10 http://localhost:8080/

# POST 请求测试
hey -z 30s -c 100 -m POST -H "Content-Type: application/json" -d '{"test": "data"}' http://localhost:8080/api

# 详细延迟分布
hey -z 60s -c 100 -q 20 http://localhost:8080/ > latency_report.txt
```

### 测试 URL 和 Payload 定义

| Phase | 测试 URL | Payload | 说明 |
|-------|----------|---------|------|
| Phase 1 | `/` | - | 简单 GET 请求（Hello World） |
| Phase 1 | `/api` | - | 路由匹配测试 |
| Phase 2 | `/static/index.html` | - | 小静态文件（< 10KB） |
| Phase 2 | `/static/test_1mb.txt` | - | 大静态文件（1MB） |
| Phase 3 | `/api/json` | - | JSON 响应（chunked） |
| Phase 3 | `/upload` | JSON (1KB) | POST 请求测试 |
| Phase 4 | `/secure/` | - | HTTPS 静态文件 |
| Phase 5 | `/api/auth` | JSON (token) | 认证中间件测试 |

### Payload 文件模板

```json
// payload.json（1KB 测试数据）
{
  "test_data": "Lorem ipsum dolor sit amet...",
  "timestamp": 1234567890,
  "user_id": "test_user_001",
  "request_id": "req_001_002_003"
}

// auth_payload.json（认证测试）
{
  "token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
  "action": "read",
  "resource": "/api/data"
}
```

### 性能指标验收标准

| Phase | 指标 | 目标值 | 测试工具 | 命令 |
|-------|------|--------|----------|------|
| Phase 1 | 吞吐量 | ≥ 2000 req/s（未优化） | wrk | `-t1 -c10 -d10s` |
| Phase 1 | P99 延迟 | < 10ms | wrk --latency | `-t1 -c10 -d30s` |
| Phase 2 | 吞吐量 | ≥ 5000 req/s | wrk | `-t4 -c100 -d30s` |
| Phase 2 | 负载均衡偏差 | < 10% | 自定义脚本 | 统计 Worker 连接数 |
| Phase 2 | 静态文件吞吐 | ≥ 3000 req/s | wrk | 1MB 文件测试 |
| Phase 3 | Keep-Alive 并发 | ≥ 100 | wrk -k | `-t4 -c100 -d60s` |
| Phase 4 | HTTPS 握手延迟 | < 50ms | openssl s_client | `time openssl s_client` |
| Phase 5 | 异步中间件延迟 | < 5ms | hey | `-z 30s -c 100` |
| Phase 6 | 24h 内存泄漏 | 0 | Valgrind | 长跑测试 |

### 测试报告模板

```
========== HTTP Server 性能测试报告 ==========

测试时间: 2026-04-XX XX:XX:XX
测试工具: wrk 4.2.0
测试配置:
  - 线程数: 4
  - 连接数: 100
  - 持续时间: 30s
  - URL: http://localhost:8080/

测试环境:
  - OS: Ubuntu 20.04 LTS
  - CPU: Intel i7-9700K (8 cores)
  - Memory: 16GB DDR4
  - Network: 本地局域网（RTT < 1ms）

服务器配置:
  - Worker threads: 4
  - Max connections: 10000
  - Keepalive timeout: 60s

测试结果:
  - 吞吐量: 5234 req/s ✓ (目标 ≥ 5000)
  - 平均延迟: 19.2ms
  - P50 延迟: 15ms
  - P90 延迟: 32ms
  - P99 延迟: 45ms ✓ (目标 < 100ms)
  - 最大延迟: 120ms
  - 错误率: 0.00%

负载均衡统计:
  - Worker 0: 256 connections (25.6%)
  - Worker 1: 248 connections (24.8%)
  - Worker 2: 251 connections (25.1%)
  - Worker 3: 245 connections (24.5%)
  - 偏差: 1.2% ✓ (目标 < 10%)

验收结论: ✓ 全部通过
```

### 快速测试脚本

```bash
#!/bin/bash
# scripts/quick_benchmark.sh

echo "=== HTTP Server 快速基准测试 ==="

# 启动服务器（假设已编译）
./build/examples/minimal_server &
SERVER_PID=$!
sleep 2  # 等待服务器启动

# 测试 1: 单线程吞吐量
echo ">>> 测试 1: 单线程吞吐量"
wrk -t1 -c10 -d10s --latency http://localhost:8080/ > test1_result.txt
THROUGHPUT=$(grep "Requests/sec" test1_result.txt | awk '{print $2}')
echo "吞吐量: $THROUGHPUT req/s"

# 测试 2: 多线程吞吐量
echo ">>> 测试 2: 多线程吞吐量"
wrk -t4 -c100 -d30s --latency http://localhost:8080/ > test2_result.txt

# 测试 3: 静态文件
echo ">>> 测试 3: 静态文件吞吐量"
wrk -t4 -c100 -d30s --latency http://localhost:8080/static/test_1mb.txt > test3_result.txt

# 清理
kill $SERVER_PID

echo "=== 测试完成，结果保存在 test*_result.txt ==="
```

### 注意事项

1. **预热时间**: 服务器启动后等待 2-5 秒预热，避免冷启动影响
2. **多次测试**: 每个测试运行 3 次，取平均值，避免偶然波动
3. **资源监控**: 测试时监控 CPU/内存/网络使用率（top/htop/nethogs）
4. **错误分析**: 如果错误率 > 0.1%，分析日志找出原因
5. **环境一致性**: 多次测试使用相同配置，避免结果不可比
6. **线程数匹配**: wrk 线程数建议与服务器 Worker 数匹配（避免过度竞争）

---

**下一步**: 开始执行任务 **1.1.1**（创建目录结构 + CMake 配置）