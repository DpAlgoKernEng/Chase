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

int main(void) {
    printf("=== Router Module Tests ===\n\n");

    test_router_create();
    test_router_add_exact();
    test_router_match_exact();
    test_router_method_filter();
    test_router_priority();

    printf("\n=== All tests passed ===\n");
    return 0;
}