/**
 * @file    http_parser.h
 * @brief   HTTP/1.1 请求解析器，增量式状态机实现
 *
 * @details
 *          - 增量解析，支持流式输入
 *          - 状态机驱动，解析与 I/O 解耦
 *          - 支持 GET/POST/PUT/DELETE/HEAD/OPTIONS/PATCH
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

/* Phase 5: 解压结果 */
typedef enum {
    DECOMPRESS_OK,             /* 解压成功 */
    DECOMPRESS_NOT_NEEDED,     /* 无需解压 */
    DECOMPRESS_ERROR,          /* 解压错误 */
    DECOMPRESS_ZIP_BOMB,       /* Zip Bomb 检测 */
    DECOMPRESS_SIZE_EXCEEDED   /* 解压后大小超限 */
} DecompressResult;

/* HTTP 头部 */
typedef struct HttpHeader {
    char *name;
    char *value;
} HttpHeader;

/* Phase 5: 解压配置 */
typedef struct DecompressConfig {
    size_t max_decompressed_size;   /* 解压后最大大小 */
    double max_ratio;               /* 压缩比上限（解压/原始） */
    bool enable_gzip;               /* 启用 gzip 解压 */
    bool enable_deflate;            /* 启用 deflate 解压 */
} DecompressConfig;

/* 默认解压配置 */
#define DECOMPRESS_DEFAULT_MAX_SIZE       (10 * 1024 * 1024)  /* 10MB */
#define DECOMPRESS_DEFAULT_MAX_RATIO      100.0               /* 100:1 */

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
    /* Phase 5: 解压相关字段 */
    bool needs_decompression;   /* 是否需要解压 */
    const char *content_encoding; /* Content-Encoding 头部值 */
    DecompressResult decompress_result; /* 解压结果 */
    size_t original_body_size;  /* 原始 body 大小（压缩前） */
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

/* ========== Phase 5: Gzip/Deflate 解压 API ========== */

/**
 * 设置解压配置
 * @param parser HttpParser 指针
 * @param config 解压配置
 * @return 0 成功，-1 失败
 */
int http_parser_set_decompress_config(HttpParser *parser, const DecompressConfig *config);

/**
 * 解压请求 body
 * @param req HttpRequest 指针
 * @param parser HttpParser 指针（用于获取配置）
 * @return DecompressResult
 */
DecompressResult http_request_decompress_body(HttpRequest *req, HttpParser *parser);

/**
 * 检测 Zip Bomb
 * @param original_size 原始（压缩）大小
 * @param decompressed_size 解压后大小
 * @param max_ratio 最大允许压缩比
 * @return true 为 Zip Bomb，false 安全
 */
bool http_detect_zip_bomb(size_t original_size, size_t decompressed_size, double max_ratio);

/**
 * 获取 Content-Encoding 头部值
 * @param req HttpRequest 指针
 * @return Content-Encoding 值，无则返回 NULL
 */
const char *http_request_get_content_encoding(HttpRequest *req);

/**
 * 检查请求是否需要解压
 * @param req HttpRequest 指针
 * @return true 需要解压
 */
bool http_request_needs_decompression(HttpRequest *req);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_HTTP_PARSER_H */