#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* 测试 1: 错误码描述 */
static void test_error_description(void) {
    printf("Test 1: Error description\n");

    const char *desc = error_get_description(ERR_OK);
    assert(desc != NULL);
    assert(strlen(desc) > 0);

    desc = error_get_description(ERR_SOCKET_FAILED);
    assert(desc != NULL);

    desc = error_get_description(ERR_MEMORY_FAILED);
    assert(desc != NULL);

    printf("  PASS\n");
}

/* 测试 2: HTTP 状态码映射 */
static void test_error_to_http_status(void) {
    printf("Test 2: Error to HTTP status\n");

    HttpStatus status = error_to_http_status(ERR_OK);
    assert(status == HTTP_STATUS_OK);

    status = error_to_http_status(ERR_NOT_FOUND);
    assert(status == HTTP_STATUS_NOT_FOUND);

    status = error_to_http_status(ERR_FORBIDDEN);
    assert(status == HTTP_STATUS_FORBIDDEN);

    status = error_to_http_status(ERR_PARSE_FAILED);
    assert(status == HTTP_STATUS_BAD_REQUEST);

    status = error_to_http_status(ERR_INTERNAL);
    assert(status == HTTP_STATUS_INTERNAL_ERROR);

    printf("  PASS\n");
}

/* 测试 3: HTTP 状态码描述 */
static void test_http_status_description(void) {
    printf("Test 3: HTTP status description\n");

    const char *desc = http_status_get_description(HTTP_STATUS_OK);
    assert(desc != NULL);
    assert(strcmp(desc, "OK") == 0);

    desc = http_status_get_description(HTTP_STATUS_NOT_FOUND);
    assert(desc != NULL);
    assert(strcmp(desc, "Not Found") == 0);

    desc = http_status_get_description(HTTP_STATUS_INTERNAL_ERROR);
    assert(desc != NULL);
    assert(strcmp(desc, "Internal Server Error") == 0);

    desc = http_status_get_description(HTTP_STATUS_BAD_REQUEST);
    assert(desc != NULL);
    assert(strcmp(desc, "Bad Request") == 0);

    printf("  PASS\n");
}

int main(void) {
    printf("=== Error Module Tests ===\n\n");

    test_error_description();
    test_error_to_http_status();
    test_http_status_description();

    printf("\n=== All tests passed ===\n");
    return 0;
}