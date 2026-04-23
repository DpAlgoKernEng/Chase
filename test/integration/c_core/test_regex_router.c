/**
 * @file    test_regex_router.c
 * @brief   Router 正则匹配测试
 *
 * @details
 *          - 正则表达式匹配
 *          - 捕获组提取
 *          - 优先级测试
 *          - 方法过滤
 *          - 无效正则处理
 *          - 正则缓存测试
 *          - 冲突检测
 *          - 冲突策略测试
 *
 * @layer   Test Layer
 *
 * @depends router
 *
 * @author  minghui.liu
 * @date    2026-04-23
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "router.h"

static int test_count = 0;
static int test_passed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("Test %d: %s\n", ++test_count, #name); \
    fflush(stdout); \
    test_##name(); \
    test_passed++; \
} while(0)

/* 测试处理器（空函数） */
static void test_handler(HttpRequest *req, void *resp, void *user_data) {
    (void)req; (void)resp; (void)user_data;
}

/* ============== 测试用例 ============== */

TEST(regex_simple) {
    Router *router = router_create();
    assert(router != NULL);

    /* 添加正则路由 */
    int ret = router_add_regex_route(router, "^/api/users/[0-9]+$",
                                      test_handler, NULL,
                                      METHOD_GET, PRIORITY_NORMAL);
    assert(ret == 0);

    /* 匹配成功 */
    Route *route = router_match(router, "/api/users/123", HTTP_GET);
    assert(route != NULL);
    assert(route->type == ROUTER_MATCH_REGEX);

    /* 不匹配 */
    route = router_match(router, "/api/users/abc", HTTP_GET);
    assert(route == NULL);

    route = router_match(router, "/api/users/", HTTP_GET);
    assert(route == NULL);

    router_destroy(router);
    printf("  PASS\n");
}

TEST(capture_groups) {
    Router *router = router_create();
    assert(router != NULL);

    /* 添加带捕获组的正则路由 */
    int ret = router_add_regex_route(router, "^/api/users/([0-9]+)/posts/([0-9]+)$",
                                      test_handler, NULL,
                                      METHOD_GET, PRIORITY_NORMAL);
    assert(ret == 0);

    /* 匹配并获取捕获组 */
    RegexMatchResult result;
    Route *route = router_match_ex(router, "/api/users/42/posts/100", HTTP_GET, &result);
    assert(route != NULL);
    assert(result.group_count == 3);  /* 全匹配 + 2 个捕获组 */

    /* 验证捕获组内容 */
    assert(result.groups[0] != NULL);  /* 全匹配 */
    assert(strcmp(result.groups[0], "/api/users/42/posts/100") == 0);

    assert(result.groups[1] != NULL);  /* 用户 ID */
    assert(strcmp(result.groups[1], "42") == 0);

    assert(result.groups[2] != NULL);  /* 帖子 ID */
    assert(strcmp(result.groups[2], "100") == 0);

    /* 验证位置 */
    assert(result.group_starts[1] == 11);
    assert(result.group_ends[1] == 13);

    regex_match_result_free(&result);
    router_destroy(router);
    printf("  PASS\n");
}

TEST(priority) {
    Router *router = router_create();
    assert(router != NULL);

    /* 添加两个优先级不同的正则路由 */
    int ret = router_add_regex_route(router, "^/api/.*$",
                                      test_handler, (void*)"low",
                                      METHOD_GET, PRIORITY_LOW);
    assert(ret == 0);

    ret = router_add_regex_route(router, "^/api/users/[0-9]+$",
                                 test_handler, (void*)"high",
                                 METHOD_GET, PRIORITY_HIGH);
    assert(ret == 0);

    /* 匹配时应返回高优先级路由 */
    Route *route = router_match(router, "/api/users/123", HTTP_GET);
    assert(route != NULL);
    assert(route->priority == PRIORITY_HIGH);
    assert(strcmp((char *)route->user_data, "high") == 0);

    /* 匹配低优先级路由 */
    route = router_match(router, "/api/other", HTTP_GET);
    assert(route != NULL);
    assert(route->priority == PRIORITY_LOW);
    assert(strcmp((char *)route->user_data, "low") == 0);

    router_destroy(router);
    printf("  PASS\n");
}

TEST(method_filter) {
    Router *router = router_create();
    assert(router != NULL);

    /* 仅 GET 方法的正则路由 */
    int ret = router_add_regex_route(router, "^/api/get-only$",
                                      test_handler, NULL,
                                      METHOD_GET, PRIORITY_NORMAL);
    assert(ret == 0);

    /* 仅 POST 方法的正则路由 */
    ret = router_add_regex_route(router, "^/api/post-only$",
                                 test_handler, NULL,
                                 METHOD_POST, PRIORITY_NORMAL);
    assert(ret == 0);

    /* GET 方法匹配 */
    Route *route = router_match(router, "/api/get-only", HTTP_GET);
    assert(route != NULL);

    route = router_match(router, "/api/get-only", HTTP_POST);
    assert(route == NULL);

    /* POST 方法匹配 */
    route = router_match(router, "/api/post-only", HTTP_POST);
    assert(route != NULL);

    route = router_match(router, "/api/post-only", HTTP_GET);
    assert(route == NULL);

    router_destroy(router);
    printf("  PASS\n");
}

TEST(invalid_pattern) {
    Router *router = router_create();
    assert(router != NULL);

    /* 无效正则表达式（不平衡括号） */
    int ret = router_add_regex_route(router, "^/api/users/([0-9]+",
                                      test_handler, NULL,
                                      METHOD_GET, PRIORITY_NORMAL);
    /* 正则编译会在匹配时失败，但添加成功 */
    assert(ret == 0);

    /* 尝试匹配 - 正则编译失败后不会匹配 */
    Route *route = router_match(router, "/api/users/123", HTTP_GET);
    assert(route == NULL);  /* 编译失败导致无法匹配 */

    router_destroy(router);
    printf("  PASS\n");
}

TEST(regex_cache) {
    Router *router = router_create();
    assert(router != NULL);

    /* 添加正则路由 */
    int ret = router_add_regex_route(router, "^/cached/[a-z]+$",
                                      test_handler, NULL,
                                      METHOD_GET, PRIORITY_NORMAL);
    assert(ret == 0);

    /* 第一次匹配 - 正则编译 */
    Route *route = router_match(router, "/cached/test", HTTP_GET);
    assert(route != NULL);
    assert(route->regex_compiled == true);
    assert(route->regex_cache != NULL);

    /* 第二次匹配 - 使用缓存 */
    route = router_match(router, "/cached/another", HTTP_GET);
    assert(route != NULL);
    assert(route->regex_compiled == true);  /* 仍为 true */

    router_destroy(router);
    printf("  PASS\n");
}

TEST(conflict_detect) {
    Router *router = router_create();
    assert(router != NULL);

    /* 添加第一个正则路由 */
    int ret = router_add_regex_route(router, "^/api/users/[0-9]+$",
                                      test_handler, NULL,
                                      METHOD_GET, PRIORITY_NORMAL);
    assert(ret == 0);

    /* 创建相同模式的路由 */
    Route *new_route = route_create(ROUTER_MATCH_REGEX, "^/api/users/[0-9]+$",
                                    test_handler, NULL);
    new_route->methods = METHOD_GET;

    /* 检测冲突 */
    int conflicts = router_detect_conflicts(router, new_route);
    assert(conflicts > 0);  /* 应检测到冲突 */

    route_destroy(new_route);
    router_destroy(router);
    printf("  PASS\n");
}

TEST(conflict_warn) {
    Router *router = router_create();
    assert(router != NULL);
    router_set_conflict_policy(router, ROUTER_CONFLICT_WARN);

    /* 添加第一个正则路由 */
    int ret = router_add_regex_route(router, "^/api/test$",
                                      test_handler, (void*)"first",
                                      METHOD_GET, PRIORITY_NORMAL);
    assert(ret == 0);

    /* 添加相同正则路由（警告策略允许） */
    ret = router_add_regex_route(router, "^/api/test$",
                                 test_handler, (void*)"second",
                                 METHOD_GET, PRIORITY_NORMAL);
    assert(ret == 0);  /* 允许添加 */

    /* 应匹配第一个（高优先级或先添加） */
    Route *route = router_match(router, "/api/test", HTTP_GET);
    assert(route != NULL);

    router_destroy(router);
    printf("  PASS\n");
}

TEST(conflict_error) {
    Router *router = router_create();
    assert(router != NULL);
    router_set_conflict_policy(router, ROUTER_CONFLICT_ERROR);

    /* 添加第一个正则路由 */
    int ret = router_add_regex_route(router, "^/api/error$",
                                      test_handler, NULL,
                                      METHOD_GET, PRIORITY_NORMAL);
    assert(ret == 0);

    /* 添加相同正则路由（错误策略拒绝） */
    ret = router_add_regex_route(router, "^/api/error$",
                                 test_handler, NULL,
                                 METHOD_GET, PRIORITY_NORMAL);
    assert(ret == -1);  /* 拒绝添加 */

    router_destroy(router);
    printf("  PASS\n");
}

TEST(conflict_override) {
    Router *router = router_create();
    assert(router != NULL);
    router_set_conflict_policy(router, ROUTER_CONFLICT_OVERRIDE);

    /* 添加第一个正则路由 */
    int ret = router_add_regex_route(router, "^/api/override$",
                                      test_handler, (void*)"original",
                                      METHOD_GET, PRIORITY_NORMAL);
    assert(ret == 0);

    /* 添加相同正则路由（覆盖策略） */
    ret = router_add_regex_route(router, "^/api/override$",
                                 test_handler, (void*)"replacement",
                                 METHOD_GET, PRIORITY_HIGH);
    assert(ret == 0);  /* 覆盖成功 */

    /* 应匹配新路由 */
    Route *route = router_match(router, "/api/override", HTTP_GET);
    assert(route != NULL);
    assert(strcmp((char *)route->user_data, "replacement") == 0);

    router_destroy(router);
    printf("  PASS\n");
}

TEST(multiple_matches) {
    Router *router = router_create();
    assert(router != NULL);

    /* 添加多个正则路由 */
    int ret = router_add_regex_route(router, "^/api/.*$",
                                      test_handler, (void*)"api",
                                      METHOD_GET, PRIORITY_LOW);
    assert(ret == 0);

    ret = router_add_regex_route(router, "^/api/v[0-9]+/.*$",
                                 test_handler, (void*)"versioned",
                                 METHOD_GET, PRIORITY_NORMAL);
    assert(ret == 0);

    ret = router_add_regex_route(router, "^/api/v1/users/[0-9]+$",
                                 test_handler, (void*)"specific",
                                 METHOD_GET, PRIORITY_HIGH);
    assert(ret == 0);

    /* 匹配最具体的路由 */
    Route *route = router_match(router, "/api/v1/users/123", HTTP_GET);
    assert(route != NULL);
    assert(strcmp((char *)route->user_data, "specific") == 0);

    /* 匹配版本化路由 */
    route = router_match(router, "/api/v2/other", HTTP_GET);
    assert(route != NULL);
    assert(strcmp((char *)route->user_data, "versioned") == 0);

    /* 匹配通用 API 路由 */
    route = router_match(router, "/api/legacy", HTTP_GET);
    assert(route != NULL);
    assert(strcmp((char *)route->user_data, "api") == 0);

    router_destroy(router);
    printf("  PASS\n");
}

TEST(regex_vs_prefix) {
    Router *router = router_create();
    assert(router != NULL);

    /* 添加前缀匹配路由 */
    Route *prefix_route = route_create(ROUTER_MATCH_PREFIX, "/static/",
                                       test_handler, (void*)"prefix");
    assert(prefix_route != NULL);
    router_add_route_ex(router, prefix_route, PRIORITY_NORMAL);

    /* 添加正则路由 */
    int ret = router_add_regex_route(router, "^/static/[a-z]+\\.css$",
                                      test_handler, (void*)"regex",
                                      METHOD_GET, PRIORITY_HIGH);
    assert(ret == 0);

    /* CSS 文件应匹配正则（高优先级） */
    Route *route = router_match(router, "/static/style.css", HTTP_GET);
    assert(route != NULL);
    assert(strcmp((char *)route->user_data, "regex") == 0);

    /* JS 文件应匹配前缀（正则不匹配） */
    route = router_match(router, "/static/script.js", HTTP_GET);
    assert(route != NULL);
    assert(strcmp((char *)route->user_data, "prefix") == 0);

    router_destroy(router);
    printf("  PASS\n");
}

TEST(null_handling) {
    Router *router = router_create();

    /* NULL 参数测试 */
    assert(router_add_regex_route(NULL, "^/test$", test_handler, NULL,
                                  METHOD_GET, PRIORITY_NORMAL) == -1);
    assert(router_add_regex_route(router, NULL, test_handler, NULL,
                                  METHOD_GET, PRIORITY_NORMAL) == -1);

    assert(router_match_ex(NULL, "/test", HTTP_GET, NULL) == NULL);
    assert(router_match_ex(router, NULL, HTTP_GET, NULL) == NULL);

    assert(router_detect_conflicts(NULL, NULL) == 0);

    router_set_conflict_policy(NULL, ROUTER_CONFLICT_WARN);

    regex_match_result_free(NULL);

    RegexMatchResult result = {0};
    regex_match_result_free(&result);  /* 空 result 应安全 */

    router_destroy(router);
    printf("  PASS\n");
}

/* ============== 主函数 ============== */

int main(void) {
    printf("=== Router Regex Tests ===\n\n");

    RUN_TEST(regex_simple);
    RUN_TEST(capture_groups);
    RUN_TEST(priority);
    RUN_TEST(method_filter);
    RUN_TEST(invalid_pattern);
    RUN_TEST(regex_cache);
    RUN_TEST(conflict_detect);
    RUN_TEST(conflict_warn);
    RUN_TEST(conflict_error);
    RUN_TEST(conflict_override);
    RUN_TEST(multiple_matches);
    RUN_TEST(regex_vs_prefix);
    RUN_TEST(null_handling);

    printf("\n=== Test Summary ===\n");
    printf("Total: %d, Passed: %d, Failed: %d\n",
           test_count, test_passed, test_count - test_passed);

    return (test_count == test_passed) ? 0 : 1;
}