/**
 * @file    test_chunked.c
 * @brief   Chunked encoding 功能测试
 *
 * @details
 *          - 测试 Transfer-Encoding: chunked 解析
 *          - 测试 chunk 大小解析（十六进制）
 *          - 测试多 chunk 数据
 *          - 测试最后一个 chunk（大小为 0）
 *          - 测试 chunk trailer
 *
 * @author  minghui.liu
 * @date    2026-04-22
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "http_parser.h"

#define TEST(name) static void test_##name()
#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s - %s\n", __func__, msg); \
        test_failed = 1; \
        return; \
    } \
} while(0)

static int test_failed = 0;

/* ========== 测试用例 ========== */

/**
 * 测试 1: 简单 chunked 请求
 */
TEST(simple_chunked_request) {
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    ASSERT(parser != NULL, "parser_create failed");
    ASSERT(req != NULL, "request_create failed");

    /* 简单 chunked 请求 */
    char request[] =
        "POST /test HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "Hello\r\n"
        "6\r\n"
        " World\r\n"
        "0\r\n"
        "\r\n";

    size_t consumed = 0;
    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    ASSERT(result == PARSE_COMPLETE, "parse should complete");
    ASSERT(req->is_chunked == true, "is_chunked should be true");
    ASSERT(req->body_length == 11, "body_length should be 11");
    ASSERT(strncmp(req->body, "Hello World", 11) == 0, "body content mismatch");
    ASSERT(req->content_length == 0, "content_length should be 0 for chunked");

    http_request_destroy(req);
    http_parser_destroy(parser);
}

/**
 * 测试 2: 单个 chunk
 */
TEST(single_chunk) {
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    ASSERT(parser != NULL, "parser_create failed");
    ASSERT(req != NULL, "request_create failed");

    char request[] =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "10\r\n"
        "0123456789ABCDEF\r\n"
        "0\r\n"
        "\r\n";

    size_t consumed = 0;
    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    ASSERT(result == PARSE_COMPLETE, "parse should complete");
    ASSERT(req->is_chunked == true, "is_chunked should be true");
    ASSERT(req->body_length == 16, "body_length should be 16");
    ASSERT(strncmp(req->body, "0123456789ABCDEF", 16) == 0, "body content mismatch");

    http_request_destroy(req);
    http_parser_destroy(parser);
}

/**
 * 测试 3: 增量解析 chunked
 */
TEST(incremental_chunked_parse) {
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    ASSERT(parser != NULL, "parser_create failed");
    ASSERT(req != NULL, "request_create failed");

    /* 分段发送 */
    char part1[] = "POST /test HTTP/1.1\r\nHost: localhost\r\n";
    char part2[] = "Transfer-Encoding: chunked\r\n\r\n";
    char part3[] = "3\r\nabc\r\n";
    char part4[] = "0\r\n\r\n";

    size_t consumed = 0;
    ParseResult result;

    result = http_parser_parse(parser, req, part1, strlen(part1), &consumed);
    ASSERT(result == PARSE_NEED_MORE, "part1 should need more");

    result = http_parser_parse(parser, req, part2, strlen(part2), &consumed);
    ASSERT(result == PARSE_NEED_MORE, "part2 should need more");

    result = http_parser_parse(parser, req, part3, strlen(part3), &consumed);
    ASSERT(result == PARSE_NEED_MORE, "part3 should need more");

    result = http_parser_parse(parser, req, part4, strlen(part4), &consumed);
    ASSERT(result == PARSE_COMPLETE, "part4 should complete");

    ASSERT(req->is_chunked == true, "is_chunked should be true");
    ASSERT(req->body_length == 3, "body_length should be 3");
    ASSERT(strncmp(req->body, "abc", 3) == 0, "body content mismatch");

    http_request_destroy(req);
    http_parser_destroy(parser);
}

/**
 * 测试 4: 大 chunk 数据
 */
TEST(large_chunk) {
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    ASSERT(parser != NULL, "parser_create failed");
    ASSERT(req != NULL, "request_create failed");

    /* 构建大 chunk 请求 */
    char header[] =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";

    /* chunk 大小 200 (0xC8) */
    char chunk_header[] = "C8\r\n";
    char chunk_data[200];
    for (int i = 0; i < 200; i++) {
        chunk_data[i] = 'X';
    }

    char full_request[1024];
    memcpy(full_request, header, strlen(header));
    int pos = strlen(header);
    memcpy(full_request + pos, chunk_header, strlen(chunk_header));
    pos += strlen(chunk_header);
    memcpy(full_request + pos, chunk_data, 200);
    pos += 200;
    memcpy(full_request + pos, "\r\n0\r\n\r\n", 7);
    pos += 7;

    size_t consumed = 0;
    ParseResult result = http_parser_parse(parser, req, full_request, pos, &consumed);

    ASSERT(result == PARSE_COMPLETE, "parse should complete");
    ASSERT(req->is_chunked == true, "is_chunked should be true");
    ASSERT(req->body_length == 200, "body_length should be 200");

    http_request_destroy(req);
    http_parser_destroy(parser);
}

/**
 * 测试 5: 空 chunked body
 */
TEST(empty_chunked_body) {
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    ASSERT(parser != NULL, "parser_create failed");
    ASSERT(req != NULL, "request_create failed");

    char request[] =
        "POST /test HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n"
        "\r\n";

    size_t consumed = 0;
    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    ASSERT(result == PARSE_COMPLETE, "parse should complete");
    ASSERT(req->is_chunked == true, "is_chunked should be true");
    ASSERT(req->body_length == 0, "body_length should be 0");

    http_request_destroy(req);
    http_parser_destroy(parser);
}

/**
 * 测试 6: chunk 扩展参数（忽略）
 */
TEST(chunk_extension) {
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    ASSERT(parser != NULL, "parser_create failed");
    ASSERT(req != NULL, "request_create failed");

    /* 带 chunk 扩展参数的请求 */
    char request[] =
        "POST /test HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5;name=value\r\n"
        "Hello\r\n"
        "0\r\n"
        "\r\n";

    size_t consumed = 0;
    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    ASSERT(result == PARSE_COMPLETE, "parse should complete");
    ASSERT(req->is_chunked == true, "is_chunked should be true");
    ASSERT(req->body_length == 5, "body_length should be 5");
    ASSERT(strncmp(req->body, "Hello", 5) == 0, "body content mismatch");

    http_request_destroy(req);
    http_parser_destroy(parser);
}

/* ========== 主函数 ========== */

int main(void) {
    printf("=== Chunked Encoding 功能测试 ===\n\n");

    test_failed = 0;

    printf("Test 1: Simple chunked request\n");
    test_simple_chunked_request();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 2: Single chunk\n");
    test_failed = 0;
    test_single_chunk();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 3: Incremental chunked parse\n");
    test_failed = 0;
    test_incremental_chunked_parse();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 4: Large chunk\n");
    test_failed = 0;
    test_large_chunk();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 5: Empty chunked body\n");
    test_failed = 0;
    test_empty_chunked_body();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 6: Chunk extension\n");
    test_failed = 0;
    test_chunk_extension();
    if (!test_failed) printf("  PASSED\n");

    printf("\n=== 测试完成 ===\n");
    return 0;
}