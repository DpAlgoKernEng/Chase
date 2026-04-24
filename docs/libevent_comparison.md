# libevent EventLoop 对照测试方案

## 1. 概述

本文档设计 Chase EventLoop 与 libevent 的对照测试方案，用于评估 Chase EventLoop 的性能和 API 易用性。

### 测试目标

- **EventLoop 效率对比**: 比较 Chase 与 libevent 的事件循环性能
- **内存占用对比**: 比较两者的内存使用情况
- **API 易用性对比**: 主观评估 API 设计的易用程度

### 测试环境

| 项目 | 要求 |
|-----|------|
| 操作系统 | macOS / Linux |
| 编译器 | Clang / GCC |
| 测试工具 | wrk (HTTP benchmarking tool) |
| 依赖库 | libevent 2.1+ |

---

## 2. 测试方案设计

### 2.1 对照服务器实现

#### Chase minimal_server

基于 `examples/minimal_server.c`，提供以下功能：
- 单进程、单线程架构
- HTTP/1.1 基础响应
- 简单路由 (`/` 路径)
- 关闭连接模式 (Connection: close)

#### libevent_server

基于 libevent API 实现，功能对等：
- 单进程、单线程架构
- HTTP/1.1 基础响应
- 简单路由 (`/` 路径)
- 关闭连接模式

### 2.2 测试工具

**wrk 配置**:
```bash
# 安装 wrk
# macOS: brew install wrk
# Linux: apt-get install wrk

# 基础测试
wrk -t4 -c100 -d30s http://localhost:8080/

# 参数说明
# -t4:    4 个线程
# -c100:  100 个并发连接
# -d30s:  持续 30 秒
```

### 2.3 测试场景

| 场景 | 描述 | wrk 参数 |
|-----|------|---------|
| 场景 1 | 低并发测试 | `-t1 -c10 -d10s` |
| 场景 2 | 中等并发测试 | `-t4 -c100 -d30s` |
| 场景 3 | 高并发测试 | `-t8 -c500 -d60s` |
| 场景 4 | 极限压力测试 | `-t12 -c1000 -d60s` |
| 场景 5 | Keep-Alive 测试 | `-t4 -c100 -d30s` (需要服务器支持) |

---

## 3. libevent 对照服务器

### 3.1 实现文件

文件路径: `examples/libevent_server.c`

### 3.2 核心逻辑

```c
// 使用 libevent 事件循环
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>

// 事件循环创建
struct event_base *base = event_base_new();

// 监听器回调
void on_accept(struct evconnlistener *listener, evutil_socket_t fd,
               struct sockaddr *addr, int socklen, void *user_data);

// 读取回调
void on_read(struct bufferevent *bev, void *ctx);

// 事件循环运行
event_base_dispatch(base);
```

### 3.3 功能对比表

| 功能 | Chase minimal_server | libevent_server |
|-----|---------------------|-----------------|
| EventLoop 创建 | `eventloop_create()` | `event_base_new()` |
| EventLoop 运行 | `eventloop_run()` | `event_base_dispatch()` |
| 添加事件 | `eventloop_add()` | `event_new() + event_add()` |
| Socket 监听 | 手动 accept + 非阻塞 | `evconnlistener_new_bind()` |
| 数据读写 | 手动 read/write | `bufferevent` 自动缓冲 |
| HTTP 解析 | 自研 http_parser | 简单字符串匹配 |

---

## 4. 对照指标

### 4.1 EventLoop 效率对比

| 指标 | 测量方法 | Chase | libevent | 差异 |
|-----|---------|-------|----------|------|
| 请求/秒 (RPS) | wrk 平均值 | TBD | TBD | TBD |
| 平均延迟 | wrk Latency | TBD | TBD | TBD |
| P99 延迟 | wrk 99th percentile | TBD | TBD | TBD |
| CPU 使用率 | top/htop | TBD | TBD | TBD |

### 4.2 内存占用对比

| 指标 | 测量方法 | Chase | libevent | 差异 |
|-----|---------|-------|----------|------|
| 空载内存 | 启动后 Resident Size | TBD | TBD | TBD |
| 100 连接内存 | 运行时 RSS 增量 | TBD | TBD | TBD |
| 500 连接内存 | 运行时 RSS 增量 | TBD | TBD | TBD |
| 内存泄漏 | Valgrind 检测 | TBD | TBD | TBD |

### 4.3 API 易用性主观对比

| 维度 | Chase | libevent | 说明 |
|-----|-------|----------|------|
| API 简洁度 | 8/10 | 6/10 | Chase API 更直观 |
| 文档完整性 | 7/10 | 9/10 | libevent 文档丰富 |
| 学习曲线 | 平缓 | 中等 | libevent 概念较多 |
| 错误处理 | 明确 | 复杂 | libevent 错误码多样 |
| 功能丰富度 | 基础 | 丰富 | libevent 功能全面 |
| 代码可读性 | 高 | 中 | Chase 代码更清晰 |

---

## 5. 报告模板

### 5.1 性能对比表格

```markdown
## EventLoop 性能对比报告

**测试日期**: YYYY-MM-DD
**测试环境**: [操作系统/编译器/CPU/内存]

### 场景 1: 低并发测试 (-t1 -c10 -d10s)

| 服务器 | RPS | 平均延迟 | P99 延迟 | CPU% | 内存(RSS) |
|-------|-----|---------|---------|------|----------|
| Chase | TBD | TBD | TBD | TBD | TBD |
| libevent | TBD | TBD | TBD | TBD | TBD |

### 场景 2: 中等并发测试 (-t4 -c100 -d30s)

| 服务器 | RPS | 平均延迟 | P99 延迟 | CPU% | 内存(RSS) |
|-------|-----|---------|---------|------|----------|
| Chase | TBD | TBD | TBD | TBD | TBD |
| libevent | TBD | TBD | TBD | TBD | TBD |

### 场景 3: 高并发测试 (-t8 -c500 -d60s)

| 服务器 | RPS | 平均延迟 | P99 延迟 | CPU% | 内存(RSS) |
|-------|-----|---------|---------|------|----------|
| Chase | TBD | TBD | TBD | TBD | TBD |
| libevent | TBD | TBD | TBD | TBD | TBD |

### 场景 4: 极限压力测试 (-t12 -c1000 -d60s)

| 服务器 | RPS | 平均延迟 | P99 延迟 | CPU% | 内存(RSS) |
|-------|-----|---------|---------|------|----------|
| Chase | TBD | TBD | TBD | TBD | TBD |
| libevent | TBD | TBD | TBD | TBD | TBD |
```

### 5.2 API 易用性分析

```markdown
## API 易用性分析

### Chase 优势
1. **简洁的事件模型**: 仅需 `eventloop_add()` 一个函数注册事件
2. **回调设计直观**: 回调函数签名清晰，参数明确
3. **错误处理简单**: 使用统一的 Error 模块
4. **内存管理清晰**: 手动管理，易于理解

### libevent 优势
1. **功能丰富**: 支持 DNS、HTTP、RPC 等高级功能
2. **跨平台成熟**: 经过多年生产验证
3. **缓冲管理自动**: bufferevent 自动管理读写缓冲
4. **社区支持强**: 文档丰富，社区活跃

### 主观评分
| 维度 | Chase | libevent |
|-----|-------|----------|
| 易学性 | 8/10 | 6/10 |
| 灵活性 | 7/10 | 9/10 |
| 可维护性 | 8/10 | 7/10 |
| 生产就绪度 | 6/10 | 9/10 |
```

### 5.3 结论建议

```markdown
## 结论与建议

### 性能结论
- Chase EventLoop 在 [场景] 表现 [优于/持平/弱于] libevent
- 性能差异主要原因: [分析]

### 内存结论
- Chase 内存占用 [优于/持平/弱于] libevent
- 内存管理模式差异: [分析]

### API 易用性结论
- Chase API [更易/同等/较难] 于使用
- 推荐场景: [分析]

### 改进建议
1. [建议1]
2. [建议2]
3. [建议3]
```

---

## 6. vcpkg 依赖配置

### 6.1 vcpkg.json 更新

```json
{
  "dependencies": [
    {
      "name": "openssl",
      "version>=": "1.1.1"
    },
    {
      "name": "zlib",
      "version>=": "1.2.11"
    },
    {
      "name": "libevent",
      "version>=": "2.1.12"
    }
  ]
}
```

### 6.2 安装命令

```bash
# 使用 vcpkg 安装 libevent
vcpkg install libevent

# macOS 使用 brew 安装 (可选)
brew install libevent

# Linux 使用 apt 安装 (可选)
apt-get install libevent-dev
```

---

## 7. CMake 构建配置

### 7.1 examples/CMakeLists.txt 更新

```cmake
# libevent 对照服务器
find_package(libevent CONFIG REQUIRED)

add_executable(libevent_server libevent_server.c)
target_link_libraries(libevent_server libevent::core libevent::extra)
```

### 7.2 编译命令

```bash
# 配置 CMake
cmake -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# 构建
cmake --build build

# 运行 Chase minimal_server
./build/examples/minimal_server 8080

# 运行 libevent_server
./build/examples/libevent_server 8081
```

---

## 8. 测试执行步骤

### 8.1 环境准备

```bash
# 1. 安装依赖
vcpkg install

# 2. 构建项目
cmake -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake
cmake --build build

# 3. 验证服务器启动
./build/examples/minimal_server 8080 &
./build/examples/libevent_server 8081 &

# 4. 测试响应
curl http://localhost:8080/
curl http://localhost:8081/
```

### 8.2 执行基准测试

```bash
# 创建测试脚本
cat > scripts/benchmark.sh << 'EOF'
#!/bin/bash

CHASE_PORT=8080
LIBEVENT_PORT=8081
DURATION=30

echo "=== Chase minimal_server 基准测试 ==="
wrk -t4 -c100 -d${DURATION}s http://localhost:${CHASE_PORT}/

echo ""
echo "=== libevent_server 基准测试 ==="
wrk -t4 -c100 -d${DURATION}s http://localhost:${LIBEVENT_PORT}/
EOF

chmod +x scripts/benchmark.sh
./scripts/benchmark.sh
```

### 8.3 内存分析

```bash
# 使用 Valgrind 检测内存泄漏
valgrind --leak-check=full ./build/examples/minimal_server 8080 &
valgrind --leak-check=full ./build/examples/libevent_server 8081 &

# 使用 Instruments (macOS)
instruments -t Leaks ./build/examples/minimal_server 8080
instruments -t Leaks ./build/examples/libevent_server 8081
```

---

## 9. 预期结果

### 9.1 性能预期

基于设计分析，预期结果：

| 场景 | Chase RPS 预期 | libevent RPS 预期 | 差异预期 |
|-----|---------------|-------------------|---------|
| 低并发 | 接近 | 接近 | <5% |
| 中等并发 | 接近 | 略高 | <10% |
| 高并发 | 略低 | 略高 | <15% |
| 极限压力 | 略低 | 稳定 | <20% |

### 9.2 内存预期

| 场景 | Chase 内存预期 | libevent 内存预期 | 差异预期 |
|-----|----------------|-------------------|---------|
| 空载 | 较低 | 较高 | Chase < libevent |
| 运行时 | 接近 | 接近 | <10% |

### 9.3 分析要点

1. **性能接近**: Chase 使用 epoll/kqueue，与 libevent 底层相同
2. **libevent 优势**: 成熟的缓冲管理、更好的高并发处理
3. **Chase 优势**: 更简洁的 API、更小的二进制体积
4. **改进方向**: 参考 libevent 的 bufferevent 设计优化连接管理

---

## 10. 后续工作

1. [ ] 完成基准测试并填写报告
2. [ ] 分析性能差异原因
3. [ ] 评估是否需要优化 Chase EventLoop
4. [ ] 更新文档说明测试结果
5. [ ] 考虑添加更多对比维度（SSL、WebSocket 等）