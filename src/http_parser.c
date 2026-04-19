#include "http_parser.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* HttpParser 结构体 */
struct HttpParser {
    int state;              /* 解析状态 */
    size_t parsed_len;      /* 已解析长度 */
    char *method_str;       /* 方法字符串缓冲 */
    int method_len;
};

/* 解析状态 */
enum ParserState {
    PARSER_STATE_METHOD,
    PARSER_STATE_PATH,
    PARSER_STATE_VERSION,
    PARSER_STATE_HEADER_NAME,
    PARSER_STATE_HEADER_VALUE,
    PARSER_STATE_BODY,
    PARSER_STATE_COMPLETE
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
    char *dup = malloc(n + 1);
    if (!dup) return NULL;
    memcpy(dup, s, n);
    dup[n] = '\0';
    return dup;
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

    parser->state = PARSER_STATE_METHOD;
    parser->parsed_len = 0;
    parser->method_str = malloc(16);
    parser->method_len = 0;

    return parser;
}

void http_parser_destroy(HttpParser *parser) {
    if (!parser) return;
    free(parser->method_str);
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

ParseResult http_parser_parse(HttpParser *parser, HttpRequest *req,
                              const char *data, size_t len, size_t *consumed) {
    if (!parser || !req || !data) return PARSE_ERROR;

    size_t pos = 0;
    *consumed = 0;

    /* 简化版解析器：只处理完整请求 */
    /* 查找请求结束标记 */
    const char *header_end = strstr(data, "\r\n\r\n");
    if (!header_end) {
        return PARSE_NEED_MORE;  /* 需要更多数据 */
    }

    size_t header_len = header_end - data + 4;  /* 包含 \r\n\r\n */
    *consumed = header_len;

    /* 解析请求行 */
    const char *line_start = data;
    const char *line_end = strstr(data, "\r\n");
    if (!line_end) return PARSE_ERROR;

    /* 方法 */
    const char *space1 = strchr(line_start, ' ');
    if (!space1) return PARSE_ERROR;
    int method_len = space1 - line_start;
    req->method = parse_method(line_start, method_len);

    /* 路径 */
    const char *path_start = space1 + 1;
    const char *space2 = strchr(path_start, ' ');
    if (!space2) return PARSE_ERROR;

    /* 查找 query */
    const char *query_start = strchr(path_start, '?');
    if (query_start && query_start < space2) {
        req->path = strndup_custom(path_start, query_start - path_start);
        req->query = strndup_custom(query_start + 1, space2 - query_start - 1);
    } else {
        req->path = strndup_custom(path_start, space2 - path_start);
        req->query = NULL;
    }

    /* 版本 */
    const char *version_start = space2 + 1;
    req->version = strndup_custom(version_start, line_end - version_start);

    /* 解析头部 */
    const char *header_line = line_end + 2;
    while (header_line < header_end) {
        const char *next_line = strstr(header_line, "\r\n");
        if (!next_line) break;

        if (header_line == next_line) break;  /* 空行 */

        const char *colon = strchr(header_line, ':');
        if (!colon) {
            header_line = next_line + 2;
            continue;
        }

        /* 头部名称 */
        int name_len = colon - header_line;

        /* 头部值（跳过冒号后的空格） */
        const char *value_start = colon + 1;
        while (*value_start == ' ' && value_start < next_line) {
            value_start++;
        }
        int value_len = next_line - value_start;

        add_header(req, header_line, name_len, value_start, value_len);

        /* Content-Length 特殊处理 */
        if (strncmp(header_line, "Content-Length", name_len) == 0 ||
            strncmp(header_line, "content-length", name_len) == 0) {
            req->content_length = strtoul(value_start, NULL, 10);
        }

        header_line = next_line + 2;
    }

    /* 解析 body（如果有） */
    if (req->content_length > 0 && len > header_len) {
        size_t body_avail = len - header_len;
        if (body_avail >= req->content_length) {
            req->body = strndup_custom(header_end + 4, req->content_length);
            req->body_length = req->content_length;
            *consumed = header_len + req->content_length;
        } else {
            req->body = strndup_custom(header_end + 4, body_avail);
            req->body_length = body_avail;
            *consumed = len;
            return PARSE_NEED_MORE;
        }
    }

    return PARSE_COMPLETE;
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
    parser->state = PARSER_STATE_METHOD;
    parser->parsed_len = 0;
    parser->method_len = 0;
}