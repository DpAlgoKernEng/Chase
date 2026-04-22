/**
 * @file    test_vhost.c
 * @brief   虚拟主机模块测试
 *
 * @details
 *          - 测试精确域名匹配
 *          - 测试通配符域名匹配（仅一级子域名）
 *          - 测试默认虚拟主机
 *          - 测试优先级排序
 *
 * @author  minghui.liu
 * @date    2026-04-22
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "vhost.h"
#include "router.h"

/* 简单测试宏 */
#define TEST(name) static void test_##name()
#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s - %s\n", __func__, msg); \
        test_failed = 1; \
        return; \
    } \
} while(0)

static int test_failed = 0;

/* ========== 测试用例 ========== */

/**
 * 测试 1: VHostManager 创建和销毁
 */
TEST(manager_create_destroy) {
    VHostManager *manager = vhost_manager_create();
    ASSERT(manager != NULL, "vhost_manager_create failed");
    ASSERT(vhost_manager_count(manager) == 0, "initial count should be 0");
    ASSERT(vhost_manager_get_default(manager) == NULL, "no default vhost initially");

    vhost_manager_destroy(manager);
}

/**
 * 测试 2: VirtualHost 创建和销毁
 */
TEST(vhost_create_destroy) {
    Router *router = router_create();
    ASSERT(router != NULL, "router_create failed");

    VirtualHost *vhost = vhost_create("example.com", router, NULL);
    ASSERT(vhost != NULL, "vhost_create failed");
    ASSERT(strcmp(vhost->hostname, "example.com") == 0, "hostname mismatch");
    ASSERT(vhost->router == router, "router mismatch");
    ASSERT(vhost->ssl_ctx == NULL, "ssl_ctx should be NULL");
    ASSERT(vhost->is_wildcard == false, "should not be wildcard");
    ASSERT(vhost->priority == 0, "precise match should have priority 0");

    vhost_destroy(vhost);
    router_destroy(router);
}

/**
 * 测试 3: 添加虚拟主机到管理器
 */
TEST(manager_add_vhost) {
    VHostManager *manager = vhost_manager_create();
    ASSERT(manager != NULL, "manager_create failed");

    Router *router1 = router_create();
    Router *router2 = router_create();

    VirtualHost *vhost1 = vhost_create("example.com", router1, NULL);
    VirtualHost *vhost2 = vhost_create("api.example.com", router2, NULL);

    ASSERT(vhost_manager_add(manager, vhost1) == 0, "add vhost1 failed");
    ASSERT(vhost_manager_count(manager) == 1, "count should be 1");

    ASSERT(vhost_manager_add(manager, vhost2) == 0, "add vhost2 failed");
    ASSERT(vhost_manager_count(manager) == 2, "count should be 2");

    /* 第一个添加的应该是默认 */
    ASSERT(vhost_manager_get_default(manager) == vhost1, "first vhost should be default");

    vhost_manager_destroy(manager);
}

/**
 * 测试 4: 精确域名匹配
 */
TEST(precise_match) {
    VHostManager *manager = vhost_manager_create();
    Router *router1 = router_create();
    Router *router2 = router_create();

    VirtualHost *vhost1 = vhost_create("example.com", router1, NULL);
    VirtualHost *vhost2 = vhost_create("api.example.com", router2, NULL);

    vhost_manager_add(manager, vhost1);
    vhost_manager_add(manager, vhost2);

    /* 精确匹配 */
    VirtualHost *match1 = vhost_manager_match(manager, "example.com");
    ASSERT(match1 == vhost1, "should match example.com");

    VirtualHost *match2 = vhost_manager_match(manager, "api.example.com");
    ASSERT(match2 == vhost2, "should match api.example.com");

    /* 不存在的域名应该返回默认 */
    VirtualHost *match3 = vhost_manager_match(manager, "unknown.com");
    ASSERT(match3 == vhost1, "unknown domain should return default");

    vhost_manager_destroy(manager);
}

/**
 * 测试 5: 通配符域名识别
 */
TEST(wildcard_detection) {
    ASSERT(vhost_is_wildcard("*.example.com") == true, "*.example.com is wildcard");
    ASSERT(vhost_is_wildcard("*.api.example.com") == true, "*.api.example.com is wildcard");
    ASSERT(vhost_is_wildcard("example.com") == false, "example.com is not wildcard");
    ASSERT(vhost_is_wildcard("api.example.com") == false, "api.example.com is not wildcard");
    ASSERT(vhost_is_wildcard("*example.com") == false, "*example.com is not valid wildcard");
    ASSERT(vhost_is_wildcard(NULL) == false, "NULL is not wildcard");
}

/**
 * 测试 6: 通配符域名匹配（一级子域名）
 */
TEST(wildcard_match) {
    /* *.example.com */
    ASSERT(vhost_wildcard_match("*.example.com", "sub.example.com") == true,
           "sub.example.com should match *.example.com");
    ASSERT(vhost_wildcard_match("*.example.com", "api.example.com") == true,
           "api.example.com should match *.example.com");

    /* 不匹配：多级子域名 */
    ASSERT(vhost_wildcard_match("*.example.com", "sub.sub.example.com") == false,
           "sub.sub.example.com should NOT match *.example.com (multi-level)");

    /* 不匹配：域名本身 */
    ASSERT(vhost_wildcard_match("*.example.com", "example.com") == false,
           "example.com should NOT match *.example.com");

    /* 不匹配：其他域名 */
    ASSERT(vhost_wildcard_match("*.example.com", "other.com") == false,
           "other.com should NOT match *.example.com");

    /* 边界情况 */
    ASSERT(vhost_wildcard_match(NULL, "sub.example.com") == false,
           "NULL wildcard should not match");
    ASSERT(vhost_wildcard_match("*.example.com", NULL) == false,
           "NULL hostname should not match");
}

/**
 * 测试 7: 混合匹配（精确 + 通配符）
 */
TEST(mixed_match) {
    VHostManager *manager = vhost_manager_create();

    Router *router1 = router_create();
    Router *router2 = router_create();
    Router *router3 = router_create();

    /* 精确匹配优先级高于通配符 */
    VirtualHost *vhost_wildcard = vhost_create("*.example.com", router1, NULL);
    VirtualHost *vhost_precise = vhost_create("api.example.com", router2, NULL);
    VirtualHost *vhost_other = vhost_create("other.com", router3, NULL);

    vhost_manager_add(manager, vhost_wildcard);
    vhost_manager_add(manager, vhost_precise);
    vhost_manager_add(manager, vhost_other);

    /* api.example.com 精确匹配优先 */
    VirtualHost *match1 = vhost_manager_match(manager, "api.example.com");
    ASSERT(match1 == vhost_precise, "precise match should have higher priority");

    /* sub.example.com 通配符匹配 */
    VirtualHost *match2 = vhost_manager_match(manager, "sub.example.com");
    ASSERT(match2 == vhost_wildcard, "sub.example.com should match wildcard");

    /* test.example.com 通配符匹配 */
    VirtualHost *match3 = vhost_manager_match(manager, "test.example.com");
    ASSERT(match3 == vhost_wildcard, "test.example.com should match wildcard");

    /* unknown.com 返回默认（第一个添加的） */
    VirtualHost *match4 = vhost_manager_match(manager, "unknown.com");
    ASSERT(match4 == vhost_wildcard, "unknown domain should return default");

    vhost_manager_destroy(manager);
}

/**
 * 测试 8: 设置默认虚拟主机
 */
TEST(set_default) {
    VHostManager *manager = vhost_manager_create();

    Router *router1 = router_create();
    Router *router2 = router_create();

    VirtualHost *vhost1 = vhost_create("example.com", router1, NULL);
    VirtualHost *vhost2 = vhost_create("api.example.com", router2, NULL);

    vhost_manager_add(manager, vhost1);
    vhost_manager_add(manager, vhost2);

    /* 初始默认是第一个 */
    ASSERT(vhost_manager_get_default(manager) == vhost1, "initial default should be vhost1");

    /* 设置 vhost2 为默认 */
    ASSERT(vhost_manager_set_default(manager, vhost2) == 0, "set_default failed");
    ASSERT(vhost_manager_get_default(manager) == vhost2, "default should now be vhost2");

    /* 未知域名返回新的默认 */
    VirtualHost *match = vhost_manager_match(manager, "unknown.com");
    ASSERT(match == vhost2, "unknown domain should return new default");

    vhost_manager_destroy(manager);
}

/**
 * 测试 9: 多通配符匹配
 */
TEST(multiple_wildcards) {
    VHostManager *manager = vhost_manager_create();

    Router *router1 = router_create();
    Router *router2 = router_create();

    VirtualHost *vhost1 = vhost_create("*.example.com", router1, NULL);
    VirtualHost *vhost2 = vhost_create("*.api.example.com", router2, NULL);

    vhost_manager_add(manager, vhost1);
    vhost_manager_add(manager, vhost2);

    /* sub.example.com 匹配 *.example.com */
    VirtualHost *match1 = vhost_manager_match(manager, "sub.example.com");
    ASSERT(match1 == vhost1, "sub.example.com should match *.example.com");

    /* test.api.example.com 匹配 *.api.example.com */
    VirtualHost *match2 = vhost_manager_match(manager, "test.api.example.com");
    ASSERT(match2 == vhost2, "test.api.example.com should match *.api.example.com");

    vhost_manager_destroy(manager);
}

/* ========== 主函数 ========== */

int main(void) {
    printf("=== VHost 模块测试 ===\n\n");

    test_failed = 0;

    printf("Test 1: VHostManager create/destroy\n");
    test_manager_create_destroy();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 2: VirtualHost create/destroy\n");
    test_failed = 0;
    test_vhost_create_destroy();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 3: Add vhost to manager\n");
    test_failed = 0;
    test_manager_add_vhost();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 4: Precise hostname match\n");
    test_failed = 0;
    test_precise_match();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 5: Wildcard detection\n");
    test_failed = 0;
    test_wildcard_detection();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 6: Wildcard match (one-level subdomain)\n");
    test_failed = 0;
    test_wildcard_match();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 7: Mixed match (precise + wildcard)\n");
    test_failed = 0;
    test_mixed_match();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 8: Set default vhost\n");
    test_failed = 0;
    test_set_default();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 9: Multiple wildcards\n");
    test_failed = 0;
    test_multiple_wildcards();
    if (!test_failed) printf("  PASSED\n");

    printf("\n=== 测试完成 ===\n");

    return test_failed;
}