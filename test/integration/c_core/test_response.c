/**
 * @file    test_response.c
 * @brief   Response 模块测试
 *
 * @details
 *          - 测试响应创建和销毁
 *          - 测试状态码设置
 *          - 测试响应头设置
 *          - 测试响应体设置
 *          - 测试响应构建
 *
 * @layer   Test
 *
 * @depends response, error
 * @usedby  测试框架
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "response.h"
#include "error.h"
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

/* 测试 1: 创建和销毁 */
TEST(response_create_destroy) {
    HttpResponse *resp = response_create(HTTP_STATUS_OK);
    assert(resp != NULL);
    assert(response_get_status(resp) == HTTP_STATUS_OK);

    response_destroy(resp);
}

/* 测试 2: 状态码设置 */
TEST(response_set_status) {
    HttpResponse *resp = response_create(HTTP_STATUS_OK);
    assert(resp != NULL);

    response_set_status(resp, HTTP_STATUS_NOT_FOUND);
    assert(response_get_status(resp) == HTTP_STATUS_NOT_FOUND);

    response_set_status(resp, HTTP_STATUS_INTERNAL_ERROR);
    assert(response_get_status(resp) == HTTP_STATUS_INTERNAL_ERROR);

    response_destroy(resp);
}

/* 测试 3: 响应头设置 */
TEST(response_set_header) {
    HttpResponse *resp = response_create(HTTP_STATUS_OK);
    assert(resp != NULL);

    response_set_header(resp, "Content-Type", "text/html");
    response_set_header(resp, "X-Custom", "value");

    /* 测试覆盖同名头 */
    response_set_header(resp, "Content-Type", "application/json");

    response_destroy(resp);
}

/* 测试 4: 响应体设置 */
TEST(response_set_body) {
    HttpResponse *resp = response_create(HTTP_STATUS_OK);
    assert(resp != NULL);

    const char *body = "Hello World";
    response_set_body(resp, body, strlen(body));

    response_destroy(resp);
}

/* 测试 5: JSON 响应体 */
TEST(response_set_body_json) {
    HttpResponse *resp = response_create(HTTP_STATUS_OK);
    assert(resp != NULL);

    response_set_body_json(resp, "{\"status\":\"ok\"}");

    response_destroy(resp);
}

/* 测试 6: HTML 响应体 */
TEST(response_set_body_html) {
    HttpResponse *resp = response_create(HTTP_STATUS_OK);
    assert(resp != NULL);

    response_set_body_html(resp, "<h1>Hello</h1>", 14);

    response_destroy(resp);
}

/* 测试 7: 响应构建 */
TEST(response_build) {
    HttpResponse *resp = response_create(HTTP_STATUS_OK);
    assert(resp != NULL);

    response_set_header(resp, "Content-Type", "text/plain");
    response_set_body(resp, "Hello", 5);

    char buf[1024];
    int len = response_build(resp, buf, sizeof(buf));
    assert(len > 0);

    /* 验证响应包含状态行 */
    assert(strncmp(buf, "HTTP/1.1 200", 12) == 0);

    /* 验证响应包含 Content-Type */
    assert(strstr(buf, "Content-Type: text/plain") != NULL);

    /* 验证响应包含 Content-Length */
    assert(strstr(buf, "Content-Length: 5") != NULL);

    /* 验证响应包含响应体 */
    assert(strstr(buf, "Hello") != NULL);

    response_destroy(resp);
}

/* 测试 8: 404 响应 */
TEST(response_build_404) {
    HttpResponse *resp = response_create(HTTP_STATUS_NOT_FOUND);
    assert(resp != NULL);

    response_set_body_html(resp, "<h1>404 Not Found</h1>", 22);

    char buf[1024];
    int len = response_build(resp, buf, sizeof(buf));
    assert(len > 0);

    /* 验证状态码 */
    assert(strncmp(buf, "HTTP/1.1 404", 12) == 0);

    response_destroy(resp);
}

int main(void) {
    printf("=== Response Module Tests ===\n\n");

    RUN_TEST(response_create_destroy);
    RUN_TEST(response_set_status);
    RUN_TEST(response_set_header);
    RUN_TEST(response_set_body);
    RUN_TEST(response_set_body_json);
    RUN_TEST(response_set_body_html);
    RUN_TEST(response_build);
    RUN_TEST(response_build_404);

    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", test_passed);

    return 0;
}