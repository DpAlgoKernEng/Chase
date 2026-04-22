/**
 * @file    http_parser.h
 * @brief   HTTP/1.1 请求解析器，增量式状态机实现
 *
 * @details
 *          - 增量解析，支持流式输入
 *          - 状态机驱动，解析与 I/O 解耦
 *          - 支持 GET/POST/PUT/DELETE/HEAD/OPTIONS/PATCH
 *          - 解析请求行、头部、响应体
 *
 * @layer   Core Layer
 *
 * @depends 无依赖
 * @usedby  server, handler, router
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#ifndef CHASE_HTTP_PARSER_H
#define CHASE_HTTP_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP 方法 */
typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE,
    HTTP_HEAD,
    HTTP_OPTIONS,
    HTTP_PATCH
} HttpMethod;

/* 解析结果 */
typedef enum {
    PARSE_OK,          /* 解析成功 */
    PARSE_NEED_MORE,   /* 需要更多数据 */
    PARSE_ERROR,       /* 解析错误 */
    PARSE_COMPLETE     /* 解析完成 */
} ParseResult;

/* HTTP 头部 */
typedef struct HttpHeader {
    char *name;
    char *value;
} HttpHeader;

/* HTTP 请求 */
typedef struct HttpRequest {
    HttpMethod method;
    char *path;
    char *query;
    char *version;
    HttpHeader *headers;
    int header_count;
    int header_capacity;
    char *body;
    size_t body_length;
    size_t content_length;
    bool is_chunked;            /* Phase 3: 是否使用 chunked 编码 */
    size_t chunk_body_capacity; /* Phase 3: chunked body 缓冲区容量 */
} HttpRequest;

/* HTTP 解析器 */
typedef struct HttpParser HttpParser;

/**
 * 创建 HTTP 解析器
 * @return HttpParser 指针，失败返回 NULL
 */
HttpParser *http_parser_create(void);

/**
 * 销毁 HTTP 解析器
 * @param parser HttpParser 指针
 */
void http_parser_destroy(HttpParser *parser);

/**
 * 创建 HTTP 请求
 * @return HttpRequest 指针，失败返回 NULL
 */
HttpRequest *http_request_create(void);

/**
 * 销毁 HTTP 请求
 * @param req HttpRequest 指针
 */
void http_request_destroy(HttpRequest *req);

/**
 * 解析 HTTP 数据
 * @param parser HttpParser 挀针
 * @param req HttpRequest 指针
 * @param data 输入数据
 * @param len 数据长度
 * @param consumed 已消耗的字节数（输出）
 * @return ParseResult
 */
ParseResult http_parser_parse(HttpParser *parser, HttpRequest *req,
                              const char *data, size_t len, size_t *consumed);

/**
 * 获取请求头
 * @param req HttpRequest 指针
 * @param name 头部名称
 * @return HttpHeader 指针，未找到返回 NULL
 */
HttpHeader *http_request_get_header(HttpRequest *req, const char *name);

/**
 * 获取请求头值
 * @param req HttpRequest 指针
 * @param name 头部名称
 * @return 头部值，未找到返回 NULL
 */
const char *http_request_get_header_value(HttpRequest *req, const char *name);

/**
 * 重置解析器（用于 Keep-Alive）
 * @param parser HttpParser 挀针
 */
void http_parser_reset(HttpParser *parser);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_HTTP_PARSER_H */