/**
 * @file    test_handler.c
 * @brief   Handler 模块测试
 *
 * @details
 *          - 测试预置 Handler 函数
 *          - 测试 Handler 调用和响应生成
 *
 * @layer   Test
 *
 * @depends handler, response, http_parser
 * @usedby  测试框架
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "handler.h"
#include "response.h"
#include "http_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* 测试辅助宏 */
#define TEST(name) static void test_##name()
#define RUN_TEST(name) do { \
    printf("Test %d: %s\n", test_count++, #name); \
    test_##name(); \
    test_passed++; \
} while(0)

static int test_passed = 0;
static int test_count = 1;

/* 测试 1: 404 Handler */
TEST(handler_404) {
    HttpRequest *req = http_request_create();
    HttpResponse *resp = response_create(HTTP_STATUS_OK);
    assert(req != NULL);
    assert(resp != NULL);

    handler_404(req, resp, NULL);

    assert(response_get_status(resp) == HTTP_STATUS_NOT_FOUND);

    char buf[1024];
    int len = response_build(resp, buf, sizeof(buf));
    assert(len > 0);
    assert(strncmp(buf, "HTTP/1.1 404", 12) == 0);

    http_request_destroy(req);
    response_destroy(resp);
}

/* 测试 2: 500 Handler */
TEST(handler_500) {
    HttpRequest *req = http_request_create();
    HttpResponse *resp = response_create(HTTP_STATUS_OK);
    assert(req != NULL);
    assert(resp != NULL);

    handler_500(req, resp, NULL);

    assert(response_get_status(resp) == HTTP_STATUS_INTERNAL_ERROR);

    char buf[1024];
    int len = response_build(resp, buf, sizeof(buf));
    assert(len > 0);
    assert(strncmp(buf, "HTTP/1.1 500", 12) == 0);

    http_request_destroy(req);
    response_destroy(resp);
}

/* 测试 3: JSON API Handler */
TEST(handler_json_api) {
    HttpRequest *req = http_request_create();
    HttpResponse *resp = response_create(HTTP_STATUS_OK);
    assert(req != NULL);
    assert(resp != NULL);

    const char *json = "{\"test\":\"ok\"}";
    handler_json_api(req, resp, json);

    assert(response_get_status(resp) == HTTP_STATUS_OK);

    char buf[1024];
    int len = response_build(resp, buf, sizeof(buf));
    assert(len > 0);

    /* 验证 Content-Type 是 JSON */
    assert(strstr(buf, "Content-Type: application/json") != NULL);
    assert(strstr(buf, json) != NULL);

    http_request_destroy(req);
    response_destroy(resp);
}

/* 测试 4: 文本 Handler */
TEST(handler_text) {
    HttpRequest *req = http_request_create();
    HttpResponse *resp = response_create(HTTP_STATUS_OK);
    assert(req != NULL);
    assert(resp != NULL);

    const char *text = "Hello World";
    handler_text(req, resp, text);

    assert(response_get_status(resp) == HTTP_STATUS_OK);

    char buf[1024];
    int len = response_build(resp, buf, sizeof(buf));
    assert(len > 0);

    /* 验证 Content-Type 是 text/plain */
    assert(strstr(buf, "Content-Type: text/plain") != NULL);
    assert(strstr(buf, text) != NULL);

    http_request_destroy(req);
    response_destroy(resp);
}

/* 测试 5: Handler 空参数 */
TEST(handler_null_params) {
    /* 测试 NULL 参数处理 */
    handler_404(NULL, NULL, NULL);  /* 应该不会崩溃 */
    handler_500(NULL, NULL, NULL);
    handler_json_api(NULL, NULL, NULL);
    handler_text(NULL, NULL, NULL);
}

int main(void) {
    printf("=== Handler Module Tests ===\n\n");

    RUN_TEST(handler_404);
    RUN_TEST(handler_500);
    RUN_TEST(handler_json_api);
    RUN_TEST(handler_text);
    RUN_TEST(handler_null_params);

    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", test_passed);

    return 0;
}