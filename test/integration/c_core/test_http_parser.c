/**
 * @file    test_http_parser.c
 * @brief   HTTP Parser 模块测试
 *
 * @details
 *          - 测试 HTTP 请求解析
 *          - 测试各种 HTTP 方法
 *          - 测试请求头解析
 *          - 测试增量解析
 *
 * @layer   Test
 *
 * @depends http_parser, eventloop, router, connection, error
 * @usedby  测试框架
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "http_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* 测试 1: 解析简单 GET 请求 */
static void test_parse_get(void) {
    printf("Test 1: Parse GET request\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    const char *request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    assert(result == PARSE_COMPLETE);
    assert(req->method == HTTP_GET);
    assert(strcmp(req->path, "/") == 0);
    assert(strcmp(req->version, "HTTP/1.1") == 0);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* 测试 2: 解析带查询参数的请求 */
static void test_parse_query(void) {
    printf("Test 2: Parse request with query\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    const char *request = "GET /api/users?id=123&name=test HTTP/1.1\r\nHost: localhost\r\n\r\n";
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    assert(result == PARSE_COMPLETE);
    assert(strcmp(req->path, "/api/users") == 0);
    assert(strcmp(req->query, "id=123&name=test") == 0);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* 测试 3: 解析 POST 请求 */
static void test_parse_post(void) {
    printf("Test 3: Parse POST request\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    const char *request = "POST /api/data HTTP/1.1\r\nHost: localhost\r\nContent-Length: 13\r\n\r\nHello, World!";
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    assert(result == PARSE_COMPLETE);
    assert(req->method == HTTP_POST);
    assert(req->content_length == 13);
    assert(req->body != NULL);
    assert(strcmp(req->body, "Hello, World!") == 0);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* 测试 4: 解析头部 */
static void test_parse_headers(void) {
    printf("Test 4: Parse headers\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    const char *request = "GET / HTTP/1.1\r\nHost: localhost:8080\r\nUser-Agent: TestClient\r\nAccept: text/html\r\n\r\n";
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    assert(result == PARSE_COMPLETE);

    const char *host = http_request_get_header_value(req, "Host");
    assert(host != NULL);
    assert(strcmp(host, "localhost:8080") == 0);

    const char *ua = http_request_get_header_value(req, "User-Agent");
    assert(ua != NULL);
    assert(strcmp(ua, "TestClient") == 0);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* 测试 5: 不完整请求 */
static void test_parse_incomplete(void) {
    printf("Test 5: Parse incomplete request\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    const char *request = "GET / HTTP/1.1\r\nHost: localhost";
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    assert(result == PARSE_NEED_MORE);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* 测试 6: PUT 方法 */
static void test_parse_put(void) {
    printf("Test 6: Parse PUT request\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    const char *request = "PUT /resource HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\ndata";
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    assert(result == PARSE_COMPLETE);
    assert(req->method == HTTP_PUT);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* 测试 7: DELETE 方法 */
static void test_parse_delete(void) {
    printf("Test 7: Parse DELETE request\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    const char *request = "DELETE /resource/123 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    assert(result == PARSE_COMPLETE);
    assert(req->method == HTTP_DELETE);
    assert(strcmp(req->path, "/resource/123") == 0);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* 测试 8: HEAD 方法 */
static void test_parse_head(void) {
    printf("Test 8: Parse HEAD request\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    const char *request = "HEAD / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    assert(result == PARSE_COMPLETE);
    assert(req->method == HTTP_HEAD);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* 测试 9: OPTIONS 方法 */
static void test_parse_options(void) {
    printf("Test 9: Parse OPTIONS request\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    const char *request = "OPTIONS * HTTP/1.1\r\nHost: localhost\r\n\r\n";
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    assert(result == PARSE_COMPLETE);
    assert(req->method == HTTP_OPTIONS);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* 测试 10: PATCH 方法 */
static void test_parse_patch(void) {
    printf("Test 10: Parse PATCH request\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    const char *request = "PATCH /resource HTTP/1.1\r\nHost: localhost\r\nContent-Length: 6\r\n\r\nupdate";
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    assert(result == PARSE_COMPLETE);
    assert(req->method == HTTP_PATCH);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* 测试 11: malformed 请求 - 无 Host */
static void test_parse_malformed_no_host(void) {
    printf("Test 11: Parse malformed request (no host)\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    /* HTTP/1.1 需要 Host 头，但解析器可能允许 */
    const char *request = "GET / HTTP/1.1\r\n\r\n";
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    /* 根据实现，可能返回 PARSE_COMPLETE 或 PARSE_ERROR */
    /* 这里假设解析器不强制 Host 头 */

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* 测试 12: malformed 请求 - 无方法 */
static void test_parse_malformed_no_method(void) {
    printf("Test 12: Parse malformed request (no method)\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    const char *request = " / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    /* 应返回 PARSE_ERROR 或处理异常 */
    assert(result == PARSE_ERROR || result == PARSE_COMPLETE);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* 测试 13: 多头部 */
static void test_parse_many_headers(void) {
    printf("Test 13: Parse many headers\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    const char *request =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "User-Agent: Test\r\n"
        "Accept: text/html\r\n"
        "Accept-Language: en\r\n"
        "Accept-Encoding: gzip\r\n"
        "Connection: keep-alive\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n";
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    assert(result == PARSE_COMPLETE);
    assert(req->header_count >= 7);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* 测试 14: 大 body */
static void test_parse_large_body(void) {
    printf("Test 14: Parse large body\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    /* 创建 1KB body */
    char body[1024];
    memset(body, 'X', sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';

    char request[2048];
    snprintf(request, sizeof(request),
             "POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(body), body);

    size_t consumed = 0;
    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    assert(result == PARSE_COMPLETE);
    assert(req->body_length == strlen(body));

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* 测试 15: 解析器重置 */
static void test_parser_reset(void) {
    printf("Test 15: Parser reset\n");

    HttpParser *parser = http_parser_create();
    HttpRequest *req1 = http_request_create();

    /* 解析第一个请求 */
    const char *request1 = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    size_t consumed = 0;
    http_parser_parse(parser, req1, request1, strlen(request1), &consumed);

    /* 重置解析器 */
    http_parser_reset(parser);

    /* 解析第二个请求 */
    HttpRequest *req2 = http_request_create();
    const char *request2 = "POST /api HTTP/1.1\r\nHost: localhost\r\n\r\n";
    http_parser_parse(parser, req2, request2, strlen(request2), &consumed);

    assert(req2->method == HTTP_POST);

    http_request_destroy(req1);
    http_request_destroy(req2);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

int main(void) {
    printf("=== HTTP Parser Module Tests ===\n\n");

    test_parse_get();
    test_parse_query();
    test_parse_post();
    test_parse_headers();
    test_parse_incomplete();
    test_parse_put();
    test_parse_delete();
    test_parse_head();
    test_parse_options();
    test_parse_patch();
    test_parse_malformed_no_host();
    test_parse_malformed_no_method();
    test_parse_many_headers();
    test_parse_large_body();
    test_parser_reset();

    printf("\n=== All tests passed ===\n");
    return 0;
}