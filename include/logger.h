/**
 * @file    logger.h
 * @brief   日志模块，支持异步 Ring Buffer 和安全审计
 *
 * @details
 *          - Worker 级文件日志
 *          - Ring Buffer 异步非阻塞写入
 *          - 日志级别: DEBUG, INFO, WARN, ERROR, SECURITY
 *          - 请求延迟日志
 *          - 安全审计日志（路径穿越、速率限制触发）
 *          - 可配置日志格式（文本/JSON）
 *
 * @layer   Core Layer
 *
 * @depends timer, error
 * @usedby  server, security, fileserve, handler
 *
 * @author  minghui.liu
 * @date    2026-04-23
 */

#ifndef CHASE_LOGGER_H
#define CHASE_LOGGER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 日志级别 */
typedef enum {
    LOG_DEBUG,      /* 调试信息 */
    LOG_INFO,       /* 一般信息 */
    LOG_WARN,       /* 警告 */
    LOG_ERROR,      /* 错误 */
    LOG_SECURITY    /* 安全事件（审计） */
} LogLevel;

/* 日志格式 */
typedef enum {
    LOG_FORMAT_TEXT,        /* 文本格式 */
    LOG_FORMAT_JSON         /* JSON 格式 */
} LogFormat;

/* 请求日志上下文 */
typedef struct RequestLogContext {
    const char *method;         /* HTTP 方法 */
    const char *path;           /* 请求路径 */
    const char *query;          /* 查询字符串 */
    int status_code;            /* 响应状态码 */
    uint64_t latency_ms;        /* 请求延迟（毫秒） */
    const char *client_ip;      /* 客户端 IP */
    size_t bytes_sent;          /* 发送字节 */
    size_t bytes_received;      /* 接收字节 */
} RequestLogContext;

/* 安全审计上下文 */
typedef struct SecurityLogContext {
    const char *event_type;     /* 事件类型 (如 "path_traversal", "rate_limit") */
    const char *client_ip;      /* 客户端 IP */
    const char *details;        /* 事件详情 */
    int severity;               /* 严重级别 (1-5) */
    bool blocked;               /* 是否被阻止 */
} SecurityLogContext;

/* 日志配置 */
typedef struct LoggerConfig {
    const char *log_file;       /* 日志文件路径 */
    const char *audit_file;     /* 审计日志文件路径（可选） */
    LogLevel min_level;         /* 最小日志级别 */
    LogFormat format;           /* 日志格式 */
    int ring_buffer_size;       /* Ring Buffer 大小（字节） */
    int flush_interval_ms;      /* 刷新间隔（毫秒） */
    bool enable_stdout;         /* 同时输出到 stdout */
} LoggerConfig;

/* Logger 结构体（不透明指针） */
typedef struct Logger Logger;

/* 默认配置值 */
#define LOGGER_DEFAULT_RING_BUFFER_SIZE   (64 * 1024)    /* 64KB */
#define LOGGER_DEFAULT_FLUSH_INTERVAL     1000           /* 1秒 */
#define LOGGER_DEFAULT_MIN_LEVEL          LOG_INFO

/**
 * 创建 Logger
 * @param config Logger 配置
 * @return Logger 指针，失败返回 NULL
 */
Logger *logger_create(const LoggerConfig *config);

/**
 * 销毁 Logger
 * @param logger Logger 指针
 */
void logger_destroy(Logger *logger);

/**
 * 记录日志
 * @param logger Logger 指针
 * @param level 日志级别
 * @param format 格式字符串
 * @param ... 格式参数
 */
void logger_log(Logger *logger, LogLevel level, const char *format, ...);

/**
 * 记录日志（va_list 版本）
 * @param logger Logger 指针
 * @param level 日志级别
 * @param format 格式字符串
 * @param args 格式参数
 */
void logger_log_v(Logger *logger, LogLevel level, const char *format, va_list args);

/**
 * 记录请求（含延迟）
 * @param logger Logger 指针
 * @param ctx 请求日志上下文
 */
void logger_log_request(Logger *logger, const RequestLogContext *ctx);

/**
 * 记录安全审计事件
 * @param logger Logger 指针
 * @param ctx 安全审计上下文
 */
void logger_log_security(Logger *logger, const SecurityLogContext *ctx);

/**
 * 记录路径穿越尝试（审计）
 * @param logger Logger 指针
 * @param ip 客户端 IP
 * @param attempted_path 尝试的路径
 * @param resolved_path 解析的路径（如有）
 */
void logger_log_path_traversal(Logger *logger, const char *ip,
                               const char *attempted_path,
                               const char *resolved_path);

/**
 * 记录速率限制触发（审计）
 * @param logger Logger 指针
 * @param ip 客户端 IP
 * @param limit_type 限制类型
 * @param current_count 当前计数
 * @param limit_value 限制阈值
 */
void logger_log_rate_limit(Logger *logger, const char *ip,
                           const char *limit_type,
                           int current_count, int limit_value);

/**
 * 设置最小日志级别
 * @param logger Logger 指针
 * @param level 最小级别
 */
void logger_set_level(Logger *logger, LogLevel level);

/**
 * 获取最小日志级别
 * @param logger Logger 指针
 * @return 最小级别
 */
LogLevel logger_get_level(Logger *logger);

/**
 * 强制刷新缓冲区
 * @param logger Logger 指针
 */
void logger_flush(Logger *logger);

/**
 * 获取日志级别名称
 * @param level 日志级别
 * @return 级别名称字符串
 */
const char *logger_level_name(LogLevel level);

/**
 * 格式化时间戳
 * @param ts 时间戳
 * @param buffer 输出缓冲区
 * @param size 缓冲区大小
 * @return 格式化字符串长度
 */
int logger_format_timestamp(struct timespec *ts, char *buffer, size_t size);

/**
 * 获取当前时间戳（毫秒）
 * @return 当前时间戳（毫秒）
 */
uint64_t logger_get_current_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_LOGGER_H */