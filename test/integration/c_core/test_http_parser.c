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

int main(void) {
    printf("=== HTTP Parser Module Tests ===\n\n");

    test_parse_get();
    test_parse_query();
    test_parse_post();
    test_parse_headers();
    test_parse_incomplete();

    printf("\n=== All tests passed ===\n");
    return 0;
}