# Chase HTTP Server 版本迁移指南

本文档提供 Chase HTTP Server 版本升级和迁移的详细指南，帮助用户平滑升级并保持兼容性。

---

## 1. 版本历史

### 1.1 v0.1.0 当前版本功能概述

**发布日期**: 2026-04-23

v0.1.0 是 Chase HTTP Server 的首个稳定版本，已完成 Phase 1-5 全部功能开发并通过验收测试。

#### 核心功能

| Phase | 功能模块 | 状态 | 说明 |
|-------|----------|------|------|
| Phase 1 | 核心框架 | ✅ | EventLoop、Timer、Buffer、Connection、HTTP Parser、Router |
| Phase 2 | 多进程架构 | ✅ | SO_REUSEPORT + Master/Worker 进程管理 |
| Phase 3 | HTTP/1.1 完整特性 | ✅ | Keep-Alive、Chunked 编码、Range 请求 |
| Phase 4 | SSL/TLS + 虚拟主机 | ✅ | OpenSSL 1.1.1/3.x 兼容、虚拟主机匹配 |
| Phase 5 | 安全防护 + 日志系统 | ✅ | Security、Logger、gzip、正则路由、热配置 |

#### 性能指标

| 指标 | 目标值 | 实际值 | 备注 |
|------|--------|--------|------|
| 单 Worker 吞吐量 | 2,000 req/s | **33,245 req/s** | 超标 16x |
| 多 Worker 吞吐量 | 5,000 req/s | **30,856 req/s** | 超标 6x |
| P50 延迟 | < 10ms | **112 μs** | 极低延迟 |
| Worker 崩溃恢复 | < 3s | < 1s | 快速恢复 |

#### API 模块列表

```
Phase 1 核心 API (14 个模块):
  - eventloop.h    事件循环 (epoll/kqueue/poll)
  - timer.h        定时器最小堆
  - buffer.h       环形缓冲区
  - connection.h   连接管理
  - connection_pool.h  连接池
  - http_parser.h  HTTP/1.1 解析器
  - router.h       URL 路由匹配
  - handler.h      预置处理器
  - response.h     HTTP 响应构建
  - mime.h         MIME 类型推断
  - fileserve.h    静态文件服务
  - server.h       服务器封装层
  - socket.h       Socket 创建
  - error.h        错误码定义

Phase 2 多进程 API (2 个模块):
  - master.h       Master 进程管理
  - worker.h       Worker 进程生命周期

Phase 4 SSL/vhost API (3 个模块):
  - ssl_wrap.h     SSL/TLS 包装
  - vhost.h        虚拟主机管理
  - config.h       JSON 配置加载

Phase 5 安全/日志 API (2 个模块):
  - security.h     DDoS 防护
  - logger.h       异步日志系统
```

### 1.2 未来版本规划

| 版本 | 状态 | 目标 | 预计时间 | 主要变更 |
|------|------|------|----------|----------|
| v0.2.0 | 规划中 | Phase 6: 文档完善 + 测试覆盖率 | TBD | API 稳定化，测试覆盖 ≥ 80% |
| v0.3.0 | 待规划 | Phase 7: C++ API 封装层 | TBD | C++ 封装，RAII 资源管理 |
| v1.0.0 | 待规划 | 生产级发布 | TBD | API 锁定，完整文档 |

#### v0.2.0 规划特性

- 测试覆盖率提升至 80%+
- 压力测试验证（24h 运行）
- 内存泄漏检测（Valgrind 长跑）
- API 文档完善（Doxygen）
- 用户指南和示例代码

#### v0.3.0 规划特性

- C++ API 封装层
- RAII 资源管理
- 异常安全保证
- STL 容器集成
- Lambda 表达式路由注册

---

## 2. API 变更记录模板

### 2.1 API 添加记录格式

```markdown
### [版本号] 新增 API

#### 新增结构体

**结构体名**: `StructName`
**头文件**: `include/module.h`
**用途**: 简要说明功能

```c
typedef struct StructName {
    int field1;          // 字段说明
    char *field2;        // 字段说明
    // ...
} StructName;
```

**使用示例**:
```c
StructName *obj = structname_create();
// 使用示例代码
structname_destroy(obj);
```

---

#### 新增函数

**函数名**: `function_name`
**头文件**: `include/module.h`
**用途**: 简要说明功能

**函数签名**:
```c
ReturnType function_name(ParamType1 param1, ParamType2 param2);
```

**参数说明**:
| 参数 | 类型 | 说明 |
|------|------|------|
| param1 | ParamType1 | 参数说明 |
| param2 | ParamType2 | 参数说明 |

**返回值**:
- 成功: 返回值说明
- 失败: 返回值说明（含错误码）

**使用示例**:
```c
ReturnType result = function_name(param1, param2);
if (result < 0) {
    // 错误处理
}
```
```

### 2.2 API 修改记录格式

```markdown
### [版本号] API 修改

#### 修改函数

**函数名**: `function_name`
**头文件**: `include/module.h`
**变更类型**: 签名变更 / 行为变更 / 默认值变更

**旧版签名** (v0.X.X):
```c
ReturnType old_function_name(OldParamType param);
```

**新版签名** (v0.Y.Y):
```c
ReturnType new_function_name(NewParamType param, ExtraParam extra);
```

**变更说明**:
1. 参数类型变更: `OldParamType` → `NewParamType`
2. 新增参数: `extra` 用于扩展功能
3. 返回值行为变更: 说明差异

**迁移步骤**:
1. 更新函数调用，添加新参数
2. 处理新的返回值逻辑
3. 更新依赖代码

**兼容性**:
- [ ] 源码兼容: 需修改调用代码
- [ ] 二进制兼容: 需重新编译
- [ ] 配置兼容: 无影响

**迁移示例**:
```c
// v0.X.X 旧代码
ReturnType result = old_function_name(param);

// v0.Y.Y 新代码
ReturnType result = new_function_name(param, extra);
```
```

### 2.3 API 废弃记录格式

```markdown
### [版本号] API 废弃

#### 废弃函数

**函数名**: `deprecated_function`
**头文件**: `include/module.h`
**废弃版本**: v0.X.X
**移除版本**: v1.0.0（计划）
**替代方案**: `new_function`

**废弃说明**:
该函数因 [原因] 被废弃，将在 v1.0.0 版本移除。

**迁移步骤**:
1. 将 `deprecated_function()` 替换为 `new_function()`
2. 更新相关代码适配新 API
3. 测试验证功能一致性

**废弃警告**:
```c
// 编译时警告
#pragma message("deprecated_function is deprecated, use new_function")
```

**迁移示例**:
```c
// 废弃代码
int result = deprecated_function(old_param);

// 替代代码
int result = new_function(new_param);
```

---

#### 废弃宏定义

**宏名**: `DEPRECATED_MACRO`
**头文件**: `include/module.h`
**废弃版本**: v0.X.X
**移除版本**: v1.0.0（计划）
**替代方案**: `NEW_MACRO` 或函数调用

**废弃说明**:
该宏因 [原因] 被废弃。

**迁移示例**:
```c
// 废弃代码
int value = DEPRECATED_MACRO(x);

// 替代代码
int value = NEW_MACRO(x);
// 或使用函数
int value = new_function(x);
```
```

---

## 3. 配置格式变更说明

### 3.1 JSON 配置格式版本管理

Chase 使用 JSON 配置文件，配置格式通过版本字段管理。

#### 配置版本字段

```json
{
  "config_version": "0.1.0",
  "server": {
    "port": 8080,
    "workers": 4,
    "max_connections": 1024
  }
}
```

#### 版本兼容性矩阵

| 配置版本 | 兼容服务器版本 | 变更说明 |
|----------|----------------|----------|
| 0.1.0 | v0.1.0 - v0.2.x | 基础配置格式 |
| 0.2.0 | v0.2.0+ | 新增热更新字段 |
| 0.3.0 | v0.3.0+ | 新增 C++ 配置选项 |

#### 版本校验机制

```c
// config.h 提供版本校验 API
HttpConfig *http_config_load_from_file(const char *file_path, 
                                        const ConfigLoadOptions *options);

// 加载时自动校验版本
ConfigLoadOptions options = {
    .config_file = "/etc/chase/server.json",
    .validate_required = true,
    .load_defaults = true
};

HttpConfig *config = http_config_load_from_file(file_path, &options);
if (!config) {
    int error = http_config_get_last_error();
    // CONFIG_ERR_VERSION_MISMATCH 等
}
```

### 3.2 配置迁移示例

#### v0.1.0 基础配置

```json
{
  "config_version": "0.1.0",
  "server": {
    "port": 8080,
    "bind_address": "0.0.0.0",
    "max_connections": 1024,
    "backlog": 128,
    "reuseport": true
  },
  "keepalive": {
    "timeout_ms": 5000,
    "max_requests": 100
  },
  "ssl": {
    "enabled": false,
    "cert_file": null,
    "key_file": null
  },
  "vhosts": []
}
```

#### v0.2.0 扩展配置（计划）

```json
{
  "config_version": "0.2.0",
  "server": {
    "port": 8080,
    "bind_address": "0.0.0.0",
    "max_connections": 1024,
    "backlog": 128,
    "reuseport": true
  },
  "keepalive": {
    "timeout_ms": 5000,
    "max_requests": 100
  },
  "ssl": {
    "enabled": false,
    "cert_file": null,
    "key_file": null,
    "protocols": ["TLS1.2", "TLS1.3"],    // 新增: TLS 协议版本
    "ciphers": "ECDHE+AESGCM"             // 新增: 加密套件
  },
  "vhosts": [],
  "security": {                            // 新增: 安全配置
    "enabled": true,
    "max_connections_per_ip": 10,
    "rate_limit_per_ip": 100,
    "block_duration_ms": 60000
  },
  "logging": {                             // 新增: 日志配置
    "level": "INFO",
    "file": "/var/log/chase/server.log",
    "format": "text"
  },
  "hot_update": {                          // 新增: 热更新配置
    "enabled": true,
    "policy": "gradual",
    "watch_file": true
  }
}
```

#### 配置迁移脚本示例

```bash
#!/bin/bash
# migrate_config.sh - 配置迁移脚本

OLD_CONFIG="/etc/chase/server.json.v0.1"
NEW_CONFIG="/etc/chase/server.json"

# 备份旧配置
cp "$OLD_CONFIG" "${OLD_CONFIG}.bak"

# 使用 jq 迁移配置
jq '
  .config_version = "0.2.0" |
  .ssl += {
    "protocols": ["TLS1.2", "TLS1.3"],
    "ciphers": "ECDHE+AESGCM"
  } |
  .security = {
    "enabled": true,
    "max_connections_per_ip": 10,
    "rate_limit_per_ip": 100,
    "block_duration_ms": 60000
  } |
  .logging = {
    "level": "INFO",
    "file": "/var/log/chase/server.log",
    "format": "text"
  } |
  .hot_update = {
    "enabled": true,
    "policy": "gradual",
    "watch_file": true
  }
' "$OLD_CONFIG" > "$NEW_CONFIG"

# 验证新配置
./chase_server --config "$NEW_CONFIG" --validate
```

---

## 4. 兼容性说明

### 4.1 OpenSSL 版本兼容性

Chase 支持 OpenSSL 1.1.1 和 OpenSSL 3.x 两个主要版本。

#### OpenSSL 1.1.1 支持

```c
// ssl_wrap.h 提供兼容层
#if OPENSSL_VERSION_NUMBER < 0x30000000L
    // OpenSSL 1.1.1 API
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
#else
    // OpenSSL 3.x API
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
#endif
```

#### OpenSSL 3.x 支持

OpenSSL 3.x 引入了新的 API 和废弃警告，Chase 已适配：

| OpenSSL 3.x 变更 | Chase 适配 |
|------------------|------------|
| `SSL_CTX_set_security_level` | 新增支持 |
| `EVP_MD_fetch` 替代 `EVP_sha256` | 自动选择 |
| 废弃 `SSL_CTX_set_tmp_rsa_callback` | 已移除 |
| 废弃低强度加密套件 | 默认禁用 |

#### 推荐 OpenSSL 版本

| 使用场景 | 推荐 OpenSSL | 原因 |
|----------|--------------|------|
| 生产环境 | OpenSSL 3.0+ | 安全更新、新特性 |
| 开发环境 | OpenSSL 1.1.1 | 广泛支持、稳定 |
| macOS 系统 | LibreSSL 2.x | 系统自带 |

#### 版本检测

```c
#include <openssl/opensslv.h>

// 运行时检测 OpenSSL 版本
void check_openssl_version(void) {
    unsigned long version = OPENSSL_VERSION_NUMBER;
    
    if (version >= 0x30000000L) {
        printf("OpenSSL 3.x detected: %s\n", OpenSSL_version(OPENSSL_VERSION));
    } else if (version >= 0x10101000L) {
        printf("OpenSSL 1.1.1 detected: %s\n", OpenSSL_version(OPENSSL_VERSION));
    } else {
        printf("Warning: OpenSSL version too old: %s\n", OpenSSL_version(OPENSSL_VERSION));
    }
}
```

### 4.2 macOS / Linux 平台兼容性

#### 平台特性对比

| 特性 | Linux | macOS | Chase 支持 |
|------|-------|-------|------------|
| SO_REUSEPORT | 内核 3.9+ | 全版本 | ✅ 跨平台 |
| epoll | ✅ 原生 | ❌ | ✅ kqueue 替代 |
| kqueue | ❌ | ✅ 原生 | ✅ 原生支持 |
| eventfd | ✅ 原生 | ❌ | ✅ pipe 替代 |
| timerfd | ✅ 原生 | ❌ | ✅ kqueue EVFILT_TIMER |
| sendfile | ✅ 原生 | ✅ 不同 API | ✅ 平台抽象 |

#### EventLoop 平台抽象

```c
// eventloop.c 自动选择最佳后端

#if defined(__linux__)
    // 使用 epoll
    #define EVENTLOOP_BACKEND_EPOLL
#elif defined(__APPLE__) || defined(__BSD__)
    // 使用 kqueue
    #define EVENTLOOP_BACKEND_KQUEUE
#else
    // 使用 poll (fallback)
    #define EVENTLOOP_BACKEND_POLL
#endif

EventLoop *eventloop_create(int max_events) {
    EventLoop *loop = malloc(sizeof(EventLoop));
    
#ifdef EVENTLOOP_BACKEND_EPOLL
    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
#elif defined(EVENTLOOP_BACKEND_KQUEUE)
    loop->kqueue_fd = kqueue();
#else
    // poll fallback
#endif
    
    return loop;
}
```

#### 平台特定配置

| 配置项 | Linux 默认值 | macOS 默认值 | 说明 |
|--------|--------------|--------------|------|
| max_events | 1024 | 1024 | 每循环最大事件数 |
| backlog | SOMAXCONN (128) | SOMAXCONN (128) | listen backlog |
| worker_count | CPU 核数 | CPU 核数 | Worker 进程数 |

#### macOS 特殊说明

1. **系统 OpenSSL**: macOS 使用 LibreSSL，可能与 OpenSSL 有 API 差异
2. **推荐安装**: 使用 vcpkg 或 Homebrew 安装 OpenSSL
   ```bash
   # Homebrew 安装
   brew install openssl
   
   # vcpkg 安装
   vcpkg install openssl
   ```
3. **编译指定 OpenSSL**:
   ```bash
   cmake -B build -DOPENSSL_ROOT_DIR=/usr/local/opt/openssl
   ```

### 4.3 C11 标准要求

Chase 要求 C11 编译器支持，使用以下 C11 特性：

#### C11 特性使用列表

| 特性 | 使用位置 | 说明 |
|------|----------|------|
| `_Generic` | error.c | 类型泛型错误处理 |
| `static_assert` | config.c | 编译时断言检查 |
| `alignas` | buffer.c | 内存对齐优化 |
| `thread_local` | logger.c | 线程局部存储 |
| `<stdatomic.h>` | security.c | 原子操作 |

#### 编译器兼容性

| 编译器 | 版本要求 | C11 支持 |
|--------|----------|----------|
| GCC | 4.9+ | ✅ 完全支持 |
| Clang | 3.6+ | ✅ 完全支持 |
| MSVC | 2015+ | ⚠️ 部分支持 |

#### CMake 配置

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.19)

# 设置 C11 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)  # 禁用 GNU 扩展，严格 C11

# 编译器检查
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # GCC/Clang: 完全支持 C11
    add_compile_options(-Wall -Wextra -Wpedantic)
elseif(CMAKE_C_COMPILER_ID MATCHES "MSVC")
    # MSVC: 部分支持，可能需要调整
    message(WARNING "MSVC may have limited C11 support")
endif()
```

#### C11 特性检测

```c
// 编译时特性检测
#if __STDC_VERSION__ >= 201112L
    #define HAS_C11_ATOMICS 1
    #include <stdatomic.h>
#else
    #define HAS_C11_ATOMICS 0
    // 使用 pthread mutex 替代
#endif

#if defined(__clang__) || defined(__GNUC__)
    #define HAS_GENERIC 1
#else
    #define HAS_GENERIC 0
#endif
```

---

## 5. 升级步骤

### 5.1 从 v0.1 升级到 v0.2 步骤模板

#### 升级前准备

1. **备份当前配置**
   ```bash
   cp /etc/chase/server.json /etc/chase/server.json.v0.1.bak
   cp -r /var/log/chase /var/log/chase.v0.1.bak
   ```

2. **检查 API 依赖**
   ```bash
   # 扫描代码中使用的 Chase API
   grep -r "chase_" --include="*.c" --include="*.h" src/
   ```

3. **查看变更日志**
   ```bash
   git log v0.1.0..v0.2.0 --oneline
   ```

#### 升级步骤

```
步骤 1: 下载新版本源码
────────────────────────────────────────
git fetch origin
git checkout v0.2.0

步骤 2: 更新依赖
────────────────────────────────────────
vcpkg upgrade
vcpkg install openssl zlib

步骤 3: 编译新版本
────────────────────────────────────────
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target install

步骤 4: 迁移配置文件
────────────────────────────────────────
# 使用迁移脚本
./scripts/migrate_config.sh \
    --old /etc/chase/server.json.v0.1 \
    --new /etc/chase/server.json

# 或手动迁移
# 参考 "配置格式变更说明" 章节

步骤 5: 验证新配置
────────────────────────────────────────
./build/chase_server --config /etc/chase/server.json --validate

步骤 6: 停止旧服务
────────────────────────────────────────
systemctl stop chase-server
# 或
kill -TERM $(cat /var/run/chase.pid)

步骤 7: 启动新服务
────────────────────────────────────────
systemctl start chase-server
# 或
./build/chase_server --config /etc/chase/server.json

步骤 8: 验证服务状态
────────────────────────────────────────
curl -I http://localhost:8080/
systemctl status chase-server

步骤 9: 监控运行状态
────────────────────────────────────────
tail -f /var/log/chase/server.log
# 观察是否有异常日志

步骤 10: 清理备份（确认稳定后）
────────────────────────────────────────
# 运行稳定一周后清理备份
rm /etc/chase/server.json.v0.1.bak
```

#### 回滚步骤（升级失败）

```bash
# 1. 停止新服务
systemctl stop chase-server

# 2. 回滚代码版本
git checkout v0.1.0
cmake --build build --target install

# 3. 恢复配置
cp /etc/chase/server.json.v0.1.bak /etc/chase/server.json

# 4. 启动旧服务
systemctl start chase-server

# 5. 分析失败原因
grep ERROR /var/log/chase/server.log
```

### 5.2 数据迁移说明

#### 无状态服务（推荐）

Chase HTTP Server 设计为无状态服务，无需数据迁移：

- 不持久化连接状态
- 不存储用户数据
- 配置文件仅包含启动参数

#### 有状态场景迁移

如果使用以下功能，需注意迁移细节：

| 功能 | 迁移注意事项 |
|------|--------------|
| SSL 会话缓存 | 无需迁移，重新建立即可 |
| Security IP 封禁记录 | 建议清空，新版本重新检测 |
| 日志文件 | 可保留，格式兼容 |

#### SSL 证书迁移

```bash
# SSL 证书无需迁移，直接使用
# 确保证书文件路径在配置中正确
scp /etc/ssl/chase/server.crt new-server:/etc/ssl/chase/
scp /etc/ssl/chase/server.key new-server:/etc/ssl/chase/
```

### 5.3 配置迁移说明

#### 自动迁移工具

Chase 提供配置迁移脚本 `scripts/migrate_config.sh`：

```bash
#!/bin/bash
# 使用示例

# 基础迁移
./scripts/migrate_config.sh \
    --input /etc/chase/server.json.old \
    --output /etc/chase/server.json.new \
    --version 0.2.0

# 交互式迁移（提示每个变更）
./scripts/migrate_config.sh \
    --interactive \
    --input /etc/chase/server.json.old

# 验证迁移结果
./scripts/migrate_config.sh \
    --validate /etc/chase/server.json.new
```

#### 手动迁移检查清单

```markdown
配置迁移检查清单:

□ 1. 检查 config_version 字段是否更新
□ 2. 新增字段是否设置默认值
□ 3. 废弃字段是否已移除
□ 4. SSL 配置是否新增 protocols/ciphers
□ 5. Security 配置是否新增（如有）
□ 6. Logging 配置是否新增（如有）
□ 7. 热更新配置是否新增（如有）
□ 8. 使用 --validate 选项验证配置
□ 9. 启动服务检查配置加载是否成功
□ 10. 运行功能测试验证配置生效
```

#### 配置验证命令

```bash
# 验证配置文件语法
./chase_server --config /etc/chase/server.json --validate

# 输出配置解析结果
./chase_server --config /etc/chase/server.json --dump-config

# 检查 SSL 配置
./chase_server --config /etc/chase/server.json --check-ssl
```

---

## 6. 最佳实践建议

### 6.1 版本选择建议

#### 生产环境版本选择

| 场景 | 推荐版本 | 原因 |
|------|----------|------|
| 新项目 | v0.1.0 (最新稳定版) | 功能完整，性能优异 |
| 高可用要求 | v0.1.0 | Worker 崩溃恢复机制 |
| SSL/TLS 需求 | v0.1.0 | OpenSSL 1.1.1/3.x 兼容 |
| 开发/测试 | v0.1.0 或开发版 | 快速迭代 |

#### 版本升级时机

| 时机 | 建议 |
|------|------|
| 大版本升级 (v0.x → v1.x) | 等待稳定期（建议 1-2 周） |
| 小版本升级 (v0.1 → v0.2) | 尽快升级，获取新功能和修复 |
| 补丁版本 (v0.1.0 → v0.1.1) | 立即升级，仅含 Bug 修复 |
| API 变更版本 | 评估影响后升级 |

#### 版本固定策略

```bash
# 生产环境固定版本
git checkout v0.1.0

# 或使用 vcpkg 固定版本
vcpkg install chase-http-server@0.1.0
```

### 6.2 兼容性测试建议

#### 升级前测试流程

```
┌─────────────────────────────────────────────────────────────────┐
│                     兼容性测试流程                               │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  1. 编译测试                                                    │
│     ├── 目标平台编译成功                                        │
│     ├── 无编译警告                                              │
│     └── 依赖库版本正确                                          │
│                                                                 │
│  2. 单元测试                                                    │
│     ├── 运行全部测试套件                                        │
│     ├── 覆盖率 ≥ 70%                                            │
│     └── 无新增失败用例                                          │
│                                                                 │
│  3. 功能测试                                                    │
│     ├── HTTP GET/POST 基础功能                                  │
│     ├── Keep-Alive 连接                                         │
│     ├── SSL/TLS 握手                                            │
│     ├── 虚拟主机匹配                                            │
│     ├── 静态文件服务                                            │
│     └── 安全防护功能                                            │
│                                                                 │
│  4. 性能测试                                                    │
│     ├── wrk 基准测试                                            │
│     ├── 延迟分布检查                                            │
│     └── 对比旧版本性能                                          │
│                                                                 │
│  5. 压力测试                                                    │
│     ├── 高并发连接                                              │
│     ├── 长时间运行（≥ 1h）                                      │
│     ├── Worker 崩溃恢复                                         │
│     └── 内存泄漏检测                                            │
│                                                                 │
│  6. 回归测试                                                    │
│     ├── API 行为一致性                                          │
│     ├── 配置兼容性                                              │
│     └── 日志格式一致性                                          │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

#### 测试命令参考

```bash
# 1. 编译测试
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# 检查编译警告数
cmake --build build 2>&1 | grep -c "warning"

# 2. 单元测试
cd build && ctest --output-on-failure

# 3. 功能测试
curl -X GET http://localhost:8080/
curl -X POST -d "data" http://localhost:8080/api
curl -k https://localhost:8443/  # SSL 测试

# 4. 性能测试
wrk -t4 -c100 -d30s http://localhost:8080/

# 5. 压力测试
# 长时间运行
./chase_server --config /etc/chase/server.json &
sleep 3600  # 运行 1 小时
curl http://localhost:8080/health

# Worker 崩溃恢复测试
kill -9 $(pgrep -f "chase_worker")
sleep 2
curl http://localhost:8080/  # 应正常响应

# 6. 内存泄漏检测
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         ./chase_server --config /etc/chase/server.json
```

#### 兼容性测试矩阵

| 测试项 | Linux x86_64 | Linux ARM64 | macOS x86_64 | macOS ARM64 |
|--------|--------------|-------------|--------------|-------------|
| 编译 | ✅ | ✅ | ✅ | ✅ |
| 单元测试 | ✅ | ✅ | ✅ | ✅ |
| OpenSSL 1.1.1 | ✅ | ✅ | ✅ | ✅ |
| OpenSSL 3.x | ✅ | ✅ | ⚠️ (LibreSSL) | ⚠️ |
| 性能基准 | ✅ | ✅ | ✅ | ✅ |
| Worker 崩溃恢复 | ✅ | ✅ | ✅ | ✅ |

---

## 附录

### A. 错误码迁移对照

| v0.1 错误码 | v0.2 错误码（计划） | 说明 |
|-------------|---------------------|------|
| CONFIG_ERR_INVALID_PORT | CONFIG_ERR_INVALID_PORT | 无变更 |
| CONFIG_ERR_MISSING_CERT | CONFIG_ERR_SSL_MISSING_CERT | 更精确命名 |
| - | CONFIG_ERR_VERSION_MISMATCH | 新增 |

### B. 性能基准迁移对照

| 性能指标 | v0.1.0 | v0.2.0（目标） | 变化 |
|----------|--------|----------------|------|
| 单 Worker 吞吐 | 33k req/s | ≥ 35k req/s | 提升 |
| 多 Worker 吞吐 | 31k req/s | ≥ 35k req/s | 提升 |
| P50 延迟 | 112 μs | < 100 μs | 降低 |
| 内存占用 | TBD | ≤ 50MB/Worker | 优化 |

### C. 相关文档链接

- [README.md](../README.md) - 项目概览
- [architecture.md](superpowers/architecture/architecture.md) - 架构设计
- [implementation-plan.md](superpowers/plans/2026-04-15-http-server-implementation-plan.md) - 实施计划
- [test_report.md](../test/report/test_verification_report_2026-04-21.md) - 测试报告

---

**文档版本**: 1.0
**更新日期**: 2026-04-24
**作者**: Chase Team