/**
 * @file    test_security.c
 * @brief   Security 模块测试
 *
 * @details
 *          - 基础生命周期测试
 *          - 连接限制测试
 *          - 速率限制测试
 *          - Slowloris 检测测试
 *          - IP 封禁测试
 *          - IPv4/IPv6 解析测试
 *
 * @layer   Test Layer
 *
 * @depends security
 *
 * @author  minghui.liu
 * @date    2026-04-23
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "security.h"

static int test_count = 0;
static int test_passed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("Test %d: %s\n", ++test_count, #name); \
    test_##name(); \
    test_passed++; \
} while(0)

/* 辅助函数：创建 IPv4 地址 */
static IpAddress make_ipv4(const char *str) {
    IpAddress ip;
    security_string_to_ip(str, &ip);
    return ip;
}

/* 辅助函数：创建 IPv6 地址 */
static IpAddress make_ipv6(const char *str) {
    IpAddress ip;
    security_string_to_ip(str, &ip);
    return ip;
}

/* ============== 测试用例 ============== */

TEST(create_destroy) {
    SecurityConfig config = {
        .max_connections_per_ip = 10,
        .max_requests_per_second = 100,
        .min_request_rate = 50,
        .slowloris_timeout_ms = 30000,
        .block_duration_ms = 60000,
        .shard_count = 16
    };

    Security *security = security_create(&config);
    assert(security != NULL);

    /* 验证默认配置已应用 */
    int tracked = 0, blocked = 0;
    security_get_summary(security, &tracked, &blocked);
    assert(tracked == 0);
    assert(blocked == 0);

    security_destroy(security);
    printf("  PASS\n");
}

TEST(connection_limit) {
    SecurityConfig config = {
        .max_connections_per_ip = 3,  /* 小限制便于测试 */
        .max_requests_per_second = 100,
        .min_request_rate = 50,
        .slowloris_timeout_ms = 30000,
        .block_duration_ms = 60000,
        .shard_count = 16
    };

    Security *security = security_create(&config);
    assert(security != NULL);

    IpAddress ip = make_ipv4("192.168.1.1");

    /* 前 3 个连接应该允许 */
    assert(security_check_connection(security, &ip) == SECURITY_OK);
    security_add_connection(security, &ip);

    assert(security_check_connection(security, &ip) == SECURITY_OK);
    security_add_connection(security, &ip);

    assert(security_check_connection(security, &ip) == SECURITY_OK);
    security_add_connection(security, &ip);

    /* 第 4 个应该拒绝 */
    SecurityResult result = security_check_connection(security, &ip);
    assert(result == SECURITY_TOO_MANY_CONN);

    /* 移除一个连接后应该允许 */
    security_remove_connection(security, &ip);
    result = security_check_connection(security, &ip);
    assert(result == SECURITY_OK);

    /* 清理 */
    security_remove_connection(security, &ip);
    security_remove_connection(security, &ip);
    assert(security_check_connection(security, &ip) == SECURITY_OK);

    security_destroy(security);
    printf("  PASS\n");
}

TEST(rate_limit) {
    SecurityConfig config = {
        .max_connections_per_ip = 10,
        .max_requests_per_second = 5,  /* 小限制便于测试 */
        .min_request_rate = 50,
        .slowloris_timeout_ms = 30000,
        .block_duration_ms = 1000,  /* 短封禁时间便于测试 */
        .shard_count = 16
    };

    Security *security = security_create(&config);
    assert(security != NULL);

    IpAddress ip = make_ipv4("10.0.0.1");

    /* 快速发送请求（模拟同一秒内） */
    SecurityResult result;
    for (int i = 0; i < 5; i++) {
        result = security_check_request_rate(security, &ip, 100);
        assert(result == SECURITY_OK);
    }

    /* 第 6 个请求应该触发速率限制 */
    result = security_check_request_rate(security, &ip, 100);
    assert(result == SECURITY_RATE_LIMITED);

    /* IP 应被自动封禁 */
    assert(security_is_blocked(security, &ip) == true);

    /* 等待封禁过期 */
    usleep(1500000);  /* 1.5 秒 */

    /* 封禁过期后应该允许 */
    result = security_check_request_rate(security, &ip, 100);
    assert(result == SECURITY_OK);

    security_destroy(security);
    printf("  PASS\n");
}

TEST(slowloris_detection) {
    SecurityConfig config = {
        .max_connections_per_ip = 10,
        .max_requests_per_second = 100,
        .min_request_rate = 100,  /* 最小 100 bytes/sec */
        .slowloris_timeout_ms = 1000,  /* 1 秒超时 */
        .block_duration_ms = 1000,
        .shard_count = 16
    };

    Security *security = security_create(&config);
    assert(security != NULL);

    IpAddress ip = make_ipv4("172.16.0.1");

    /* 正常请求（高速率） */
    SecurityResult result = security_check_request_rate(security, &ip, 1000);
    assert(result == SECURITY_OK);

    /* 等待超过 slowloris 超时时间 */
    usleep(2000000);  /* 2 秒 */

    /* 极低字节速率（Slowloris） */
    /* elapsed_ms = 2000ms, bytes = 10 */
    /* bytes_per_sec = 10 * 1000 / 2000 = 5 bytes/sec < 100 */
    result = security_check_request_rate(security, &ip, 10);
    assert(result == SECURITY_SLOWLORIS);

    /* IP 应被封禁 */
    assert(security_is_blocked(security, &ip) == true);

    security_destroy(security);
    printf("  PASS\n");
}

TEST(ip_blocking) {
    SecurityConfig config = {
        .max_connections_per_ip = 10,
        .max_requests_per_second = 100,
        .min_request_rate = 50,
        .slowloris_timeout_ms = 30000,
        .block_duration_ms = 60000,
        .shard_count = 16
    };

    Security *security = security_create(&config);
    assert(security != NULL);

    IpAddress ip = make_ipv4("192.168.100.1");

    /* 手动封禁 IP */
    int ret = security_block_ip(security, &ip, 2000);
    assert(ret == 0);

    /* 验证封禁状态 */
    assert(security_is_blocked(security, &ip) == true);

    /* 封禁 IP 的连接应被拒绝 */
    SecurityResult result = security_check_connection(security, &ip);
    assert(result == SECURITY_BLOCKED_IP);

    result = security_check_request_rate(security, &ip, 100);
    assert(result == SECURITY_BLOCKED_IP);

    /* 等待封禁过期 */
    usleep(2500000);  /* 2.5 秒 */

    /* 封禁过期后应允许 */
    result = security_check_connection(security, &ip);
    assert(result == SECURITY_OK);

    /* 手动解封 */
    security_block_ip(security, &ip, 5000);
    assert(security_is_blocked(security, &ip) == true);

    security_unblock_ip(security, &ip);
    assert(security_is_blocked(security, &ip) == false);

    security_destroy(security);
    printf("  PASS\n");
}

TEST(auto_block) {
    SecurityConfig config = {
        .max_connections_per_ip = 10,
        .max_requests_per_second = 3,
        .min_request_rate = 50,
        .slowloris_timeout_ms = 30000,
        .block_duration_ms = 1000,
        .shard_count = 16
    };

    Security *security = security_create(&config);
    assert(security != NULL);

    IpAddress ip = make_ipv4("10.20.30.40");

    /* 超过速率限制触发自动封禁 */
    for (int i = 0; i < 3; i++) {
        security_check_request_rate(security, &ip, 100);
    }

    SecurityResult result = security_check_request_rate(security, &ip, 100);
    assert(result == SECURITY_RATE_LIMITED);

    /* 验证自动封禁 */
    assert(security_is_blocked(security, &ip) == true);

    /* 后续请求被拒绝 */
    result = security_check_connection(security, &ip);
    assert(result == SECURITY_BLOCKED_IP);

    security_destroy(security);
    printf("  PASS\n");
}

TEST(block_expiration) {
    SecurityConfig config = {
        .max_connections_per_ip = 10,
        .max_requests_per_second = 100,
        .min_request_rate = 50,
        .slowloris_timeout_ms = 30000,
        .block_duration_ms = 500,  /* 500ms 短封禁 */
        .shard_count = 16
    };

    Security *security = security_create(&config);
    assert(security != NULL);

    IpAddress ip = make_ipv4("10.10.10.10");

    /* 封禁 IP */
    security_block_ip(security, &ip, 0);  /* 使用默认持续时间 */

    assert(security_is_blocked(security, &ip) == true);

    /* 等待过期 */
    usleep(600000);  /* 600ms */

    /* 过期后自动解封 */
    assert(security_is_blocked(security, &ip) == false);

    SecurityResult result = security_check_connection(security, &ip);
    assert(result == SECURITY_OK);

    security_destroy(security);
    printf("  PASS\n");
}

TEST(ipv4_parse) {
    IpAddress ip;
    int ret;

    /* 字符串转 IP */
    ret = security_string_to_ip("192.168.1.100", &ip);
    assert(ret == 0);
    assert(ip.is_ipv6 == false);

    /* IP 转字符串 */
    char buffer[64];
    ret = security_ip_to_string(&ip, buffer, sizeof(buffer));
    assert(ret > 0);
    assert(strcmp(buffer, "192.168.1.100") == 0);

    /* sockaddr 解析 */
    struct sockaddr_in addr_in;
    addr_in.sin_family = AF_INET;
    inet_pton(AF_INET, "10.0.0.1", &addr_in.sin_addr);

    IpAddress ip2;
    ret = security_parse_ip((struct sockaddr *)&addr_in, &ip2);
    assert(ret == 0);
    assert(ip2.is_ipv6 == false);

    security_ip_to_string(&ip2, buffer, sizeof(buffer));
    assert(strcmp(buffer, "10.0.0.1") == 0);

    printf("  PASS\n");
}

TEST(ipv6_parse) {
    IpAddress ip;
    int ret;

    /* 字符串转 IPv6 */
    ret = security_string_to_ip("2001:db8::1", &ip);
    assert(ret == 0);
    assert(ip.is_ipv6 == true);

    /* IP 转字符串 */
    char buffer[64];
    ret = security_ip_to_string(&ip, buffer, sizeof(buffer));
    assert(ret > 0);
    /* IPv6 表示可能压缩，检查包含关键部分 */
    assert(strstr(buffer, "2001") != NULL);
    assert(strstr(buffer, "db8") != NULL);

    /* sockaddr 解析 */
    struct sockaddr_in6 addr_in6;
    addr_in6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "::1", &addr_in6.sin6_addr);

    IpAddress ip2;
    ret = security_parse_ip((struct sockaddr *)&addr_in6, &ip2);
    assert(ret == 0);
    assert(ip2.is_ipv6 == true);

    security_ip_to_string(&ip2, buffer, sizeof(buffer));
    /* 本地 IPv6 */
    assert(strstr(buffer, "::1") != NULL || strcmp(buffer, "0:0:0:0:0:0:0:1") == 0);

    printf("  PASS\n");
}

TEST(multiple_ips) {
    SecurityConfig config = {
        .max_connections_per_ip = 2,
        .max_requests_per_second = 100,
        .min_request_rate = 50,
        .slowloris_timeout_ms = 30000,
        .block_duration_ms = 60000,
        .shard_count = 16
    };

    Security *security = security_create(&config);
    assert(security != NULL);

    /* 多个不同 IP */
    IpAddress ip1 = make_ipv4("192.168.1.1");
    IpAddress ip2 = make_ipv4("192.168.1.2");
    IpAddress ip3 = make_ipv4("192.168.1.3");

    /* 每个 IP 独立计数 */
    assert(security_check_connection(security, &ip1) == SECURITY_OK);
    security_add_connection(security, &ip1);
    security_add_connection(security, &ip1);
    assert(security_check_connection(security, &ip1) == SECURITY_TOO_MANY_CONN);

    assert(security_check_connection(security, &ip2) == SECURITY_OK);
    security_add_connection(security, &ip2);
    assert(security_check_connection(security, &ip2) == SECURITY_OK);

    assert(security_check_connection(security, &ip3) == SECURITY_OK);
    security_add_connection(security, &ip3);

    /* 封禁一个 IP 不影响其他 */
    security_block_ip(security, &ip1, 60000);
    assert(security_is_blocked(security, &ip1) == true);
    assert(security_is_blocked(security, &ip2) == false);
    assert(security_is_blocked(security, &ip3) == false);

    /* 统计摘要 */
    int tracked = 0, blocked = 0;
    security_get_summary(security, &tracked, &blocked);
    assert(tracked >= 3);
    assert(blocked == 1);

    security_destroy(security);
    printf("  PASS\n");
}

TEST(cleanup) {
    SecurityConfig config = {
        .max_connections_per_ip = 10,
        .max_requests_per_second = 100,
        .min_request_rate = 50,
        .slowloris_timeout_ms = 30000,
        .block_duration_ms = 500,
        .shard_count = 16
    };

    Security *security = security_create(&config);
    assert(security != NULL);

    /* 创建一些条目 */
    IpAddress ip1 = make_ipv4("10.0.0.1");
    IpAddress ip2 = make_ipv4("10.0.0.2");

    security_add_connection(security, &ip1);
    security_add_connection(security, &ip2);

    /* 封禁一个 */
    security_block_ip(security, &ip1, 500);

    int tracked = 0, blocked = 0;
    security_get_summary(security, &tracked, &blocked);
    assert(tracked == 2);
    assert(blocked == 1);

    /* 移除连接 */
    security_remove_connection(security, &ip1);
    security_remove_connection(security, &ip2);

    /* 等待封禁过期 */
    usleep(600000);

    /* 执行清理 */
    security_cleanup(security);

    /* 过期封禁和无活动条目应被清理 */
    security_get_summary(security, &tracked, &blocked);
    assert(blocked == 0);  /* 封禁过期 */

    security_destroy(security);
    printf("  PASS\n");
}

TEST(shard_distribution) {
    SecurityConfig config = {
        .max_connections_per_ip = 10,
        .max_requests_per_second = 100,
        .min_request_rate = 50,
        .slowloris_timeout_ms = 30000,
        .block_duration_ms = 60000,
        .shard_count = 16
    };

    Security *security = security_create(&config);
    assert(security != NULL);

    /* 创建大量不同 IP */
    for (int i = 0; i < 100; i++) {
        char ip_str[32];
        snprintf(ip_str, sizeof(ip_str), "192.168.%d.%d", i / 10, i % 10);
        IpAddress ip = make_ipv4(ip_str);

        security_add_connection(security, &ip);
    }

    int tracked = 0, blocked = 0;
    security_get_summary(security, &tracked, &blocked);
    assert(tracked == 100);
    assert(blocked == 0);

    /* 清理 */
    for (int i = 0; i < 100; i++) {
        char ip_str[32];
        snprintf(ip_str, sizeof(ip_str), "192.168.%d.%d", i / 10, i % 10);
        IpAddress ip = make_ipv4(ip_str);

        security_remove_connection(security, &ip);
    }

    security_cleanup(security);

    security_get_summary(security, &tracked, &blocked);
    assert(tracked == 0);

    security_destroy(security);
    printf("  PASS\n");
}

TEST(get_ip_stats) {
    SecurityConfig config = {
        .max_connections_per_ip = 10,
        .max_requests_per_second = 100,
        .min_request_rate = 50,
        .slowloris_timeout_ms = 30000,
        .block_duration_ms = 60000,
        .shard_count = 16
    };

    Security *security = security_create(&config);
    assert(security != NULL);

    IpAddress ip = make_ipv4("172.16.0.100");

    /* 无条目时应返回 -1 */
    IpStats stats;
    int ret = security_get_ip_stats(security, &ip, &stats);
    assert(ret == -1);

    /* 添加连接 */
    security_add_connection(security, &ip);
    security_add_connection(security, &ip);

    /* 发送请求 */
    security_check_request_rate(security, &ip, 500);
    security_check_request_rate(security, &ip, 300);

    /* 获取统计 */
    ret = security_get_ip_stats(security, &ip, &stats);
    assert(ret == 0);
    assert(stats.connection_count == 2);
    assert(stats.request_count >= 2);
    assert(stats.bytes_received >= 800);
    assert(stats.is_blocked == false);

    /* 封禁后检查 */
    security_block_ip(security, &ip, 60000);
    ret = security_get_ip_stats(security, &ip, &stats);
    assert(ret == 0);
    assert(stats.is_blocked == true);
    assert(stats.block_expire_time > 0);

    security_destroy(security);
    printf("  PASS\n");
}

TEST(default_config) {
    SecurityConfig config = {0};  /* 全零，测试默认值 */

    Security *security = security_create(&config);
    assert(security != NULL);

    /* 验证默认值已应用 */
    IpAddress ip1 = make_ipv4("10.0.0.1");
    assert(security_check_connection(security, &ip1) == SECURITY_OK);

    /* 多次添加连接，验证默认 max_conn_per_ip = 10 */
    IpAddress ip = make_ipv4("10.0.0.1");
    for (int i = 0; i < 10; i++) {
        security_add_connection(security, &ip);
    }

    /* 第 11 个应拒绝 */
    assert(security_check_connection(security, &ip) == SECURITY_TOO_MANY_CONN);

    security_destroy(security);
    printf("  PASS\n");
}

TEST(null_handling) {
    SecurityConfig config = {
        .max_connections_per_ip = 10,
        .max_requests_per_second = 100,
        .shard_count = 16
    };

    Security *security = security_create(&config);
    assert(security != NULL);

    IpAddress ip = make_ipv4("10.0.0.1");

    /* NULL 参数应安全处理 */
    assert(security_create(NULL) == NULL);

    SecurityResult result = security_check_connection(NULL, &ip);
    assert(result == SECURITY_INTERNAL_ERROR);

    result = security_check_connection(security, NULL);
    assert(result == SECURITY_INTERNAL_ERROR);

    security_add_connection(NULL, &ip);  /* 应安全返回 */
    security_add_connection(security, NULL);  /* 应安全返回 */

    security_remove_connection(NULL, &ip);  /* 应安全返回 */
    security_remove_connection(security, NULL);  /* 应安全返回 */

    assert(security_is_blocked(NULL, &ip) == false);
    assert(security_is_blocked(security, NULL) == false);

    assert(security_block_ip(NULL, &ip, 1000) == -1);
    assert(security_block_ip(security, NULL, 1000) == -1);

    IpStats stats;
    assert(security_get_ip_stats(NULL, &ip, &stats) == -1);
    assert(security_get_ip_stats(security, NULL, &stats) == -1);
    assert(security_get_ip_stats(security, &ip, NULL) == -1);

    security_destroy(NULL);  /* 应安全返回 */
    security_cleanup(NULL);  /* 应安全返回 */
    security_get_summary(NULL, NULL, NULL);  /* 应安全返回 */

    security_destroy(security);
    printf("  PASS\n");
}

/* ============== 主函数 ============== */

int main(void) {
    printf("=== Security Module Tests ===\n\n");

    RUN_TEST(create_destroy);
    RUN_TEST(connection_limit);
    RUN_TEST(rate_limit);
    RUN_TEST(slowloris_detection);
    RUN_TEST(ip_blocking);
    RUN_TEST(auto_block);
    RUN_TEST(block_expiration);
    RUN_TEST(ipv4_parse);
    RUN_TEST(ipv6_parse);
    RUN_TEST(multiple_ips);
    RUN_TEST(cleanup);
    RUN_TEST(shard_distribution);
    RUN_TEST(get_ip_stats);
    RUN_TEST(default_config);
    RUN_TEST(null_handling);

    printf("\n=== Test Summary ===\n");
    printf("Total: %d, Passed: %d, Failed: %d\n",
           test_count, test_passed, test_count - test_passed);

    return (test_count == test_passed) ? 0 : 1;
}