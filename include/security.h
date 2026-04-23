/**
 * @file    security.h
 * @brief   安全模块，提供 IP 连接限制、速率限制和 Slowloris 检测
 *
 * @details
 *          - Worker 级实例（无跨 Worker 共享状态）
 *          - 单 IP 连接计数和限制
 *          - 速率限制（每秒请求数）
 *          - Slowloris 检测（最小请求速率）
 *          - IP 封禁（可配置持续时间）
 *          - 分片哈希表（高并发）
 *
 * @layer   Core Layer
 *
 * @depends timer, error
 * @usedby  server, worker
 *
 * @author  minghui.liu
 * @date    2026-04-23
 */

#ifndef CHASE_SECURITY_H
#define CHASE_SECURITY_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Security 结果码 */
typedef enum {
    SECURITY_OK,                /* 允许操作 */
    SECURITY_BLOCKED_IP,        /* IP 已被封禁 */
    SECURITY_RATE_LIMITED,      /* 超过速率限制 */
    SECURITY_TOO_MANY_CONN,     /* 连接数过多 */
    SECURITY_SLOWLORIS,         /* Slowloris 攻击检测 */
    SECURITY_INTERNAL_ERROR     /* 内部错误 */
} SecurityResult;

/* Security 配置 */
typedef struct SecurityConfig {
    int max_connections_per_ip;     /* 单 IP 最大并发连接数 */
    int max_requests_per_second;    /* 单 IP 每秒最大请求数 */
    int min_request_rate;           /* Slowloris 最小字节/秒 */
    int slowloris_timeout_ms;       /* Slowloris 检测超时 */
    int block_duration_ms;          /* IP 封禁持续时间 */
    int shard_count;                /* 分片哈希表分片数 */
} SecurityConfig;

/* IP 地址存储（支持 IPv4 和 IPv6） */
typedef struct IpAddress {
    uint8_t data[16];               /* IPv4 使用前 4 字节，IPv6 全部 */
    bool is_ipv6;                   /* 是否 IPv6 地址 */
} IpAddress;

/* IP 统计信息 */
typedef struct IpStats {
    int connection_count;           /* 当前连接数 */
    int request_count;              /* 请求计数 */
    uint64_t bytes_received;        /* 接收字节 */
    uint64_t last_request_time;     /* 最后请求时间 */
    bool is_blocked;                /* 是否被封禁 */
    uint64_t block_expire_time;     /* 封禁过期时间 */
} IpStats;

/* Security 结构体（不透明指针） */
typedef struct Security Security;

/* 默认配置值 */
#define SECURITY_DEFAULT_MAX_CONN_PER_IP      10
#define SECURITY_DEFAULT_MAX_REQ_PER_SEC      100
#define SECURITY_DEFAULT_MIN_REQ_RATE         50      /* 50 bytes/sec */
#define SECURITY_DEFAULT_SLOWLORIS_TIMEOUT    30000   /* 30 seconds */
#define SECURITY_DEFAULT_BLOCK_DURATION       60000   /* 60 seconds */
#define SECURITY_DEFAULT_SHARD_COUNT          16

/**
 * 创建 Security 模块
 * @param config Security 配置
 * @return Security 指针，失败返回 NULL
 */
Security *security_create(const SecurityConfig *config);

/**
 * 销毁 Security 模块
 * @param security Security 指针
 */
void security_destroy(Security *security);

/**
 * 检查连接是否允许
 * @param security Security 指针
 * @param ip IP 地址
 * @return SecurityResult
 */
SecurityResult security_check_connection(Security *security, const IpAddress *ip);

/**
 * 记录新连接
 * @param security Security 指针
 * @param ip IP 地址
 * @return SecurityResult
 */
SecurityResult security_add_connection(Security *security, const IpAddress *ip);

/**
 * 移除连接记录
 * @param security Security 指针
 * @param ip IP 地址
 */
void security_remove_connection(Security *security, const IpAddress *ip);

/**
 * 检查请求速率
 * @param security Security 指针
 * @param ip IP 地址
 * @param bytes_received 本次接收字节
 * @return SecurityResult
 */
SecurityResult security_check_request_rate(Security *security, const IpAddress *ip,
                                           size_t bytes_received);

/**
 * 封禁 IP
 * @param security Security 指针
 * @param ip IP 地址
 * @param duration_ms 封禁时长（0=使用默认）
 * @return 0 成功，-1 失败
 */
int security_block_ip(Security *security, const IpAddress *ip, int duration_ms);

/**
 * 解封 IP
 * @param security Security 指针
 * @param ip IP 地址
 */
void security_unblock_ip(Security *security, const IpAddress *ip);

/**
 * 检查 IP 是否被封禁
 * @param security Security 指针
 * @param ip IP 地址
 * @return true 已封禁
 */
bool security_is_blocked(Security *security, const IpAddress *ip);

/**
 * 获取 IP 统计信息
 * @param security Security 指针
 * @param ip IP 地址
 * @param stats 输出：统计信息
 * @return 0 成功，-1 未追踪
 */
int security_get_ip_stats(Security *security, const IpAddress *ip, IpStats *stats);

/**
 * 从 sockaddr 解析 IP 地址
 * @param addr sockaddr 指针
 * @param ip 输出：IP 地址结构
 * @return 0 成功，-1 失败
 */
int security_parse_ip(const struct sockaddr *addr, IpAddress *ip);

/**
 * IP 地址转字符串
 * @param ip IP 地址
 * @param buffer 输出缓冲区
 * @param size 缓冲区大小
 * @return 字符串长度
 */
int security_ip_to_string(const IpAddress *ip, char *buffer, size_t size);

/**
 * 字符串转 IP 地址
 * @param str IP 字符串
 * @param ip 输出：IP 地址结构
 * @return 0 成功，-1 失败
 */
int security_string_to_ip(const char *str, IpAddress *ip);

/**
 * 清理过期条目（定期调用）
 * @param security Security 指针
 */
void security_cleanup(Security *security);

/**
 * 获取统计摘要
 * @param security Security 指针
 * @param total_tracked 输出：总追踪 IP 数
 * @param total_blocked 输出：总封禁 IP 数
 */
void security_get_summary(Security *security, int *total_tracked, int *total_blocked);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_SECURITY_H */