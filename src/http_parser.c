/**
 * @file    http_parser.c
 * @brief   HTTP/1.1 请求解析器实现
 *
 * @details
 *          - 增量式状态机解析
 *          - 支持流式输入
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

#include "http_parser.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* 解析状态（增量状态机） */
typedef enum {
    PARSER_STATE_INIT,           /* 初始状态 */
    PARSER_STATE_METHOD,         /* 解析方法 */
    PARSER_STATE_METHOD_END,     /* 方法后空格 */
    PARSER_STATE_PATH,           /* 解析路径 */
    PARSER_STATE_QUERY,          /* 解析查询参数 */
    PARSER_STATE_PATH_END,       /* 路径后空格 */
    PARSER_STATE_VERSION,        /* 解析版本 */
    PARSER_STATE_VERSION_CR,     /* 版本后 CR */
    PARSER_STATE_VERSION_LF,     /* 版本后 LF */
    PARSER_STATE_HEADER_NAME,    /* 解析头部名称 */
    PARSER_STATE_HEADER_COLON,   /* 头部冒号 */
    PARSER_STATE_HEADER_VALUE,   /* 解析头部值 */
    PARSER_STATE_HEADER_CR,      /* 头部值后 CR */
    PARSER_STATE_HEADER_LF,      /* 头部值后 LF */
    PARSER_STATE_HEADERS_END_CR, /* 空行 CR */
    PARSER_STATE_HEADERS_END_LF, /* 空行 LF（头部结束） */
    PARSER_STATE_BODY,           /* 解析 body */
    PARSER_STATE_COMPLETE        /* 解析完成 */
} ParserState;

/* HttpParser 结构体 */
struct HttpParser {
    ParserState state;           /* 当前解析状态 */
    size_t parsed_len;           /* 已解析长度 */

    /* 临时缓冲区（用于增量解析） */
    char *method_buf;            /* 方法缓冲区 */
    int method_len;
    int method_capacity;

    char *path_buf;              /* 路径缓冲区 */
    int path_len;
    int path_capacity;

    char *query_buf;             /* 查询参数缓冲区 */
    int query_len;
    int query_capacity;

    char *version_buf;           /* 版本缓冲区 */
    int version_len;
    int version_capacity;

    char *header_name_buf;       /* 头部名称缓冲区 */
    int header_name_len;
    int header_name_capacity;

    char *header_value_buf;      /* 头部值缓冲区 */
    int header_value_len;
    int header_value_capacity;

    size_t body_received;        /* 已接收 body 长度 */
};

/* ========== 辅助函数 ========== */

static HttpMethod parse_method(const char *method, int len) {
    if (len == 3 && strncmp(method, "GET", 3) == 0) return HTTP_GET;
    if (len == 4 && strncmp(method, "POST", 4) == 0) return HTTP_POST;
    if (len == 3 && strncmp(method, "PUT", 3) == 0) return HTTP_PUT;
    if (len == 6 && strncmp(method, "DELETE", 6) == 0) return HTTP_DELETE;
    if (len == 4 && strncmp(method, "HEAD", 4) == 0) return HTTP_HEAD;
    if (len == 7 && strncmp(method, "OPTIONS", 7) == 0) return HTTP_OPTIONS;
    if (len == 5 && strncmp(method, "PATCH", 5) == 0) return HTTP_PATCH;
    return HTTP_GET;  /* 默认 */
}

static char *strndup_custom(const char *s, size_t n) {
    if (n == 0) return NULL;
    char *dup = malloc(n + 1);
    if (!dup) return NULL;
    memcpy(dup, s, n);
    dup[n] = '\0';
    return dup;
}

static int append_to_buffer(char **buf, int *len, int *capacity, char c) {
    if (*len >= *capacity) {
        int new_cap = *capacity * 2;
        if (new_cap < 16) new_cap = 16;
        char *new_buf = realloc(*buf, new_cap);
        if (!new_buf) return -1;
        *buf = new_buf;
        *capacity = new_cap;
    }
    (*buf)[(*len)++] = c;
    return 0;
}

static int add_header(HttpRequest *req, const char *name, int name_len,
                      const char *value, int value_len) {
    if (req->header_count >= req->header_capacity) {
        int new_cap = req->header_capacity * 2;
        if (new_cap < 16) new_cap = 16;
        HttpHeader *new_headers = realloc(req->headers, new_cap * sizeof(HttpHeader));
        if (!new_headers) return -1;
        req->headers = new_headers;
        req->header_capacity = new_cap;
    }

    req->headers[req->header_count].name = strndup_custom(name, name_len);
    req->headers[req->header_count].value = strndup_custom(value, value_len);

    if (!req->headers[req->header_count].name ||
        !req->headers[req->header_count].value) {
        return -1;
    }

    req->header_count++;
    return 0;
}

/* ========== 公共 API 实现 ========== */

HttpParser *http_parser_create(void) {
    HttpParser *parser = malloc(sizeof(HttpParser));
    if (!parser) return NULL;

    parser->state = PARSER_STATE_INIT;
    parser->parsed_len = 0;

    /* 初始化缓冲区 */
    parser->method_capacity = 16;
    parser->method_buf = malloc(parser->method_capacity);
    parser->method_len = 0;

    parser->path_capacity = 256;
    parser->path_buf = malloc(parser->path_capacity);
    parser->path_len = 0;

    parser->query_capacity = 256;
    parser->query_buf = malloc(parser->query_capacity);
    parser->query_len = 0;

    parser->version_capacity = 16;
    parser->version_buf = malloc(parser->version_capacity);
    parser->version_len = 0;

    parser->header_name_capacity = 64;
    parser->header_name_buf = malloc(parser->header_name_capacity);
    parser->header_name_len = 0;

    parser->header_value_capacity = 256;
    parser->header_value_buf = malloc(parser->header_value_capacity);
    parser->header_value_len = 0;

    parser->body_received = 0;

    if (!parser->method_buf || !parser->path_buf || !parser->query_buf ||
        !parser->version_buf || !parser->header_name_buf || !parser->header_value_buf) {
        http_parser_destroy(parser);
        return NULL;
    }

    return parser;
}

void http_parser_destroy(HttpParser *parser) {
    if (!parser) return;
    free(parser->method_buf);
    free(parser->path_buf);
    free(parser->query_buf);
    free(parser->version_buf);
    free(parser->header_name_buf);
    free(parser->header_value_buf);
    free(parser);
}

HttpRequest *http_request_create(void) {
    HttpRequest *req = malloc(sizeof(HttpRequest));
    if (!req) return NULL;

    req->method = HTTP_GET;
    req->path = NULL;
    req->query = NULL;
    req->version = NULL;
    req->headers = malloc(8 * sizeof(HttpHeader));
    req->header_count = 0;
    req->header_capacity = 8;
    req->body = NULL;
    req->body_length = 0;
    req->content_length = 0;

    if (!req->headers) {
        free(req);
        return NULL;
    }

    return req;
}

void http_request_destroy(HttpRequest *req) {
    if (!req) return;

    free(req->path);
    free(req->query);
    free(req->version);

    for (int i = 0; i < req->header_count; i++) {
        free(req->headers[i].name);
        free(req->headers[i].value);
    }
    free(req->headers);

    free(req->body);
    free(req);
}

/* ========== 增量状态机解析 ========== */

ParseResult http_parser_parse(HttpParser *parser, HttpRequest *req,
                              const char *data, size_t len, size_t *consumed) {
    if (!parser || !req || !data) return PARSE_ERROR;

    *consumed = 0;
    size_t pos = 0;

    while (pos < len) {
        char c = data[pos];
        int ret = 0;

        switch (parser->state) {
        case PARSER_STATE_INIT:
            parser->state = PARSER_STATE_METHOD;
            parser->method_len = 0;
            /* 继续处理当前字符 */
            continue;

        case PARSER_STATE_METHOD:
            if (c == ' ') {
                /* 方法结束 */
                req->method = parse_method(parser->method_buf, parser->method_len);
                parser->state = PARSER_STATE_METHOD_END;
            } else {
                ret = append_to_buffer(&parser->method_buf, &parser->method_len,
                                       &parser->method_capacity, c);
                if (ret < 0) return PARSE_ERROR;
            }
            break;

        case PARSER_STATE_METHOD_END:
            /* 跳过方法后的空格 */
            if (c != ' ') {
                parser->state = PARSER_STATE_PATH;
                parser->path_len = 0;
                /* 当前字符是路径开始，不跳过 */
                continue;
            }
            break;

        case PARSER_STATE_PATH:
            if (c == '?') {
                /* 开始查询参数 */
                parser->state = PARSER_STATE_QUERY;
                parser->query_len = 0;
            } else if (c == ' ') {
                /* 路径结束 */
                parser->state = PARSER_STATE_PATH_END;
            } else {
                ret = append_to_buffer(&parser->path_buf, &parser->path_len,
                                       &parser->path_capacity, c);
                if (ret < 0) return PARSE_ERROR;
            }
            break;

        case PARSER_STATE_QUERY:
            if (c == ' ') {
                /* 查询参数结束 */
                parser->state = PARSER_STATE_PATH_END;
            } else {
                ret = append_to_buffer(&parser->query_buf, &parser->query_len,
                                       &parser->query_capacity, c);
                if (ret < 0) return PARSE_ERROR;
            }
            break;

        case PARSER_STATE_PATH_END:
            /* 跳过路径后的空格 */
            if (c != ' ') {
                parser->state = PARSER_STATE_VERSION;
                parser->version_len = 0;
                /* 当前字符是版本开始，不跳过 */
                continue;
            }
            break;

        case PARSER_STATE_VERSION:
            if (c == '\r') {
                /* 版本结束 */
                parser->state = PARSER_STATE_VERSION_CR;
            } else {
                ret = append_to_buffer(&parser->version_buf, &parser->version_len,
                                       &parser->version_capacity, c);
                if (ret < 0) return PARSE_ERROR;
            }
            break;

        case PARSER_STATE_VERSION_CR:
            if (c == '\n') {
                parser->state = PARSER_STATE_VERSION_LF;
            } else {
                return PARSE_ERROR;
            }
            break;

        case PARSER_STATE_VERSION_LF:
            /* 开始解析头部 */
            if (c == '\r') {
                /* 可能是空行（头部结束） */
                parser->state = PARSER_STATE_HEADERS_END_CR;
            } else {
                parser->state = PARSER_STATE_HEADER_NAME;
                parser->header_name_len = 0;
                /* 当前字符是头部名称开始，不跳过 */
                continue;
            }
            break;

        case PARSER_STATE_HEADER_NAME:
            if (c == ':') {
                parser->state = PARSER_STATE_HEADER_COLON;
            } else if (c == '\r') {
                /* 空行，头部结束 */
                parser->state = PARSER_STATE_HEADERS_END_CR;
            } else {
                ret = append_to_buffer(&parser->header_name_buf, &parser->header_name_len,
                                       &parser->header_name_capacity, c);
                if (ret < 0) return PARSE_ERROR;
            }
            break;

        case PARSER_STATE_HEADER_COLON:
            /* 跳过冒号后的空格 */
            if (c == ' ') {
                /* 继续跳过 */
            } else {
                parser->state = PARSER_STATE_HEADER_VALUE;
                parser->header_value_len = 0;
                /* 当前字符是头部值开始，不跳过 */
                continue;
            }
            break;

        case PARSER_STATE_HEADER_VALUE:
            if (c == '\r') {
                /* 头部值结束 */
                parser->state = PARSER_STATE_HEADER_CR;
            } else {
                ret = append_to_buffer(&parser->header_value_buf, &parser->header_value_len,
                                       &parser->header_value_capacity, c);
                if (ret < 0) return PARSE_ERROR;
            }
            break;

        case PARSER_STATE_HEADER_CR:
            if (c == '\n') {
                /* 保存头部 */
                add_header(req, parser->header_name_buf, parser->header_name_len,
                           parser->header_value_buf, parser->header_value_len);

                /* 检查 Content-Length */
                if (strcasecmp(parser->header_name_buf, "Content-Length") == 0) {
                    req->content_length = strtoul(parser->header_value_buf, NULL, 10);
                }

                parser->state = PARSER_STATE_HEADER_LF;
            } else {
                return PARSE_ERROR;
            }
            break;

        case PARSER_STATE_HEADER_LF:
            /* 开始下一个头部或检查结束 */
            if (c == '\r') {
                /* 可能是空行（头部结束） */
                parser->state = PARSER_STATE_HEADERS_END_CR;
            } else {
                parser->state = PARSER_STATE_HEADER_NAME;
                parser->header_name_len = 0;
                /* 当前字符是下一个头部名称开始，不跳过 */
                continue;
            }
            break;

        case PARSER_STATE_HEADERS_END_CR:
            if (c == '\n') {
                /* 头部结束 */
                parser->state = PARSER_STATE_HEADERS_END_LF;

                /* 完成请求行和头部解析 */
                req->path = strndup_custom(parser->path_buf, parser->path_len);
                req->query = parser->query_len > 0 ?
                             strndup_custom(parser->query_buf, parser->query_len) : NULL;
                req->version = strndup_custom(parser->version_buf, parser->version_len);

                /* 检查是否有 body */
                if (req->content_length > 0) {
                    parser->state = PARSER_STATE_BODY;
                    parser->body_received = 0;
                    req->body = malloc(req->content_length + 1);
                    if (!req->body) return PARSE_ERROR;
                } else {
                    parser->state = PARSER_STATE_COMPLETE;
                }
            } else {
                return PARSE_ERROR;
            }
            break;

        case PARSER_STATE_HEADERS_END_LF:
            /* 这个状态只在无 body 时进入，立即完成 */
            parser->state = PARSER_STATE_COMPLETE;
            break;

        case PARSER_STATE_BODY:
            /* 收集 body 数据 */
            if (parser->body_received < req->content_length) {
                req->body[parser->body_received++] = c;
                if (parser->body_received >= req->content_length) {
                    req->body[parser->body_received] = '\0';
                    req->body_length = parser->body_received;
                    parser->state = PARSER_STATE_COMPLETE;
                }
            }
            break;

        case PARSER_STATE_COMPLETE:
            /* 解析已完成 */
            pos++;  /* 消耗当前字符 */
            *consumed = pos;
            parser->parsed_len += pos;
            return PARSE_COMPLETE;
        }

        pos++;  /* 消耗当前字符 */
    }

    *consumed = pos;
    parser->parsed_len += pos;

    /* 返回当前状态 */
    if (parser->state == PARSER_STATE_COMPLETE) {
        return PARSE_COMPLETE;
    }
    return PARSE_NEED_MORE;
}

HttpHeader *http_request_get_header(HttpRequest *req, const char *name) {
    if (!req || !name) return NULL;

    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0) {
            return &req->headers[i];
        }
    }
    return NULL;
}

const char *http_request_get_header_value(HttpRequest *req, const char *name) {
    HttpHeader *header = http_request_get_header(req, name);
    if (!header) return NULL;
    return header->value;
}

void http_parser_reset(HttpParser *parser) {
    if (!parser) return;

    parser->state = PARSER_STATE_INIT;
    parser->parsed_len = 0;
    parser->method_len = 0;
    parser->path_len = 0;
    parser->query_len = 0;
    parser->version_len = 0;
    parser->header_name_len = 0;
    parser->header_value_len = 0;
    parser->body_received = 0;
}