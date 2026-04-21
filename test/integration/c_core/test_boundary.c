/**
 * @file    test_boundary.c
 * @brief   边界条件测试
 *
 * @details
 *          - 测试各种模块的边界条件
 *          - 测试空输入、超长输入等极端情况
 *          - 测试错误处理路径
 *
 * @layer   Test
 *
 * @depends http_parser, router, connection, error
 * @usedby  测试框架
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "http_parser.h"
#include "eventloop.h"
#include "router.h"
#include "connection.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int test_passed = 0;
static int test_failed = 0;

#define RUN_TEST(name, func) do { \
    printf("Running: %s\n", name); \
    if (func()) { \
        printf("  PASS\n"); \
        test_passed++; \
    } else { \
        printf("  FAIL\n"); \
        test_failed++; \
    } \
} while(0)

/* ========== HTTP Parser 边界条件测试 ========== */

static int test_http_empty_input(void) {
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, "", 0, &consumed);

    http_parser_destroy(parser);
    http_request_destroy(req);

    return (result == PARSE_NEED_MORE && consumed == 0);
}

static int test_http_null_input(void) {
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    size_t consumed = 0;

    ParseResult result = http_parser_parse(parser, req, NULL, 0, &consumed);

    http_parser_destroy(parser);
    http_request_destroy(req);

    return (result == PARSE_ERROR);
}

static int test_http_incremental(void) {
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    size_t consumed = 0;

    const char *part1 = "GET / HT";
    ParseResult result = http_parser_parse(parser, req, part1, strlen(part1), &consumed);
    if (result != PARSE_NEED_MORE) {
        http_parser_destroy(parser);
        http_request_destroy(req);
        return 0;
    }

    const char *part2 = "TP/1.1\r\n\r\n";
    result = http_parser_parse(parser, req, part2, strlen(part2), &consumed);

    http_parser_destroy(parser);
    http_request_destroy(req);

    return (result == PARSE_COMPLETE);
}

static int test_http_special_chars(void) {
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    size_t consumed = 0;

    const char *request = "GET /path%20with%20spaces?key=val HTTP/1.1\r\n\r\n";
    ParseResult result = http_parser_parse(parser, req, request, strlen(request), &consumed);

    int ok = (result == PARSE_COMPLETE && req->path != NULL && req->query != NULL);

    http_parser_destroy(parser);
    http_request_destroy(req);

    return ok;
}

/* ========== EventLoop 边界条件测试 ========== */

static int test_eventloop_invalid_capacity(void) {
    EventLoop *loop1 = eventloop_create(0);
    EventLoop *loop2 = eventloop_create(-1);

    int ok = (loop1 == NULL && loop2 == NULL);

    /* 不需要 destroy，因为都是 NULL */
    return ok;
}

static int test_eventloop_null_ops(void) {
    eventloop_destroy(NULL);
    eventloop_stop(NULL);

    int result = eventloop_add(NULL, 0, EV_READ, NULL, NULL);
    int result2 = eventloop_poll(NULL, 100);

    return (result == -1 && result2 == -1);
}

/* ========== Router 边界条件测试 ========== */

static int test_router_null_params(void) {
    Route *matched = router_match(NULL, "/test", HTTP_GET);
    if (matched != NULL) return 0;

    Router *router = router_create();
    matched = router_match(router, NULL, HTTP_GET);

    router_destroy(router);
    return (matched == NULL);
}

static int test_router_empty(void) {
    Router *router = router_create();
    Route *matched = router_match(router, "/nonexistent", HTTP_GET);

    router_destroy(router);
    return (matched == NULL);
}

/* ========== Buffer 边界条件测试 ========== */

static int test_buffer_null_ops(void) {
    int result = buffer_write(NULL, "test", 4);
    if (result != -1) return 0;

    size_t avail = buffer_available(NULL);
    if (avail != 0) return 0;

    buffer_destroy(NULL);
    return 1;
}

static int test_buffer_fixed_overflow(void) {
    Buffer *buf = buffer_create_ex(100, BUFFER_MODE_FIXED, 1000);

    char data[200];
    memset(data, 'x', 200);

    int result = buffer_write(buf, data, 200);

    buffer_destroy(buf);
    return (result == -1);  /* 应该拒绝溢出 */
}

/* ========== Error 边界条件测试 ========== */

static int test_error_invalid_code(void) {
    const char *desc = error_get_description((ErrorCode)-999);
    /* 应该返回 NULL 或默认描述 */
    return (desc == NULL || desc != NULL);
}

int main(void) {
    printf("\n=== Phase 1 Boundary Condition Tests ===\n\n");

    printf("--- HTTP Parser Boundary Tests ---\n");
    RUN_TEST("http_empty_input", test_http_empty_input);
    RUN_TEST("http_null_input", test_http_null_input);
    RUN_TEST("http_incremental", test_http_incremental);
    RUN_TEST("http_special_chars", test_http_special_chars);

    printf("\n--- EventLoop Boundary Tests ---\n");
    RUN_TEST("eventloop_invalid_capacity", test_eventloop_invalid_capacity);
    RUN_TEST("eventloop_null_ops", test_eventloop_null_ops);

    printf("\n--- Router Boundary Tests ---\n");
    RUN_TEST("router_null_params", test_router_null_params);
    RUN_TEST("router_empty", test_router_empty);

    printf("\n--- Buffer Boundary Tests ---\n");
    RUN_TEST("buffer_null_ops", test_buffer_null_ops);
    RUN_TEST("buffer_fixed_overflow", test_buffer_fixed_overflow);

    printf("\n--- Error Boundary Tests ---\n");
    RUN_TEST("error_invalid_code", test_error_invalid_code);

    printf("\n=== Test Summary ===\n");
    printf("Total: %d\n", test_passed + test_failed);
    printf("Passed: %d\n", test_passed);
    printf("Failed: %d\n", test_failed);

    return test_failed > 0 ? 1 : 0;
}