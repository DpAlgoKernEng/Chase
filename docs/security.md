# Security 模块配置指南

## 1. 模块概述

Chase 的 Security 模块提供多层次的安全防护机制，专为高并发 Web 服务器设计。该模块采用 Worker 级实例模式，无跨 Worker 共享状态，通过分片哈希表实现高效并发处理。

### 1.1 核心功能

| 功能 | 描述 | 防护场景 |
|------|------|----------|
| **DDoS 防护** | 单 IP 连接数限制 | 防止大量连接占用资源 |
| **速率限制** | 请求数/秒限制 | 防止 API滥用、暴力攻击 |
| **Slowloris 检测** | 最小字节速率检测 | 防止慢速连接攻击 |
| **IP 封禁** | 自动/手动封禁机制 | 惩罚恶意行为 |

### 1.2 防护流程

```
客户端连接
    |
    v
[检查封禁状态] --> 已封禁 --> 拒绝连接 (SECURITY_BLOCKED_IP)
    |
    未封禁
    v
[检查连接数限制] --> 超限 --> 拒绝连接 (SECURITY_TOO_MANY_CONN)
    |
    未超限
    v
[记录连接] --> 允许连接
    |
    v
[请求速率检查] --> 超速 --> 自动封禁 (SECURITY_RATE_LIMITED)
    |
    正常
    v
[Slowloris 检测] --> 检测到 --> 自动封禁 (SECURITY_SLOWLORIS)
    |
    正常
    v
处理请求
```

### 1.3 返回状态码

```c
typedef enum {
    SECURITY_OK,                /* 允许操作 */
    SECURITY_BLOCKED_IP,        /* IP 已被封禁 */
    SECURITY_RATE_LIMITED,      /* 超过速率限制 */
    SECURITY_TOO_MANY_CONN,     /* 连接数过多 */
    SECURITY_SLOWLORIS,         /* Slowloris 攻击检测 */
    SECURITY_INTERNAL_ERROR     /* 内部错误 */
} SecurityResult;
```

---

## 2. 配置参数说明

### 2.1 SecurityConfig 结构

```c
typedef struct SecurityConfig {
    int max_connections_per_ip;     /* 单 IP 最大并发连接数 */
    int max_requests_per_second;    /* 单 IP 每秒最大请求数 */
    int min_request_rate;           /* Slowloris 最小字节/秒 */
    int slowloris_timeout_ms;       /* Slowloris 检测超时 */
    int block_duration_ms;          /* IP 封禁持续时间 */
    int shard_count;                /* 分片哈希表分片数 */
} SecurityConfig;
```

### 2.2 参数详解

#### max_connections_per_ip

**用途**: 限制单个 IP 地址的最大并发连接数，防止 DDoS 攻击占用服务器资源。

**默认值**: `10`

**推荐范围**:
- 开发/测试环境: `50-100`
- 生产环境（低流量）: `20-30`
- 生产环境（高流量）: `10-20`

**注意事项**:
- NAT 网络环境下多个用户共享同一 IP，需要适当提高此值
- 与 `worker_count` 配合，实际限制为 `max_connections_per_ip * worker_count`

#### max_requests_per_second

**用途**: 限制单个 IP 每秒的最大请求数，防止 API 滥用和暴力攻击。

**默认值**: `100`

**推荐范围**:
- API 服务: `50-200`
- 静态资源服务: `200-500`
- 高安全场景（登录接口）: `10-20`

**触发行为**: 超过限制后自动封禁该 IP，封禁时长由 `block_duration_ms` 决定。

#### min_request_rate

**用途**: Slowloris 检测的最小字节速率阈值。如果客户端在检测窗口内发送数据的速率低于此值，视为攻击。

**默认值**: `50` (bytes/sec)

**推荐范围**:
- 标准场景: `50-100`
- 低带宽客户端支持: `20-30`

**计算方式**:
```
bytes_per_sec = bytes_received * 1000 / elapsed_ms
```

#### slowloris_timeout_ms

**用途**: Slowloris 检测的时间窗口，客户端在此窗口内数据速率过低触发检测。

**默认值**: `30000` (30 秒)

**推荐范围**:
- 标准场景: `30000-60000`
- 高安全要求: `15000-30000`

**注意**: 值过小可能导致正常慢速客户端误报。

#### block_duration_ms

**用途**: IP 被封禁后的持续时间，超时后自动解封。

**默认值**: `60000` (60 秒)

**推荐范围**:
- 自动封禁: `60000-300000` (1-5 分钟)
- 手动封禁: 可指定任意时长，默认使用此配置值

**注意**: 生产环境建议根据攻击类型动态调整：
- 速率限制触发: 较短封禁 (1-5 分钟)
- Slowloris 检测: 中等封禁 (5-30 分钟)
- 手动封禁恶意 IP: 较长封禁 (数小时至永久)

#### shard_count

**用途**: 分片哈希表的分片数量，用于减少多线程环境下的锁竞争。

**默认值**: `16`

**推荐范围**:
- 低并发（单 Worker）: `4-8`
- 中等并发（2-4 Workers）: `8-16`
- 高并发（8+ Workers）: `16-32`

**技术细节**:
- 每个分片独立维护一个哈希表和互斥锁
- IP 地址通过哈希函数映射到特定分片
- 分片数应与 Worker 数量相近或略高以平衡锁竞争

---

## 3. 使用示例代码

### 3.1 基础配置示例

```c
#include "security.h"
#include "logger.h"

/* 创建默认配置的 Security 实例 */
Security *create_default_security(void) {
    SecurityConfig config = {
        .max_connections_per_ip = SECURITY_DEFAULT_MAX_CONN_PER_IP,
        .max_requests_per_second = SECURITY_DEFAULT_MAX_REQ_PER_SEC,
        .min_request_rate = SECURITY_DEFAULT_MIN_REQ_RATE,
        .slowloris_timeout_ms = SECURITY_DEFAULT_SLOWLORIS_TIMEOUT,
        .block_duration_ms = SECURITY_DEFAULT_BLOCK_DURATION,
        .shard_count = SECURITY_DEFAULT_SHARD_COUNT
    };
    
    return security_create(&config);
}

/* 处理新连接 */
void handle_new_connection(Security *security, Logger *logger,
                           struct sockaddr *client_addr) {
    IpAddress ip;
    
    /* 解析客户端 IP */
    if (security_parse_ip(client_addr, &ip) != 0) {
        logger_log(logger, LOG_ERROR, "Failed to parse client IP");
        return;
    }
    
    /* 检查连接权限 */
    SecurityResult result = security_check_connection(security, &ip);
    
    char ip_str[64];
    security_ip_to_string(&ip, ip_str, sizeof(ip_str));
    
    switch (result) {
        case SECURITY_OK:
            security_add_connection(security, &ip);
            logger_log(logger, LOG_INFO, "Connection accepted from %s", ip_str);
            break;
            
        case SECURITY_BLOCKED_IP:
            logger_log_security(logger, &(SecurityLogContext){
                .event_type = "blocked_connection",
                .client_ip = ip_str,
                .details = "IP is currently blocked",
                .severity = 3,
                .blocked = true
            });
            /* 关闭连接 */
            break;
            
        case SECURITY_TOO_MANY_CONN:
            logger_log_rate_limit(logger, ip_str, "connections",
                                  /* current */, /* limit */);
            /* 关闭连接 */
            break;
            
        default:
            logger_log(logger, LOG_ERROR, "Security check failed for %s", ip_str);
            break;
    }
}
```

### 3.2 高流量场景配置

```c
#include "security.h"

/* 高流量 Web 服务配置 */
Security *create_high_traffic_security(void) {
    SecurityConfig config = {
        /* 允许更多并发连接（考虑 NAT 环境） */
        .max_connections_per_ip = 30,
        
        /* 较宽松的请求速率限制 */
        .max_requests_per_second = 200,
        
        /* 标准 Slowloris 检测 */
        .min_request_rate = 50,
        .slowloris_timeout_ms = 30000,
        
        /* 较短封禁时长，避免误封影响正常用户 */
        .block_duration_ms = 30000,  /* 30 秒 */
        
        /* 高并发场景：增加分片数减少锁竞争 */
        .shard_count = 32
    };
    
    return security_create(&config);
}

/* API 服务配置（更严格的速率限制） */
Security *create_api_security(void) {
    SecurityConfig config = {
        .max_connections_per_ip = 10,
        .max_requests_per_second = 50,   /* 严格的 API 速率限制 */
        .min_request_rate = 30,
        .slowloris_timeout_ms = 20000,
        .block_duration_ms = 120000,     /* 2 分钟封禁 */
        .shard_count = 16
    };
    
    return security_create(&config);
}

/* 管理后台配置（极高安全） */
Security *create_admin_security(void) {
    SecurityConfig config = {
        .max_connections_per_ip = 5,     /* 严格限制 */
        .max_requests_per_second = 20,
        .min_request_rate = 100,
        .slowloris_timeout_ms = 15000,
        .block_duration_ms = 3600000,    /* 1 小时封禁 */
        .shard_count = 4
    };
    
    return security_create(&config);
}
```

### 3.3 安全审计日志集成

```c
#include "security.h"
#include "logger.h"

/* 完整的安全审计处理流程 */
typedef struct {
    Security *security;
    Logger *logger;
} SecurityAuditContext;

/* 处理请求并记录审计日志 */
SecurityResult audit_request(SecurityAuditContext *ctx,
                             const IpAddress *ip,
                             size_t bytes_received) {
    char ip_str[64];
    security_ip_to_string(ip, ip_str, sizeof(ip_str));
    
    /* 检查请求速率 */
    SecurityResult result = security_check_request_rate(ctx->security, ip, bytes_received);
    
    switch (result) {
        case SECURITY_OK:
            /* 正常请求，可选记录 */
            break;
            
        case SECURITY_BLOCKED_IP:
            logger_log_security(ctx->logger, &(SecurityLogContext){
                .event_type = "blocked_request",
                .client_ip = ip_str,
                .details = "Request from blocked IP",
                .severity = 4,
                .blocked = true
            });
            break;
            
        case SECURITY_RATE_LIMITED:
            /* 获取当前统计 */
            IpStats stats;
            if (security_get_ip_stats(ctx->security, ip, &stats) == 0) {
                logger_log_rate_limit(ctx->logger, ip_str, "requests",
                                      stats.request_count,
                                      ctx->security->config.max_requests_per_second);
            }
            
            logger_log_security(ctx->logger, &(SecurityLogContext){
                .event_type = "rate_limit_triggered",
                .client_ip = ip_str,
                .details = "Auto-blocked due to excessive requests",
                .severity = 3,
                .blocked = true
            });
            break;
            
        case SECURITY_SLOWLORIS:
            logger_log_security(ctx->logger, &(SecurityLogContext){
                .event_type = "slowloris_detected",
                .client_ip = ip_str,
                .details = "Slow request rate detected",
                .severity = 4,
                .blocked = true
            });
            break;
            
        default:
            logger_log(ctx->logger, LOG_ERROR,
                       "Security check error for IP %s", ip_str);
            break;
    }
    
    return result;
}

/* 手动封禁并记录审计 */
void manual_block_with_audit(SecurityAuditContext *ctx,
                             const char *ip_str,
                             int duration_ms,
                             const char *reason) {
    IpAddress ip;
    if (security_string_to_ip(ip_str, &ip) != 0) {
        logger_log(ctx->logger, LOG_ERROR, "Invalid IP: %s", ip_str);
        return;
    }
    
    security_block_ip(ctx->security, &ip, duration_ms);
    
    logger_log_security(ctx->logger, &(SecurityLogContext){
        .event_type = "manual_block",
        .client_ip = ip_str,
        .details = reason,
        .severity = 5,
        .blocked = true
    });
}

/* 定期清理和统计报告 */
void security_maintenance(SecurityAuditContext *ctx) {
    /* 清理过期条目 */
    security_cleanup(ctx->security);
    
    /* 获取统计摘要 */
    int total_tracked, total_blocked;
    security_get_summary(ctx->security, &total_tracked, &total_blocked);
    
    logger_log(ctx->logger, LOG_INFO,
               "Security stats: %d IPs tracked, %d blocked",
               total_tracked, total_blocked);
}
```

---

## 4. 最佳实践建议

### 4.1 生产环境推荐配置

#### Web 服务（通用）

```c
SecurityConfig web_config = {
    .max_connections_per_ip = 20,
    .max_requests_per_second = 100,
    .min_request_rate = 50,
    .slowloris_timeout_ms = 30000,
    .block_duration_ms = 60000,
    .shard_count = 16
};
```

#### CDN/静态资源服务

```c
SecurityConfig cdn_config = {
    .max_connections_per_ip = 50,     /* 允许更多并发 */
    .max_requests_per_second = 500,   /* 高速率限制 */
    .min_request_rate = 30,           /* 较宽松 Slowloris */
    .slowloris_timeout_ms = 60000,
    .block_duration_ms = 30000,       /* 短封禁 */
    .shard_count = 32
};
```

#### API 服务

```c
SecurityConfig api_config = {
    .max_connections_per_ip = 10,
    .max_requests_per_second = 50,
    .min_request_rate = 100,
    .slowloris_timeout_ms = 20000,
    .block_duration_ms = 120000,
    .shard_count = 16
};
```

#### 内部服务（可信网络）

```c
SecurityConfig internal_config = {
    .max_connections_per_ip = 100,
    .max_requests_per_second = 1000,
    .min_request_rate = 10,
    .slowloris_timeout_ms = 120000,   /* 2 分钟 */
    .block_duration_ms = 10000,       /* 10 秒（快速恢复） */
    .shard_count = 8
};
```

### 4.2 监控和调优建议

#### 定期监控指标

```c
/* 每分钟执行一次统计收集 */
void collect_security_metrics(Security *security) {
    int tracked, blocked;
    security_get_summary(security, &tracked, &blocked);
    
    /* 记录到监控系统 */
    /* metrics_record("security.tracked_ips", tracked); */
    /* metrics_record("security.blocked_ips", blocked); */
    
    /* 警告阈值 */
    if (blocked > tracked * 0.1) {
        /* 超过 10% 的追踪 IP 被封禁，可能需要调查 */
    }
}
```

#### 调优检查清单

1. **连接数限制调整**:
   - 监控 `SECURITY_TOO_MANY_CONN` 拒绝率
   - 如果 NAT 网络用户多，提高 `max_connections_per_ip`
   - 如果资源紧张，降低此值

2. **速率限制调整**:
   - 监控 `SECURITY_RATE_LIMITED` 触发频率
   - 检查被封禁 IP 是否为正常用户
   - 根据业务特性调整 `max_requests_per_second`

3. **Slowloris 检测调整**:
   - 监控 `SECURITY_SLOWLORIS` 误报率
   - 检查是否影响移动端/低带宽用户
   - 调整 `min_request_rate` 和 `slowloris_timeout_ms`

4. **分片数调整**:
   - 监控锁竞争情况（如果可用）
   - 分片数 ≈ Worker 数 × 1.5 ~ 2
   - 过多分片增加内存开销，过少增加锁等待

### 4.3 故障排查

#### 封禁解除操作

```c
/* 紧急解除特定 IP 封禁 */
void emergency_unblock(Security *security, const char *ip_str) {
    IpAddress ip;
    security_string_to_ip(ip_str, &ip);
    security_unblock_ip(security, &ip);
}

/* 批量解除封禁（谨慎使用） */
void unblock_all(Security *security) {
    /* 通过 cleanup 强制清理 */
    security_cleanup(security);
}
```

#### 查看 IP 统计

```c
void inspect_ip_status(Security *security, const char *ip_str) {
    IpAddress ip;
    if (security_string_to_ip(ip_str, &ip) != 0) {
        printf("Invalid IP address\n");
        return;
    }
    
    IpStats stats;
    if (security_get_ip_stats(security, &ip, &stats) != 0) {
        printf("IP not tracked\n");
        return;
    }
    
    printf("IP: %s\n", ip_str);
    printf("  Connections: %d\n", stats.connection_count);
    printf("  Requests: %d\n", stats.request_count);
    printf("  Bytes received: %llu\n", stats.bytes_received);
    printf("  Blocked: %s\n", stats.is_blocked ? "YES" : "NO");
    if (stats.is_blocked) {
        printf("  Block expires: %llu ms\n", stats.block_expire_time);
    }
}
```

---

## 5. 与 Logger 模块集成

### 5.1 安全审计日志格式

#### TEXT 格式示例

```
[2026-04-23 14:30:15.123] [SECURITY] rate_limit_triggered | IP: 192.168.1.100 | requests: 105/100/sec | BLOCKED
[2026-04-23 14:30:45.456] [SECURITY] slowloris_detected | IP: 10.0.0.50 | rate: 15 bytes/sec | BLOCKED
[2026-04-23 14:31:00.789] [SECURITY] manual_block | IP: 203.0.113.42 | reason: known attacker | severity: 5
```

#### JSON 格式示例

```json
{
  "timestamp": "2026-04-23T14:30:15.123Z",
  "level": "SECURITY",
  "event_type": "rate_limit_triggered",
  "client_ip": "192.168.1.100",
  "details": "requests: 105/100/sec",
  "severity": 3,
  "blocked": true
}
```

### 5.2 Logger 配置建议

```c
/* 安全审计专用 Logger 配置 */
LoggerConfig security_logger_config = {
    .log_file = "/var/log/chase/security.log",
    .audit_file = "/var/log/chase/audit.log",  /* 单独审计文件 */
    .min_level = LOG_SECURITY,                 /* 仅记录安全事件 */
    .format = LOG_FORMAT_JSON,                  /* JSON便于分析 */
    .ring_buffer_size = 32 * 1024,              /* 32KB */
    .flush_interval_ms = 500,                   /* 快速刷新 */
    .enable_stdout = false
};

/* 主 Logger 配置（包含安全日志） */
LoggerConfig main_logger_config = {
    .log_file = "/var/log/chase/server.log",
    .audit_file = NULL,                         /* 不单独审计文件 */
    .min_level = LOG_INFO,
    .format = LOG_FORMAT_TEXT,
    .ring_buffer_size = 64 * 1024,
    .flush_interval_ms = 1000,
    .enable_stdout = true
};
```

### 5.3 安全事件类型对照表

| event_type | 触发条件 | severity | blocked |
|------------|----------|----------|---------|
| `blocked_connection` | 已封禁 IP 尝试连接 | 3-4 | true |
| `blocked_request` | 已封禁 IP 发送请求 | 4 | true |
| `rate_limit_triggered` | 超过请求速率限制 | 3 | true |
| `slowloris_detected` | Slowloris 攻击检测 | 4 | true |
| `path_traversal` | 路径穿越尝试 | 5 | true |
| `manual_block` | 手动封禁操作 | 5 | true |
| `manual_unblock` | 手动解封操作 | 2 | false |
| `connection_limit` | 连接数超限 | 3 | false |

### 5.4 日志级别配置

```c
/* 生产环境：仅记录重要安全事件 */
logger_set_level(logger, LOG_WARN);

/* 安全审计环境：记录所有安全事件 */
logger_set_level(logger, LOG_SECURITY);

/* 开发调试：记录所有事件 */
logger_set_level(logger, LOG_DEBUG);
```

---

## 6. 附录

### 6.1 默认配置常量

```c
#define SECURITY_DEFAULT_MAX_CONN_PER_IP      10
#define SECURITY_DEFAULT_MAX_REQ_PER_SEC      100
#define SECURITY_DEFAULT_MIN_REQ_RATE         50      /* bytes/sec */
#define SECURITY_DEFAULT_SLOWLORIS_TIMEOUT    30000   /* 30 seconds */
#define SECURITY_DEFAULT_BLOCK_DURATION       60000   /* 60 seconds */
#define SECURITY_DEFAULT_SHARD_COUNT          16
```

### 6.2 API 函数速查表

| 函数 | 用途 | 返回值 |
|------|------|--------|
| `security_create()` | 创建 Security 实例 | Security* |
| `security_destroy()` | 销毁实例 | void |
| `security_check_connection()` | 检查连接权限 | SecurityResult |
| `security_add_connection()` | 记录新连接 | SecurityResult |
| `security_remove_connection()` | 移除连接记录 | void |
| `security_check_request_rate()` | 检查请求速率 | SecurityResult |
| `security_block_ip()` | 手动封禁 IP | int (0/-1) |
| `security_unblock_ip()` | 解封 IP | void |
| `security_is_blocked()` | 检查封禁状态 | bool |
| `security_get_ip_stats()` | 获取 IP 统计 | int (0/-1) |
| `security_parse_ip()` | 解析 sockaddr | int (0/-1) |
| `security_ip_to_string()` | IP 转字符串 | int |
| `security_string_to_ip()` | 字符串转 IP | int (0/-1) |
| `security_cleanup()` | 清理过期条目 | void |
| `security_get_summary()` | 获取统计摘要 | void |

### 6.3 相关文件

- 头文件: `include/security.h`
- 实现文件: `src/security.c`
- 测试文件: `tests/test_security.c` (如有)
- Logger 头文件: `include/logger.h`

---

**文档版本**: 1.0  
**最后更新**: 2026-04-24  
**作者**: minghui.liu