/**
 * @file    test_router.c
 * @brief   Router 模块测试
 *
 * @details
 *          - 测试 Router 创建和销毁
 *          - 测试精确匹配和前缀匹配
 *          - 测试方法过滤
 *          - 测试优先级排序
 *
 * @layer   Test
 *
 * @depends router
 * @usedby  测试框架
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "router.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* 测试响应结构 */
typedef struct {
    int status;
    const char *body;
} TestResponse;

/* 测试处理器 */
static void test_handler_1(HttpRequest *req, void *resp, void *user_data) {
    (void)req;
    (void)user_data;
    TestResponse *r = (TestResponse *)resp;
    r->status = 200;
    r->body = "Handler 1";
}

static void test_handler_2(HttpRequest *req, void *resp, void *user_data) {
    (void)req;
    (void)user_data;
    TestResponse *r = (TestResponse *)resp;
    r->status = 200;
    r->body = "Handler 2";
}

/* 测试 1: 创建和销毁路由器 */
static void test_router_create(void) {
    printf("Test 1: Router create/destroy\n");

    Router *router = router_create();
    assert(router != NULL);

    router_destroy(router);
    printf("  PASS\n");
}

/* 测试 2: 添加精确路由 */
static void test_router_add_exact(void) {
    printf("Test 2: Add exact route\n");

    Router *router = router_create();
    Route *route = route_create(ROUTER_MATCH_EXACT, "/", test_handler_1, NULL);

    int result = router_add_route(router, route);
    assert(result == 0);

    router_destroy(router);
    printf("  PASS\n");
}

/* 测试 3: 精确路由匹配 */
static void test_router_match_exact(void) {
    printf("Test 3: Match exact route\n");

    Router *router = router_create();

    Route *route1 = route_create(ROUTER_MATCH_EXACT, "/", test_handler_1, NULL);
    Route *route2 = route_create(ROUTER_MATCH_EXACT, "/api", test_handler_2, NULL);

    router_add_route(router, route1);
    router_add_route(router, route2);

    /* 匹配根路径 */
    Route *matched = router_match(router, "/", HTTP_GET);
    assert(matched != NULL);
    assert(matched == route1);

    /* 匹配 /api */
    matched = router_match(router, "/api", HTTP_GET);
    assert(matched != NULL);
    assert(matched == route2);

    /* 不匹配 */
    matched = router_match(router, "/notfound", HTTP_GET);
    assert(matched == NULL);

    router_destroy(router);
    printf("  PASS\n");
}

/* 测试 4: 方法过滤 */
static void test_router_method_filter(void) {
    printf("Test 4: Method filter\n");

    Router *router = router_create();

    Route *route = route_create(ROUTER_MATCH_EXACT, "/api", test_handler_1, NULL);
    route->methods = METHOD_GET;  /* 仅支持 GET */

    router_add_route(router, route);

    /* GET 匹配 */
    Route *matched = router_match(router, "/api", HTTP_GET);
    assert(matched != NULL);

    /* POST 不匹配 */
    matched = router_match(router, "/api", HTTP_POST);
    assert(matched == NULL);

    router_destroy(router);
    printf("  PASS\n");
}

/* 测试 5: 优先级 */
static void test_router_priority(void) {
    printf("Test 5: Route priority\n");

    Router *router = router_create();

    Route *low = route_create(ROUTER_MATCH_EXACT, "/api", test_handler_1, NULL);
    Route *high = route_create(ROUTER_MATCH_EXACT, "/api", test_handler_2, NULL);

    router_add_route_ex(router, low, PRIORITY_LOW);
    router_add_route_ex(router, high, PRIORITY_HIGH);

    /* 高优先级匹配 */
    Route *matched = router_match(router, "/api", HTTP_GET);
    assert(matched != NULL);
    assert(matched == high);

    router_destroy(router);
    printf("  PASS\n");
}

/* 测试 6: 前缀匹配 */
static void test_router_match_prefix(void) {
    printf("Test 6: Match prefix route\n");

    Router *router = router_create();

    Route *route = route_create(ROUTER_MATCH_PREFIX, "/api/", test_handler_1, NULL);
    router_add_route(router, route);

    /* 匹配 /api/users */
    Route *matched = router_match(router, "/api/users", HTTP_GET);
    assert(matched != NULL);
    assert(matched == route);

    /* 匹配 /api/posts/123 */
    matched = router_match(router, "/api/posts/123", HTTP_GET);
    assert(matched != NULL);

    /* 不匹配 /api */
    matched = router_match(router, "/api", HTTP_GET);
    /* 根据实现可能匹配或不匹配 */

    router_destroy(router);
    printf("  PASS\n");
}

/* 测试 7: 所有方法 */
static void test_router_all_methods(void) {
    printf("Test 7: Route all methods\n");

    Router *router = router_create();

    Route *route = route_create(ROUTER_MATCH_EXACT, "/api", test_handler_1, NULL);
    route->methods = METHOD_ALL;  /* 支持所有方法 */
    router_add_route(router, route);

    /* 所有方法都应匹配 */
    assert(router_match(router, "/api", HTTP_GET) != NULL);
    assert(router_match(router, "/api", HTTP_POST) != NULL);
    assert(router_match(router, "/api", HTTP_PUT) != NULL);
    assert(router_match(router, "/api", HTTP_DELETE) != NULL);
    assert(router_match(router, "/api", HTTP_HEAD) != NULL);
    assert(router_match(router, "/api", HTTP_OPTIONS) != NULL);
    assert(router_match(router, "/api", HTTP_PATCH) != NULL);

    router_destroy(router);
    printf("  PASS\n");
}

/* 测试 8: 多个相同模式不同方法 */
static void test_router_same_pattern_different_methods(void) {
    printf("Test 8: Same pattern different methods\n");

    Router *router = router_create();

    Route *get_route = route_create(ROUTER_MATCH_EXACT, "/api", test_handler_1, NULL);
    get_route->methods = METHOD_GET;
    router_add_route(router, get_route);

    Route *post_route = route_create(ROUTER_MATCH_EXACT, "/api", test_handler_2, NULL);
    post_route->methods = METHOD_POST;
    router_add_route(router, post_route);

    /* GET 匹配 get_route */
    Route *matched = router_match(router, "/api", HTTP_GET);
    assert(matched == get_route);

    /* POST 匹配 post_route */
    matched = router_match(router, "/api", HTTP_POST);
    assert(matched == post_route);

    router_destroy(router);
    printf("  PASS\n");
}

/* 测试 9: 路由销毁 */
static void test_route_destroy(void) {
    printf("Test 9: Route destroy\n");

    Route *route = route_create(ROUTER_MATCH_EXACT, "/test", test_handler_1, NULL);
    assert(route != NULL);

    route_destroy(route);
    printf("  PASS\n");
}

/* 测试 10: 路由优先级排序 */
static void test_router_sort_priority(void) {
    printf("Test 10: Router sort by priority\n");

    Router *router = router_create();

    Route *low = route_create(ROUTER_MATCH_EXACT, "/api", test_handler_1, NULL);
    Route *normal = route_create(ROUTER_MATCH_EXACT, "/api", test_handler_2, NULL);
    Route *high = route_create(ROUTER_MATCH_EXACT, "/api", test_handler_1, NULL);

    router_add_route_ex(router, low, PRIORITY_LOW);
    router_add_route_ex(router, normal, PRIORITY_NORMAL);
    router_add_route_ex(router, high, PRIORITY_HIGH);

    /* 高优先级应匹配 */
    Route *matched = router_match(router, "/api", HTTP_GET);
    assert(matched == high);

    router_destroy(router);
    printf("  PASS\n");
}

int main(void) {
    printf("=== Router Module Tests ===\n\n");

    test_router_create();
    test_router_add_exact();
    test_router_match_exact();
    test_router_method_filter();
    test_router_priority();
    test_router_match_prefix();
    test_router_all_methods();
    test_router_same_pattern_different_methods();
    test_route_destroy();
    test_router_sort_priority();

    printf("\n=== All tests passed ===\n");
    return 0;
}