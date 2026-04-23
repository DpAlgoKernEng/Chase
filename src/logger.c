/**
 * @file    logger.c
 * @brief   日志模块实现
 *
 * @details
 *          - Ring Buffer 异步写入（单生产者单消费者）
 *          - 后台线程消费 Ring Buffer 写入文件
 *          - 支持文本和 JSON 格式
 *          - 安全审计独立文件
 *
 * @layer   Core Layer
 *
 * @depends timer, error
 * @usedby  server, security, fileserve, handler
 *
 * @author  minghui.liu
 * @date    2026-04-23
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>

#include "logger.h"
#include "error.h"

/* Log Ring Buffer 条目 */
typedef struct LogEntry {
    LogLevel level;
    uint64_t timestamp;
    char message[4096];
    bool is_audit;             /* 是否为审计日志 */
} LogEntry;

/* Ring Buffer 结构 */
typedef struct LogRingBuffer {
    LogEntry *entries;
    int capacity;
    int head;                  /* 读位置（消费者） */
    int tail;                  /* 写位置（生产者） */
    int count;                 /* 当前条目数 */
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} LogRingBuffer;

/* Logger 结构 */
struct Logger {
    LoggerConfig config;
    FILE *log_file;
    FILE *audit_file;
    LogRingBuffer *ring_buffer;
    pthread_t writer_thread;
    volatile bool running;
    volatile uint64_t dropped_count;
    LogLevel current_level;
};

/* ============== Ring Buffer 实现 ============== */

static LogRingBuffer *ring_buffer_create(int capacity) {
    LogRingBuffer *rb = malloc(sizeof(LogRingBuffer));
    if (!rb) return NULL;

    rb->entries = malloc(capacity * sizeof(LogEntry));
    if (!rb->entries) {
        free(rb);
        return NULL;
    }

    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;

    pthread_mutex_init(&rb->lock, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    pthread_cond_init(&rb->not_full, NULL);

    return rb;
}

static void ring_buffer_destroy(LogRingBuffer *rb) {
    if (!rb) return;

    pthread_mutex_destroy(&rb->lock);
    pthread_cond_destroy(&rb->not_empty);
    pthread_cond_destroy(&rb->not_full);

    free(rb->entries);
    free(rb);
}

/* 写入 Ring Buffer（阻塞，直到有空位） */
static int ring_buffer_write(LogRingBuffer *rb, const LogEntry *entry, bool blocking) {
    pthread_mutex_lock(&rb->lock);

    /* 检查是否满 */
    if (rb->count >= rb->capacity) {
        if (!blocking) {
            pthread_mutex_unlock(&rb->lock);
            return -1;  /* BUFFER_FULL */
        }
        /* 阻塞等待空位 */
        while (rb->count >= rb->capacity) {
            pthread_cond_wait(&rb->not_full, &rb->lock);
        }
    }

    /* 写入条目 */
    rb->entries[rb->tail] = *entry;
    rb->tail = (rb->tail + 1) % rb->capacity;
    rb->count++;

    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->lock);

    return 0;
}

/* 从 Ring Buffer 读取（支持非阻塞和超时） */
static int ring_buffer_read(LogRingBuffer *rb, LogEntry *entry, bool blocking, volatile bool *running) {
    pthread_mutex_lock(&rb->lock);

    /* 检查是否空 */
    while (rb->count <= 0) {
        if (!blocking) {
            pthread_mutex_unlock(&rb->lock);
            return 0;  /* BUFFER_EMPTY */
        }
        if (!*running) {
            /* 不再运行，退出 */
            pthread_mutex_unlock(&rb->lock);
            return 0;
        }
        /* 阻塞等待数据 */
        pthread_cond_wait(&rb->not_empty, &rb->lock);
    }

    /* 读取条目 */
    *entry = rb->entries[rb->head];
    rb->head = (rb->head + 1) % rb->capacity;
    rb->count--;

    pthread_cond_signal(&rb->not_full);
    pthread_mutex_unlock(&rb->lock);

    return 1;
}

/* 非阻塞检查 Ring Buffer 是否有数据 */
static int ring_buffer_has_data(LogRingBuffer *rb) {
    pthread_mutex_lock(&rb->lock);
    int count = rb->count;
    pthread_mutex_unlock(&rb->lock);
    return count;
}

/* ============== 后台写入线程 ============== */

static void format_log_entry_text(const LogEntry *entry, char *buffer, size_t size) {
    struct timespec ts = {
        .tv_sec = entry->timestamp / 1000,
        .tv_nsec = (entry->timestamp % 1000) * 1000000
    };
    char timestamp[64];
    logger_format_timestamp(&ts, timestamp, sizeof(timestamp));

    snprintf(buffer, size, "[%s] [%s] %s\n",
             timestamp, logger_level_name(entry->level), entry->message);
}

static void format_log_entry_json(const LogEntry *entry, char *buffer, size_t size) {
    struct timespec ts = {
        .tv_sec = entry->timestamp / 1000,
        .tv_nsec = (entry->timestamp % 1000) * 1000000
    };
    char timestamp[64];
    logger_format_timestamp(&ts, timestamp, sizeof(timestamp));

    snprintf(buffer, size,
             "{\"timestamp\":\"%s\",\"level\":\"%s\",\"message\":\"%s\"}\n",
             timestamp, logger_level_name(entry->level), entry->message);
}

static void *writer_thread_func(void *arg) {
    Logger *logger = (Logger *)arg;
    LogEntry entry;
    char formatted[8192];

    while (logger->running) {
        int ret = ring_buffer_read(logger->ring_buffer, &entry, true, &logger->running);

        if (ret > 0) {
            /* 选择输出文件 */
            FILE *file = (entry.is_audit && logger->audit_file) ?
                         logger->audit_file : logger->log_file;

            /* 格式化日志 */
            if (logger->config.format == LOG_FORMAT_JSON) {
                format_log_entry_json(&entry, formatted, sizeof(formatted));
            } else {
                format_log_entry_text(&entry, formatted, sizeof(formatted));
            }

            /* 写入文件 */
            if (file) {
                fputs(formatted, file);
                fflush(file);
            }

            /* 同时输出到 stdout */
            if (logger->config.enable_stdout) {
                fputs(formatted, stdout);
                fflush(stdout);
            }
        }
    }

    /* 线程退出前刷新所有剩余日志 */
    while (ring_buffer_read(logger->ring_buffer, &entry, false, &logger->running) > 0) {
        FILE *file = (entry.is_audit && logger->audit_file) ?
                     logger->audit_file : logger->log_file;

        if (logger->config.format == LOG_FORMAT_JSON) {
            format_log_entry_json(&entry, formatted, sizeof(formatted));
        } else {
            format_log_entry_text(&entry, formatted, sizeof(formatted));
        }

        if (file) {
            fputs(formatted, file);
        }
        if (logger->config.enable_stdout) {
            fputs(formatted, stdout);
        }
    }

    return NULL;
}

/* ============== Logger API 实现 ============== */

Logger *logger_create(const LoggerConfig *config) {
    if (!config || !config->log_file) {
        return NULL;
    }

    Logger *logger = malloc(sizeof(Logger));
    if (!logger) return NULL;

    /* 复制配置 */
    logger->config = *config;
    if (config->ring_buffer_size <= 0) {
        logger->config.ring_buffer_size = LOGGER_DEFAULT_RING_BUFFER_SIZE;
    }
    if (config->flush_interval_ms <= 0) {
        logger->config.flush_interval_ms = LOGGER_DEFAULT_FLUSH_INTERVAL;
    }

    /* 计算 Ring Buffer 条目容量 */
    int buffer_capacity = logger->config.ring_buffer_size / sizeof(LogEntry);
    if (buffer_capacity < 100) buffer_capacity = 100;

    /* 创建 Ring Buffer */
    logger->ring_buffer = ring_buffer_create(buffer_capacity);
    if (!logger->ring_buffer) {
        free(logger);
        return NULL;
    }

    /* 打开日志文件 */
    logger->log_file = fopen(config->log_file, "a");
    if (!logger->log_file) {
        ring_buffer_destroy(logger->ring_buffer);
        free(logger);
        return NULL;
    }

    /* 打开审计文件（可选） */
    if (config->audit_file) {
        logger->audit_file = fopen(config->audit_file, "a");
        if (!logger->audit_file) {
            /* 审计文件打开失败不影响主日志 */
            logger->audit_file = NULL;
        }
    } else {
        logger->audit_file = NULL;
    }

    logger->running = true;
    logger->dropped_count = 0;
    logger->current_level = config->min_level;

    /* 创建后台写入线程 */
    int ret = pthread_create(&logger->writer_thread, NULL, writer_thread_func, logger);
    if (ret != 0) {
        fclose(logger->log_file);
        if (logger->audit_file) fclose(logger->audit_file);
        ring_buffer_destroy(logger->ring_buffer);
        free(logger);
        return NULL;
    }

    return logger;
}

void logger_destroy(Logger *logger) {
    if (!logger) return;

    /* 停止后台线程 */
    logger->running = false;
    pthread_cond_signal(&logger->ring_buffer->not_empty);
    pthread_join(logger->writer_thread, NULL);

    /* 关闭文件 */
    if (logger->log_file) {
        fflush(logger->log_file);
        fclose(logger->log_file);
    }
    if (logger->audit_file) {
        fflush(logger->audit_file);
        fclose(logger->audit_file);
    }

    /* 释放资源 */
    ring_buffer_destroy(logger->ring_buffer);
    free(logger);
}

void logger_log(Logger *logger, LogLevel level, const char *format, ...) {
    va_list args;
    va_start(args, format);
    logger_log_v(logger, level, format, args);
    va_end(args);
}

void logger_log_v(Logger *logger, LogLevel level, const char *format, va_list args) {
    if (!logger || level < logger->current_level) return;

    LogEntry entry;
    entry.level = level;
    entry.timestamp = logger_get_current_ms();
    entry.is_audit = (level == LOG_SECURITY);

    /* 格式化消息 */
    vsnprintf(entry.message, sizeof(entry.message), format, args);

    /* 写入 Ring Buffer（非阻塞） */
    int ret = ring_buffer_write(logger->ring_buffer, &entry, false);
    if (ret < 0) {
        /* Ring Buffer 满，记录丢弃 */
        logger->dropped_count++;

        /* ERROR 和 SECURITY 级别直接同步写入 */
        if (level >= LOG_ERROR) {
            char formatted[8192];
            FILE *file = logger->log_file;

            if (logger->config.format == LOG_FORMAT_JSON) {
                format_log_entry_json(&entry, formatted, sizeof(formatted));
            } else {
                format_log_entry_text(&entry, formatted, sizeof(formatted));
            }

            if (file) {
                fputs(formatted, file);
                fflush(file);
            }
            if (logger->config.enable_stdout) {
                fputs(formatted, stdout);
                fflush(stdout);
            }
        }
    }
}

void logger_log_request(Logger *logger, const RequestLogContext *ctx) {
    if (!logger || !ctx) return;

    if (logger->config.format == LOG_FORMAT_JSON) {
        logger_log(logger, LOG_INFO,
                   "{\"request\":{\"method\":\"%s\",\"path\":\"%s\",\"query\":\"%s\","
                   "\"status\":%d,\"latency\":%llu,\"ip\":\"%s\",\"sent\":%zu,\"recv\":%zu}}",
                   ctx->method ? ctx->method : "-",
                   ctx->path ? ctx->path : "-",
                   ctx->query ? ctx->query : "-",
                   ctx->status_code,
                   ctx->latency_ms,
                   ctx->client_ip ? ctx->client_ip : "-",
                   ctx->bytes_sent,
                   ctx->bytes_received);
    } else {
        logger_log(logger, LOG_INFO,
                   "%s %s%s%s %d %llums IP=%s sent=%zu recv=%zu",
                   ctx->method ? ctx->method : "-",
                   ctx->path ? ctx->path : "-",
                   ctx->query ? "?" : "",
                   ctx->query ? ctx->query : "",
                   ctx->status_code,
                   ctx->latency_ms,
                   ctx->client_ip ? ctx->client_ip : "-",
                   ctx->bytes_sent,
                   ctx->bytes_received);
    }
}

void logger_log_security(Logger *logger, const SecurityLogContext *ctx) {
    if (!logger || !ctx) return;

    if (logger->config.format == LOG_FORMAT_JSON) {
        logger_log(logger, LOG_SECURITY,
                   "{\"security\":{\"type\":\"%s\",\"ip\":\"%s\",\"details\":\"%s\","
                   "\"severity\":%d,\"blocked\":%s}}",
                   ctx->event_type ? ctx->event_type : "-",
                   ctx->client_ip ? ctx->client_ip : "-",
                   ctx->details ? ctx->details : "-",
                   ctx->severity,
                   ctx->blocked ? "true" : "false");
    } else {
        logger_log(logger, LOG_SECURITY,
                   "[SECURITY] %s IP=%s severity=%d blocked=%s %s",
                   ctx->event_type ? ctx->event_type : "-",
                   ctx->client_ip ? ctx->client_ip : "-",
                   ctx->severity,
                   ctx->blocked ? "yes" : "no",
                   ctx->details ? ctx->details : "");
    }
}

void logger_log_path_traversal(Logger *logger, const char *ip,
                               const char *attempted_path,
                               const char *resolved_path) {
    if (!logger) return;

    SecurityLogContext ctx = {
        .event_type = "path_traversal",
        .client_ip = ip,
        .details = resolved_path ?
                   "resolved_path" : "resolution_failed",
        .severity = 4,
        .blocked = true
    };

    if (resolved_path) {
        logger_log_security(logger, &ctx);
        logger_log(logger, LOG_SECURITY, "attempted=%s resolved=%s",
                   attempted_path ? attempted_path : "-",
                   resolved_path);
    } else {
        logger_log_security(logger, &ctx);
        logger_log(logger, LOG_SECURITY, "attempted=%s resolution_failed",
                   attempted_path ? attempted_path : "-");
    }
}

void logger_log_rate_limit(Logger *logger, const char *ip,
                           const char *limit_type,
                           int current_count, int limit_value) {
    if (!logger) return;

    SecurityLogContext ctx = {
        .event_type = limit_type ? limit_type : "rate_limit",
        .client_ip = ip,
        .details = "limit_exceeded",
        .severity = 3,
        .blocked = true
    };

    logger_log_security(logger, &ctx);
    logger_log(logger, LOG_SECURITY, "count=%d limit=%d",
               current_count, limit_value);
}

void logger_set_level(Logger *logger, LogLevel level) {
    if (!logger) return;
    logger->current_level = level;
}

LogLevel logger_get_level(Logger *logger) {
    if (!logger) return LOG_INFO;
    return logger->current_level;
}

void logger_flush(Logger *logger) {
    if (!logger) return;

    /* 通知后台线程处理所有数据 */
    pthread_cond_signal(&logger->ring_buffer->not_empty);

    /* 等待 Ring Buffer 空 */
    while (ring_buffer_has_data(logger->ring_buffer) > 0) {
        usleep(1000);  /* 1ms */
    }

    /* 刷新文件 */
    if (logger->log_file) fflush(logger->log_file);
    if (logger->audit_file) fflush(logger->audit_file);
}

const char *logger_level_name(LogLevel level) {
    switch (level) {
        case LOG_DEBUG:    return "DEBUG";
        case LOG_INFO:     return "INFO";
        case LOG_WARN:     return "WARN";
        case LOG_ERROR:    return "ERROR";
        case LOG_SECURITY: return "SECURITY";
        default:           return "UNKNOWN";
    }
}

int logger_format_timestamp(struct timespec *ts, char *buffer, size_t size) {
    if (!ts || !buffer || size < 32) return -1;

    struct tm tm;
    time_t sec = ts->tv_sec;
    localtime_r(&sec, &tm);

    int len = snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec,
                       (int)(ts->tv_nsec / 1000000));

    return len;
}

uint64_t logger_get_current_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}