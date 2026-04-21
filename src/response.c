/**
 * @file    response.c
 * @brief   HTTP 响应构建器实现
 *
 * @details
 *          - 使用动态数组存储响应头
 *          - 自动生成 HTTP 状态行
 *          - 支持多种 Content-Type
 *
 * @layer   Handler Layer
 *
 * @depends http_parser, error
 * @usedby  handler, server, examples
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "response.h"
#include "error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>

/* 最大响应头数量 */
#define MAX_HEADERS 32

/* 响应头结构 */
typedef struct {
    char *key;
    char *value;
} ResponseHeader;

/* HttpResponse 结构体 */
struct HttpResponse {
    HttpStatus status;
    ResponseHeader headers[MAX_HEADERS];
    int header_count;
    const char *body;
    size_t body_len;
    char *body_owned;      /* 如果需要复制 body */

    /* 新增：用于部分发送的内部缓冲区 */
    char *send_buffer;     /* 构建的完整响应缓冲区 */
    size_t send_buffer_len; /* 缓冲区总长度 */
    size_t send_buffer_cap; /* 缓冲区容量 */
    size_t bytes_sent;     /* 已发送字节数 */
};

/* ========== API 实现 ========== */

HttpResponse *response_create(HttpStatus status) {
    HttpResponse *resp = malloc(sizeof(HttpResponse));
    if (!resp) return NULL;

    resp->status = status;
    resp->header_count = 0;
    resp->body = NULL;
    resp->body_len = 0;
    resp->body_owned = NULL;
    resp->send_buffer = NULL;
    resp->send_buffer_len = 0;
    resp->send_buffer_cap = 0;
    resp->bytes_sent = 0;

    /* 初始化所有头指针 */
    for (int i = 0; i < MAX_HEADERS; i++) {
        resp->headers[i].key = NULL;
        resp->headers[i].value = NULL;
    }

    return resp;
}

void response_set_status(HttpResponse *resp, HttpStatus status) {
    if (!resp) return;
    resp->status = status;
}

void response_destroy(HttpResponse *resp) {
    if (!resp) return;

    /* 释放所有头的字符串 */
    for (int i = 0; i < resp->header_count; i++) {
        free(resp->headers[i].key);
        free(resp->headers[i].value);
    }

    /* 释放 owned body */
    if (resp->body_owned) {
        free(resp->body_owned);
    }

    /* 释放发送缓冲区 */
    if (resp->send_buffer) {
        free(resp->send_buffer);
    }

    free(resp);
}

void response_set_header(HttpResponse *resp, const char *key, const char *value) {
    if (!resp || !key || !value) return;
    if (resp->header_count >= MAX_HEADERS) return;

    /* 检查是否已存在同名头，更新值 */
    for (int i = 0; i < resp->header_count; i++) {
        if (strcmp(resp->headers[i].key, key) == 0) {
            free(resp->headers[i].value);
            resp->headers[i].value = strdup(value);
            return;
        }
    }

    /* 添加新头 */
    resp->headers[resp->header_count].key = strdup(key);
    resp->headers[resp->header_count].value = strdup(value);
    resp->header_count++;
}

void response_set_body(HttpResponse *resp, const char *body, size_t len) {
    if (!resp) return;

    resp->body = body;
    resp->body_len = len;

    /* 不复制 body，由调用者管理 */
    if (resp->body_owned) {
        free(resp->body_owned);
        resp->body_owned = NULL;
    }
}

void response_set_body_json(HttpResponse *resp, const char *json) {
    if (!resp || !json) return;

    response_set_header(resp, "Content-Type", "application/json");
    response_set_body(resp, json, strlen(json));
}

void response_set_body_html(HttpResponse *resp, const char *html, size_t len) {
    if (!resp || !html) return;

    response_set_header(resp, "Content-Type", "text/html; charset=utf-8");
    response_set_body(resp, html, len);
}

HttpStatus response_get_status(HttpResponse *resp) {
    if (!resp) return HTTP_STATUS_INTERNAL_ERROR;
    return resp->status;
}

int response_build(HttpResponse *resp, char *buf, size_t buf_size) {
    if (!resp || !buf || buf_size == 0) return -1;

    /* 获取状态描述 */
    const char *status_text = http_status_get_description(resp->status);

    int offset = 0;

    /* 写入状态行 */
    offset += snprintf(buf + offset, buf_size - offset,
                       "HTTP/1.1 %d %s\r\n", resp->status, status_text);

    if (offset >= (int)buf_size) return -1;

    /* 写入响应头 */
    for (int i = 0; i < resp->header_count; i++) {
        offset += snprintf(buf + offset, buf_size - offset,
                           "%s: %s\r\n", resp->headers[i].key, resp->headers[i].value);
        if (offset >= (int)buf_size) return -1;
    }

    /* 写入 Content-Length（如果未设置） */
    bool has_content_length = false;
    for (int i = 0; i < resp->header_count; i++) {
        if (strcmp(resp->headers[i].key, "Content-Length") == 0) {
            has_content_length = true;
            break;
        }
    }

    if (!has_content_length) {
        offset += snprintf(buf + offset, buf_size - offset,
                           "Content-Length: %zu\r\n", resp->body_len);
        if (offset >= (int)buf_size) return -1;
    }

    /* 写入空行 */
    offset += snprintf(buf + offset, buf_size - offset, "\r\n");
    if (offset >= (int)buf_size) return -1;

    /* 检查是否有足够空间写入响应体 */
    if (resp->body && resp->body_len > 0) {
        size_t remaining = buf_size - offset;
        if (remaining < resp->body_len) {
            /* 响应体太大，返回错误 */
            /* 调用者应使用 response_send() 进行分块发送 */
            return -2;  /* 特殊错误码：需要分块发送 */
        }
        memcpy(buf + offset, resp->body, resp->body_len);
        offset += resp->body_len;
    }

    return offset;
}

/* 循环写入辅助函数，处理部分发送和 EAGAIN */
static ssize_t write_all(int fd, const char *data, size_t len) {
    size_t total_sent = 0;

    while (total_sent < len) {
        ssize_t sent = write(fd, data + total_sent, len - total_sent);

        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* socket 缓冲区满，需要等待 */
                /* 对于非阻塞 socket，返回已发送的字节数 */
                /* 调用者应该将剩余数据缓存，等待 EV_WRITE 事件 */
                break;
            }
            if (errno == EINTR) {
                /* 被信号中断，继续尝试 */
                continue;
            }
            /* 其他错误 */
            return -1;
        }

        if (sent == 0) {
            /* 连接关闭 */
            break;
        }

        total_sent += sent;
    }

    return (ssize_t)total_sent;
}

int response_send(HttpResponse *resp, int fd) {
    if (!resp || fd < 0) return -1;

    /* 构建响应 */
    char buf[8192];
    int total_len = response_build(resp, buf, sizeof(buf));
    if (total_len < 0) return -1;

    /* 如果响应太大，需要分块发送 */
    if ((size_t)total_len >= sizeof(buf) && resp->body && resp->body_len > 0) {
        /* 发送头部部分 */
        char header_buf[4096];
        int header_len = 0;

        const char *status_text = http_status_get_description(resp->status);
        header_len += snprintf(header_buf + header_len, sizeof(header_buf) - header_len,
                               "HTTP/1.1 %d %s\r\n", resp->status, status_text);

        for (int i = 0; i < resp->header_count; i++) {
            header_len += snprintf(header_buf + header_len, sizeof(header_buf) - header_len,
                                   "%s: %s\r\n", resp->headers[i].key, resp->headers[i].value);
        }

        /* 添加 Content-Length */
        bool has_content_length = false;
        for (int i = 0; i < resp->header_count; i++) {
            if (strcmp(resp->headers[i].key, "Content-Length") == 0) {
                has_content_length = true;
                break;
            }
        }
        if (!has_content_length) {
            header_len += snprintf(header_buf + header_len, sizeof(header_buf) - header_len,
                                   "Content-Length: %zu\r\n", resp->body_len);
        }

        header_len += snprintf(header_buf + header_len, sizeof(header_buf) - header_len, "\r\n");

        /* 循环发送头部 */
        ssize_t sent = write_all(fd, header_buf, header_len);
        if (sent < 0) return -1;
        if ((size_t)sent < header_len) {
            /* 部分发送，返回已发送字节数（调用者需处理剩余数据） */
            return (int)sent;
        }

        /* 循环发送响应体 */
        sent = write_all(fd, resp->body, resp->body_len);
        if (sent < 0) return -1;

        return header_len + sent;
    }

    /* 循环发送完整响应 */
    ssize_t sent = write_all(fd, buf, total_len);
    if (sent < 0) return -1;

    return (int)sent;
}

/* ========== 新增：部分发送支持 ========== */

/**
 * 构建完整响应到内部缓冲区
 */
static int response_build_internal(HttpResponse *resp) {
    if (!resp) return -1;

    /* 如果已有缓冲区且大小足够，直接使用 */
    size_t estimated_len = 1024 + resp->body_len;  /* 头部估算 1KB */

    if (!resp->send_buffer || resp->send_buffer_cap < estimated_len) {
        /* 分配新缓冲区 */
        size_t new_cap = estimated_len * 2;
        char *new_buf = malloc(new_cap);
        if (!new_buf) return -1;

        if (resp->send_buffer) {
            free(resp->send_buffer);
        }
        resp->send_buffer = new_buf;
        resp->send_buffer_cap = new_cap;
    }

    /* 构建响应到缓冲区 */
    int len = response_build(resp, resp->send_buffer, resp->send_buffer_cap);
    if (len < 0) {
        return -1;
    }

    resp->send_buffer_len = len;
    resp->bytes_sent = 0;

    return 0;
}

ResponseSendResult response_send_ex(HttpResponse *resp, int fd) {
    ResponseSendResult result = {RESPONSE_SEND_ERROR, 0, 0};

    if (!resp || fd < 0) {
        return result;
    }

    /* 构建响应到内部缓冲区 */
    if (response_build_internal(resp) < 0) {
        return result;
    }

    result.total_bytes = resp->send_buffer_len;

    /* 发送数据 */
    ssize_t sent = write_all(fd, resp->send_buffer + resp->bytes_sent,
                             resp->send_buffer_len - resp->bytes_sent);

    if (sent < 0) {
        result.status = RESPONSE_SEND_ERROR;
        return result;
    }

    resp->bytes_sent += sent;
    result.bytes_sent = resp->bytes_sent;

    if (resp->bytes_sent >= resp->send_buffer_len) {
        result.status = RESPONSE_SEND_COMPLETE;
    } else {
        result.status = RESPONSE_SEND_PARTIAL;
    }

    return result;
}

const char *response_get_pending(HttpResponse *resp, size_t *offset, size_t *len) {
    if (!resp || !offset || !len) {
        if (offset) *offset = 0;
        if (len) *len = 0;
        return NULL;
    }

    if (!resp->send_buffer || resp->bytes_sent >= resp->send_buffer_len) {
        *offset = 0;
        *len = 0;
        return NULL;
    }

    *offset = resp->bytes_sent;
    *len = resp->send_buffer_len - resp->bytes_sent;

    return resp->send_buffer + resp->bytes_sent;
}

int response_send_remaining(HttpResponse *resp, int fd, size_t offset, size_t len) {
    if (!resp || fd < 0) return -1;
    if (!resp->send_buffer || offset >= resp->send_buffer_len) return -1;

    size_t actual_len = len;
    if (offset + len > resp->send_buffer_len) {
        actual_len = resp->send_buffer_len - offset;
    }

    ssize_t sent = write_all(fd, resp->send_buffer + offset, actual_len);
    if (sent < 0) return -1;

    resp->bytes_sent = offset + sent;

    return (int)sent;
}