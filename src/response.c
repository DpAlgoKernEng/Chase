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

    /* 写入响应体 */
    if (resp->body && resp->body_len > 0) {
        size_t remaining = buf_size - offset;
        if (remaining < resp->body_len) {
            /* 缓冲区不足，只写入部分 */
            memcpy(buf + offset, resp->body, remaining);
            offset += remaining;
        } else {
            memcpy(buf + offset, resp->body, resp->body_len);
            offset += resp->body_len;
        }
    }

    return offset;
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

        /* 发送头部 */
        ssize_t sent = write(fd, header_buf, header_len);
        if (sent < 0) return -1;

        /* 发送响应体 */
        sent = write(fd, resp->body, resp->body_len);
        if (sent < 0) return -1;

        return header_len + sent;
    }

    /* 发送完整响应 */
    ssize_t sent = write(fd, buf, total_len);
    if (sent < 0) return -1;

    return (int)sent;
}