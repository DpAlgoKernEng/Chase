/**
 * @file    http_parser.c
 * @brief   HTTP/1.1 请求解析器实现
 *
 * @details
 *          - 增量式状态机解析
 *          - 支持流式输入
 *          - 解析请求行、头部、响应体
 *          - Phase 5: gzip/deflate 解压和 Zip Bomb 检测
 *
 * @layer   Core Layer
 *
 * @depends zlib (Phase 5)
 * @usedby  server, handler, router
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "http_parser.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <zlib.h>  /* Phase 5: gzip/deflate 解压 */

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
    PARSER_STATE_HEADERS_END_CR, /* 空行 CR（头部结束） */
    PARSER_STATE_BODY,           /* 解析 body */
    /* Phase 3: Chunked 编码状态 */
    PARSER_STATE_CHUNK_SIZE,     /* 解析 chunk 大小 */
    PARSER_STATE_CHUNK_SIZE_CR,  /* chunk 大小后 CR */
    PARSER_STATE_CHUNK_SIZE_LF,  /* chunk 大小后 LF */
    PARSER_STATE_CHUNK_DATA,     /* 解析 chunk 数据 */
    PARSER_STATE_CHUNK_DATA_CR,  /* chunk 数据后 CR */
    PARSER_STATE_CHUNK_DATA_LF,  /* chunk 数据后 LF */
    PARSER_STATE_CHUNK_TRAILER_CR, /* chunk trailer CR */
    PARSER_STATE_CHUNK_TRAILER_LF, /* chunk trailer LF */
    PARSER_STATE_CHUNK_SKIP_TRAILER_LINE, /* 跳过 trailer 行 */
    PARSER_STATE_CHUNK_SKIP_TRAILER_LF,   /* trailer 行 LF */
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

    /* Phase 3: Chunked 编码支持 */
    char *chunk_size_buf;        /* chunk 大小缓冲区 */
    int chunk_size_len;
    int chunk_size_capacity;
    size_t chunk_size;           /* 当前 chunk 大小 */
    size_t chunk_received;       /* 当前 chunk 已接收字节数 */

    /* Phase 5: 解压配置 */
    DecompressConfig decompress_config;
    bool decompress_config_set;  /* 是否已设置解压配置 */
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

/* 安全解析 Content-Length，防止溢出 */
static size_t safe_parse_content_length(const char *value, int len) {
    if (len <= 0 || len > 20) {
        /* 超过 20 位数字的值不合理，拒绝 */
        return 0;
    }

    /* 检查是否全是数字 */
    for (int i = 0; i < len; i++) {
        if (!isdigit((unsigned char)value[i])) {
            return 0;
        }
    }

    /* 创建临时 null-terminated 字符串进行解析 */
    char temp[21];
    memcpy(temp, value, len);
    temp[len] = '\0';

    /* 使用 strtoul 并检查溢出 */
    char *endptr = NULL;
    errno = 0;
    unsigned long result = strtoul(temp, &endptr, 10);

    /* 检查解析是否完成 */
    if (endptr == NULL || *endptr != '\0') {
        return 0;
    }

    /* 检查溢出 */
    if (errno == ERANGE || result > SIZE_MAX) {
        return 0;
    }

    /* 设置合理上限（100MB） */
    if (result > 100 * 1024 * 1024) {
        return 0;
    }

    return (size_t)result;
}

static char *strndup_custom(const char *s, size_t n) {
    if (n == 0) return NULL;
    char *dup = malloc(n + 1);
    if (!dup) return NULL;
    memcpy(dup, s, n);
    dup[n] = '\0';
    return dup;
}

/* HTTP 头部最大长度限制 */
#define MAX_HEADER_SIZE     (64 * 1024)   /* 64KB */
#define MAX_METHOD_SIZE     32
#define MAX_PATH_SIZE       8192
#define MAX_QUERY_SIZE      8192
#define MAX_VERSION_SIZE    16

static int append_to_buffer(char **buf, int *len, int *capacity, char c, int max_size) {
    if (*len >= max_size) {
        return -1;  /* 超过最大限制 */
    }

    if (*len >= *capacity) {
        int new_cap = *capacity * 2;
        if (new_cap < 16) new_cap = 16;
        if (new_cap > max_size) new_cap = max_size;
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

    /* Phase 3: Chunked 编码支持初始化 */
    parser->chunk_size_capacity = 16;
    parser->chunk_size_buf = malloc(parser->chunk_size_capacity);
    parser->chunk_size_len = 0;
    parser->chunk_size = 0;
    parser->chunk_received = 0;

    /* Phase 5: 解压配置初始化（使用默认值） */
    parser->decompress_config.max_decompressed_size = DECOMPRESS_DEFAULT_MAX_SIZE;
    parser->decompress_config.max_ratio = DECOMPRESS_DEFAULT_MAX_RATIO;
    parser->decompress_config.enable_gzip = true;
    parser->decompress_config.enable_deflate = true;
    parser->decompress_config_set = false;

    if (!parser->method_buf || !parser->path_buf || !parser->query_buf ||
        !parser->version_buf || !parser->header_name_buf || !parser->header_value_buf ||
        !parser->chunk_size_buf) {
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
    free(parser->chunk_size_buf);  /* Phase 3: Chunked 编码 */
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
    req->is_chunked = false;        /* Phase 3: Chunked 编码 */
    req->chunk_body_capacity = 0;   /* Phase 3: Chunked body 缓冲区 */
    /* Phase 5: 解压相关字段初始化 */
    req->needs_decompression = false;
    req->content_encoding = NULL;
    req->decompress_result = DECOMPRESS_NOT_NEEDED;
    req->original_body_size = 0;

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
    if (!parser || !req || !data) {
        *consumed = 0;
        return PARSE_ERROR;
    }

    *consumed = 0;
    size_t pos = 0;
    ParseResult result = PARSE_NEED_MORE;  /* 使用变量追踪结果 */

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
                                       &parser->method_capacity, c, MAX_METHOD_SIZE);
                if (ret < 0) {
                    *consumed = pos;
                    parser->parsed_len += pos;
                    return PARSE_ERROR;
                }
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
                                       &parser->path_capacity, c, MAX_PATH_SIZE);
                if (ret < 0) {
                    *consumed = pos;
                    parser->parsed_len += pos;
                    return PARSE_ERROR;
                }
            }
            break;

        case PARSER_STATE_QUERY:
            if (c == ' ') {
                /* 查询参数结束 */
                parser->state = PARSER_STATE_PATH_END;
            } else {
                ret = append_to_buffer(&parser->query_buf, &parser->query_len,
                                       &parser->query_capacity, c, MAX_QUERY_SIZE);
                if (ret < 0) {
                    *consumed = pos;
                    parser->parsed_len += pos;
                    return PARSE_ERROR;
                }
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
                                       &parser->version_capacity, c, MAX_VERSION_SIZE);
                if (ret < 0) {
                    *consumed = pos;
                    parser->parsed_len += pos;
                    return PARSE_ERROR;
                }
            }
            break;

        case PARSER_STATE_VERSION_CR:
            if (c == '\n') {
                parser->state = PARSER_STATE_VERSION_LF;
            } else {
                *consumed = pos;
                parser->parsed_len += pos;
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
                                       &parser->header_name_capacity, c, MAX_HEADER_SIZE);
                if (ret < 0) {
                    *consumed = pos;
                    parser->parsed_len += pos;
                    return PARSE_ERROR;
                }
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
                                       &parser->header_value_capacity, c, MAX_HEADER_SIZE);
                if (ret < 0) {
                    *consumed = pos;
                    parser->parsed_len += pos;
                    return PARSE_ERROR;
                }
            }
            break;

        case PARSER_STATE_HEADER_CR:
            if (c == '\n') {
                /* 保存头部 */
                add_header(req, parser->header_name_buf, parser->header_name_len,
                           parser->header_value_buf, parser->header_value_len);

                /* 检查 Content-Length（使用安全解析） */
                if (strcasecmp(parser->header_name_buf, "Content-Length") == 0) {
                    req->content_length = safe_parse_content_length(
                        parser->header_value_buf, parser->header_value_len);
                }

                /* Phase 3: 检查 Transfer-Encoding: chunked */
                if (strcasecmp(parser->header_name_buf, "Transfer-Encoding") == 0) {
                    /* 检查值是否包含 "chunked" */
                    for (int i = 0; i < parser->header_value_len; i++) {
                        if (strncasecmp(parser->header_value_buf + i, "chunked", 7) == 0) {
                            req->is_chunked = true;
                            break;
                        }
                    }
                }

                parser->state = PARSER_STATE_HEADER_LF;
            } else {
                *consumed = pos;
                parser->parsed_len += pos;
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
                /* 完成请求行和头部解析 */
                req->path = strndup_custom(parser->path_buf, parser->path_len);
                req->query = parser->query_len > 0 ?
                             strndup_custom(parser->query_buf, parser->query_len) : NULL;
                req->version = strndup_custom(parser->version_buf, parser->version_len);

                /* Phase 3: 检查是否有 body */
                if (req->is_chunked) {
                    /* 使用 chunked 编码，开始解析 chunk */
                    parser->state = PARSER_STATE_CHUNK_SIZE;
                    parser->chunk_size_len = 0;
                    parser->chunk_size = 0;
                    parser->chunk_received = 0;
                    /* 初始化 chunked body 缓冲区 */
                    req->body = malloc(1024);
                    req->chunk_body_capacity = 1024;
                    req->body_length = 0;
                    if (!req->body) {
                        *consumed = pos;
                        parser->parsed_len += pos;
                        return PARSE_ERROR;
                    }
                } else if (req->content_length > 0) {
                    /* 使用 Content-Length */
                    parser->state = PARSER_STATE_BODY;
                    parser->body_received = 0;
                    req->body = malloc(req->content_length + 1);
                    if (!req->body) {
                        *consumed = pos;
                        parser->parsed_len += pos;
                        return PARSE_ERROR;
                    }
                } else {
                    /* 无 body，解析完成 */
                    parser->state = PARSER_STATE_COMPLETE;
                }
            } else {
                *consumed = pos;
                parser->parsed_len += pos;
                return PARSE_ERROR;
            }
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

        /* Phase 3: Chunked 编码解析 */
        case PARSER_STATE_CHUNK_SIZE:
            /* 解析 chunk 大小（十六进制） */
            if (c == '\r') {
                /* chunk 大小结束 */
                /* 解析十六进制 chunk 大小 */
                if (parser->chunk_size_len > 0) {
                    char temp[32];
                    memcpy(temp, parser->chunk_size_buf, parser->chunk_size_len);
                    temp[parser->chunk_size_len] = '\0';
                    parser->chunk_size = strtoul(temp, NULL, 16);
                } else {
                    parser->chunk_size = 0;
                }
                parser->state = PARSER_STATE_CHUNK_SIZE_CR;
            } else if (c == ';') {
                /* chunk 扩展开始 - 先解析已收集的 chunk 大小 */
                if (parser->chunk_size_len > 0) {
                    char temp[32];
                    memcpy(temp, parser->chunk_size_buf, parser->chunk_size_len);
                    temp[parser->chunk_size_len] = '\0';
                    parser->chunk_size = strtoul(temp, NULL, 16);
                } else {
                    parser->chunk_size = 0;
                }
                parser->state = PARSER_STATE_CHUNK_SIZE_CR;
            } else if (isxdigit((unsigned char)c)) {
                /* 十六进制数字 */
                ret = append_to_buffer(&parser->chunk_size_buf, &parser->chunk_size_len,
                                       &parser->chunk_size_capacity, c, 32);
                if (ret < 0) {
                    *consumed = pos;
                    parser->parsed_len += pos;
                    return PARSE_ERROR;
                }
            } else if (c != ' ') {
                /* 跳过空格，其他字符为错误 */
                *consumed = pos;
                parser->parsed_len += pos;
                return PARSE_ERROR;
            }
            break;

        case PARSER_STATE_CHUNK_SIZE_CR:
            /* 等待 chunk-size 行的 LF，或跳过 chunk 扩展参数 */
            if (c == '\n') {
                parser->state = PARSER_STATE_CHUNK_SIZE_LF;
            } else {
                /* 跳过 chunk 扩展参数（;name=value 等）直到 LF */
                /* 保持当前状态继续消耗字符 */
            }
            break;

        case PARSER_STATE_CHUNK_SIZE_LF:
            /* 开始读取 chunk 数据 */
            if (parser->chunk_size == 0) {
                /* 最后一个 chunk（大小为 0），开始 trailer */
                parser->state = PARSER_STATE_CHUNK_TRAILER_CR;
                /* 使用 continue 处理当前字符在 CHUNK_TRAILER_CR 状态 */
                continue;
            } else {
                parser->state = PARSER_STATE_CHUNK_DATA;
                parser->chunk_received = 0;
                /* 当前字符可能是 chunk 数据，使用 continue 处理 */
                continue;
            }
            break;

        case PARSER_STATE_CHUNK_DATA:
            /* 读取 chunk 数据 */
            if (parser->chunk_received < parser->chunk_size) {
                /* 确保 body 缓冲区足够大 */
                if (req->body_length + 1 >= req->chunk_body_capacity) {
                    size_t new_cap = req->chunk_body_capacity * 2;
                    char *new_buf = realloc(req->body, new_cap);
                    if (!new_buf) {
                        *consumed = pos;
                        parser->parsed_len += pos;
                        return PARSE_ERROR;
                    }
                    req->body = new_buf;
                    req->chunk_body_capacity = new_cap;
                }
                req->body[req->body_length++] = c;
                parser->chunk_received++;
                if (parser->chunk_received >= parser->chunk_size) {
                    parser->state = PARSER_STATE_CHUNK_DATA_CR;
                }
            }
            break;

        case PARSER_STATE_CHUNK_DATA_CR:
            if (c == '\r') {
                parser->state = PARSER_STATE_CHUNK_DATA_LF;
            } else {
                *consumed = pos;
                parser->parsed_len += pos;
                return PARSE_ERROR;
            }
            break;

        case PARSER_STATE_CHUNK_DATA_LF:
            if (c == '\n') {
                /* chunk 结束，准备下一个 chunk */
                parser->state = PARSER_STATE_CHUNK_SIZE;
                parser->chunk_size_len = 0;
            } else {
                *consumed = pos;
                parser->parsed_len += pos;
                return PARSE_ERROR;
            }
            break;

        case PARSER_STATE_CHUNK_TRAILER_CR:
            /* Chunk trailer 行（可选头部）或空行 */
            if (c == '\r') {
                /* 可能是空行结束 */
                parser->state = PARSER_STATE_CHUNK_TRAILER_LF;
            } else {
                /* 有 trailer 行，跳过该行直到 CRLF */
                parser->state = PARSER_STATE_CHUNK_SKIP_TRAILER_LINE;
            }
            break;

        case PARSER_STATE_CHUNK_SKIP_TRAILER_LINE:
            /* 跳过 trailer 行内容 */
            if (c == '\r') {
                parser->state = PARSER_STATE_CHUNK_SKIP_TRAILER_LF;
            }
            /* 否则继续跳过 trailer 行字符 */
            break;

        case PARSER_STATE_CHUNK_SKIP_TRAILER_LF:
            if (c == '\n') {
                /* trailer 行结束，回到 CHUNK_TRAILER_CR 看是否有更多 trailer */
                parser->state = PARSER_STATE_CHUNK_TRAILER_CR;
            } else {
                *consumed = pos;
                parser->parsed_len += pos;
                return PARSE_ERROR;
            }
            break;

        case PARSER_STATE_CHUNK_TRAILER_LF:
            if (c == '\n') {
                /* trailer 结束，chunked body 完成 */
                req->body[req->body_length] = '\0';
                parser->state = PARSER_STATE_COMPLETE;
            } else {
                *consumed = pos;
                parser->parsed_len += pos;
                return PARSE_ERROR;
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

    /* 重置长度并清空缓冲区内容 */
    parser->method_len = 0;
    if (parser->method_buf) parser->method_buf[0] = '\0';

    parser->path_len = 0;
    if (parser->path_buf) parser->path_buf[0] = '\0';

    parser->query_len = 0;
    if (parser->query_buf) parser->query_buf[0] = '\0';

    parser->version_len = 0;
    if (parser->version_buf) parser->version_buf[0] = '\0';

    parser->header_name_len = 0;
    if (parser->header_name_buf) parser->header_name_buf[0] = '\0';

    parser->header_value_len = 0;
    if (parser->header_value_buf) parser->header_value_buf[0] = '\0';

    parser->body_received = 0;

    /* Phase 3: 重置 chunk 状态 */
    parser->chunk_size_len = 0;
    if (parser->chunk_size_buf) parser->chunk_size_buf[0] = '\0';
    parser->chunk_size = 0;
    parser->chunk_received = 0;
}

/* ========== Phase 5: Gzip/Deflate 解压实现 ========== */

int http_parser_set_decompress_config(HttpParser *parser, const DecompressConfig *config) {
    if (!parser || !config) return -1;

    parser->decompress_config = *config;
    parser->decompress_config_set = true;

    /* 设置合理的默认值 */
    if (parser->decompress_config.max_decompressed_size <= 0) {
        parser->decompress_config.max_decompressed_size = DECOMPRESS_DEFAULT_MAX_SIZE;
    }
    if (parser->decompress_config.max_ratio <= 0) {
        parser->decompress_config.max_ratio = DECOMPRESS_DEFAULT_MAX_RATIO;
    }

    return 0;
}

const char *http_request_get_content_encoding(HttpRequest *req) {
    if (!req) return NULL;

    /* 如果已经缓存，直接返回 */
    if (req->content_encoding) {
        return req->content_encoding;
    }

    /* 从头部获取 */
    return http_request_get_header_value(req, "Content-Encoding");
}

bool http_request_needs_decompression(HttpRequest *req) {
    if (!req) return false;

    /* 已经标记 */
    if (req->needs_decompression) {
        return true;
    }

    /* 检查 Content-Encoding 头部 */
    const char *encoding = http_request_get_content_encoding(req);
    if (!encoding) {
        return false;
    }

    /* 检查是否支持 gzip 或 deflate */
    if (strstr(encoding, "gzip") != NULL || strstr(encoding, "deflate") != NULL) {
        req->needs_decompression = true;
        return true;
    }

    return false;
}

bool http_detect_zip_bomb(size_t original_size, size_t decompressed_size, double max_ratio) {
    if (original_size == 0) {
        /* 原始大小为 0，解压后有数据 → 肯定是 zip bomb */
        return decompressed_size > 0;
    }

    double ratio = (double)decompressed_size / (double)original_size;
    return ratio > max_ratio;
}

DecompressResult http_request_decompress_body(HttpRequest *req, HttpParser *parser) {
    if (!req || !parser) return DECOMPRESS_ERROR;

    /* 检查是否需要解压 */
    if (!http_request_needs_decompression(req)) {
        return DECOMPRESS_NOT_NEEDED;
    }

    /* 检查是否有 body */
    if (!req->body || req->body_length == 0) {
        return DECOMPRESS_NOT_NEEDED;
    }

    const char *encoding = http_request_get_content_encoding(req);
    if (!encoding) {
        return DECOMPRESS_NOT_NEEDED;
    }

    /* 记录原始大小 */
    req->original_body_size = req->body_length;

    /* 获取配置 */
    size_t max_size = parser->decompress_config.max_decompressed_size;
    double max_ratio = parser->decompress_config.max_ratio;

    /* 分配解压缓冲区 */
    size_t buf_capacity = req->body_length * 4;  /* 初始估计 4x 扩展 */
    if (buf_capacity < 1024) buf_capacity = 1024;
    if (buf_capacity > max_size) buf_capacity = max_size;

    char *decompressed = malloc(buf_capacity);
    if (!decompressed) {
        return DECOMPRESS_ERROR;
    }

    size_t decompressed_len = 0;
    DecompressResult result = DECOMPRESS_ERROR;

    /* 判断编码类型并解压 */
    if (strstr(encoding, "gzip") != NULL && parser->decompress_config.enable_gzip) {
        /* gzip 解压 */
        z_stream strm;
        memset(&strm, 0, sizeof(strm));

        /* 初始化 gzip 解压（windowBits = 15 + 16 表示 gzip 格式） */
        int ret = inflateInit2(&strm, 15 + 16);
        if (ret != Z_OK) {
            free(decompressed);
            return DECOMPRESS_ERROR;
        }

        strm.next_in = (Bytef *)req->body;
        strm.avail_in = req->body_length;

        /* 循环解压 */
        while (ret != Z_STREAM_END) {
            /* 检查缓冲区容量 */
            if (decompressed_len >= buf_capacity) {
                /* 扩展缓冲区 */
                size_t new_cap = buf_capacity * 2;
                if (new_cap > max_size) {
                    new_cap = max_size;
                    if (decompressed_len >= max_size) {
                        /* 已达到最大大小 */
                        inflateEnd(&strm);
                        free(decompressed);
                        return DECOMPRESS_SIZE_EXCEEDED;
                    }
                }
                char *new_buf = realloc(decompressed, new_cap);
                if (!new_buf) {
                    inflateEnd(&strm);
                    free(decompressed);
                    return DECOMPRESS_ERROR;
                }
                decompressed = new_buf;
                buf_capacity = new_cap;
            }

            strm.next_out = (Bytef *)(decompressed + decompressed_len);
            strm.avail_out = buf_capacity - decompressed_len;

            ret = inflate(&strm, Z_NO_FLUSH);

            if (ret == Z_DATA_ERROR || ret == Z_MEM_ERROR || ret == Z_BUF_ERROR) {
                inflateEnd(&strm);
                free(decompressed);
                return DECOMPRESS_ERROR;
            }

            /* 输入耗尽但未完成 → 数据不完整 */
            if (strm.avail_in == 0 && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
                /* 如果inflate返回Z_OK但输入耗尽，说明数据不完整 */
                inflateEnd(&strm);
                free(decompressed);
                return DECOMPRESS_ERROR;
            }

            decompressed_len = strm.total_out;
        }

        inflateEnd(&strm);
        result = DECOMPRESS_OK;

    } else if (strstr(encoding, "deflate") != NULL && parser->decompress_config.enable_deflate) {
        /* deflate 解压（raw deflate，无 zlib header） */
        z_stream strm;
        memset(&strm, 0, sizeof(strm));

        /* 初始化 deflate 解压（windowBits = -15 表示 raw deflate） */
        int ret = inflateInit2(&strm, -15);
        if (ret != Z_OK) {
            free(decompressed);
            return DECOMPRESS_ERROR;
        }

        strm.next_in = (Bytef *)req->body;
        strm.avail_in = req->body_length;

        /* 循环解压 */
        while (ret != Z_STREAM_END) {
            /* 检查缓冲区容量 */
            if (decompressed_len >= buf_capacity) {
                size_t new_cap = buf_capacity * 2;
                if (new_cap > max_size) {
                    new_cap = max_size;
                    if (decompressed_len >= max_size) {
                        inflateEnd(&strm);
                        free(decompressed);
                        return DECOMPRESS_SIZE_EXCEEDED;
                    }
                }
                char *new_buf = realloc(decompressed, new_cap);
                if (!new_buf) {
                    inflateEnd(&strm);
                    free(decompressed);
                    return DECOMPRESS_ERROR;
                }
                decompressed = new_buf;
                buf_capacity = new_cap;
            }

            strm.next_out = (Bytef *)(decompressed + decompressed_len);
            strm.avail_out = buf_capacity - decompressed_len;

            ret = inflate(&strm, Z_NO_FLUSH);

            if (ret == Z_DATA_ERROR || ret == Z_MEM_ERROR || ret == Z_BUF_ERROR) {
                inflateEnd(&strm);
                free(decompressed);
                return DECOMPRESS_ERROR;
            }

            /* 输入耗尽但未完成 → 数据不完整 */
            if (strm.avail_in == 0 && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
                inflateEnd(&strm);
                free(decompressed);
                return DECOMPRESS_ERROR;
            }

            decompressed_len = strm.total_out;
        }

        inflateEnd(&strm);
        result = DECOMPRESS_OK;

    } else {
        /* 不支持的编码 */
        free(decompressed);
        return DECOMPRESS_NOT_NEEDED;
    }

    /* Zip Bomb 检测 */
    if (http_detect_zip_bomb(req->original_body_size, decompressed_len, max_ratio)) {
        free(decompressed);
        req->decompress_result = DECOMPRESS_ZIP_BOMB;
        return DECOMPRESS_ZIP_BOMB;
    }

    /* 替换原始 body */
    free(req->body);
    req->body = decompressed;
    req->body_length = decompressed_len;
    req->decompress_result = result;

    /* null terminate */
    if (req->body_length < buf_capacity) {
        req->body[req->body_length] = '\0';
    }

    return result;
}