/**
 * @file    response.h
 * @brief   HTTP 响应构建器，标准化 HTTP 响应格式
 *
 * @details
 *          - 支持设置状态码、响应头、响应体
 *          - 自动计算 Content-Length
 *          - 支持直接发送到 socket fd
 *          - 支持 JSON 响应快捷方法
 *
 * @layer   Handler Layer
 *
 * @depends http_parser (for status codes), error
 * @usedby  handler, server, examples
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#ifndef CHASE_RESPONSE_H
#define CHASE_RESPONSE_H

#include <stddef.h>

#include "error.h"  /* for HttpStatus */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HTTP 响应结构体（ opaque）
 */
typedef struct HttpResponse HttpResponse;

/**
 * 创建 HTTP 响应
 * @param status HTTP 状态码（HttpStatus 枚举值）
 * @return HttpResponse 指针，失败返回 NULL
 */
HttpResponse *response_create(HttpStatus status);

/**
 * 销毁 HTTP 响应
 * @param resp HttpResponse 指针
 */
void response_destroy(HttpResponse *resp);

/**
 * 设置响应头
 * @param resp HttpResponse 指针
 * @param key 头名称（如 "Content-Type"）
 * @param value 头值
 */
void response_set_header(HttpResponse *resp, const char *key, const char *value);

/**
 * 设置响应体
 * @param resp HttpResponse 指针
 * @param body 响应体内容
 * @param len 响应体长度
 */
void response_set_body(HttpResponse *resp, const char *body, size_t len);

/**
 * 设置 JSON 响应体（自动设置 Content-Type）
 * @param resp HttpResponse 挌针
 * @param json JSON 字符串
 */
void response_set_body_json(HttpResponse *resp, const char *json);

/**
 * 设置 HTML 响应体（自动设置 Content-Type）
 * @param resp HttpResponse 指针
 * @param html HTML 字符串
 * @param len HTML 长度
 */
void response_set_body_html(HttpResponse *resp, const char *html, size_t len);

/**
 * 设置响应状态码
 * @param resp HttpResponse 指针
 * @param status HTTP 状态码（HttpStatus 枚举值）
 */
void response_set_status(HttpResponse *resp, HttpStatus status);

/**
 * 获取响应状态码
 * @param resp HttpResponse 指针
 * @return 状态码（HttpStatus 枚举值）
 */
HttpStatus response_get_status(HttpResponse *resp);

/**
 * 构建完整 HTTP 响应字符串
 * @param resp HttpResponse 指针
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 实际写入长度，-1 表示错误
 */
int response_build(HttpResponse *resp, char *buf, size_t buf_size);

/**
 * 直接发送响应到 socket fd
 * @param resp HttpResponse 指针
 * @param fd socket 文件描述符
 * @return 发送的字节数，-1 表示错误
 */
int response_send(HttpResponse *resp, int fd);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_RESPONSE_H */