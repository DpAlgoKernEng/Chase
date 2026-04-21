/**
 * @file    test_server.c
 * @brief   Server 模块基础测试
 *
 * @details
 *          - 测试 Server 创建和销毁
 *          - 测试 Server 配置
 *          - 注意：完整功能测试需要实际网络环境
 *
 * @layer   Test
 *
 * @depends server, router
 * @usedby  测试框架
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "server.h"
#include "router.h"
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
TEST(server_create_destroy) {
    Router *router = router_create();
    assert(router != NULL);

    ServerConfig config = {
        .port = 18080,  /* 使用测试端口 */
        .max_connections = 100,
        .backlog = 10,
        .bind_addr = NULL,
        .reuseport = false,
        .router = router,
        .read_buf_cap = 0,
        .write_buf_cap = 0
    };

    Server *server = server_create(&config);
    assert(server != NULL);
    assert(server_get_fd(server) > 0);
    assert(server_get_router(server) == router);

    server_destroy(server);
    router_destroy(router);
}

/* 测试 2: 使用默认 Router */
TEST(server_create_default_router) {
    ServerConfig config = {
        .port = 18081,
        .max_connections = 100,
        .backlog = 10,
        .bind_addr = NULL,
        .reuseport = false,
        .router = NULL,  /* 使用默认 Router */
        .read_buf_cap = 0,
        .write_buf_cap = 0
    };

    Server *server = server_create(&config);
    assert(server != NULL);
    assert(server_get_router(server) != NULL);

    server_destroy(server);
}

/* 测试 3: 获取 EventLoop */
TEST(server_get_eventloop) {
    Router *router = router_create();
    assert(router != NULL);

    ServerConfig config = {
        .port = 18082,
        .max_connections = 100,
        .backlog = 10,
        .bind_addr = NULL,
        .reuseport = false,
        .router = router,
        .read_buf_cap = 0,
        .write_buf_cap = 0
    };

    Server *server = server_create(&config);
    assert(server != NULL);

    EventLoop *loop = server_get_eventloop(server);
    assert(loop != NULL);

    server_destroy(server);
    router_destroy(router);
}

/* 测试 4: 空配置 */
TEST(server_null_config) {
    Server *server = server_create(NULL);
    assert(server == NULL);
}

int main(void) {
    printf("=== Server Module Tests ===\n\n");

    RUN_TEST(server_create_destroy);
    RUN_TEST(server_create_default_router);
    RUN_TEST(server_get_eventloop);
    RUN_TEST(server_null_config);

    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", test_passed);

    return 0;
}