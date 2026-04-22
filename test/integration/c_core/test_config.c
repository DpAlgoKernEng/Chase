/**
 * @file    test_config.c
 * @brief   配置模块测试
 *
 * @details
 *          - 测试默认配置创建
 *          - 测试 JSON 配置加载
 *          - 测试配置验证
 *          - 测试配置合并
 *          - 测试配置转换
 *
 * @author  minghui.liu
 * @date    2026-04-22
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "config.h"
#include "server.h"

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
 * 测试 1: 默认配置创建和销毁
 */
TEST(config_create_default) {
    HttpConfig *config = http_config_create_default();
    ASSERT(config != NULL, "http_config_create_default failed");

    /* 验证默认值 */
    ASSERT(config->port == DEFAULT_PORT, "default port mismatch");
    ASSERT(config->max_connections == DEFAULT_MAX_CONNECTIONS, "default max_connections mismatch");
    ASSERT(config->backlog == DEFAULT_BACKLOG, "default backlog mismatch");
    ASSERT(config->connection_timeout_ms == DEFAULT_CONNECTION_TIMEOUT, "default connection timeout mismatch");
    ASSERT(config->keepalive_timeout_ms == DEFAULT_KEEPALIVE_TIMEOUT, "default keepalive timeout mismatch");
    ASSERT(config->max_keepalive_requests == DEFAULT_MAX_KEEPALIVE_REQS, "default max keepalive requests mismatch");

    ASSERT(config->ssl_enabled == false, "ssl should be disabled by default");
    ASSERT(config->bind_address == NULL, "bind_address should be NULL by default");
    ASSERT(config->vhost_manager == NULL, "vhost_manager should be NULL by default");
    ASSERT(config->from_file == false, "should not be from file");

    http_config_destroy(config);
}

/**
 * 测试 2: 配置验证
 */
TEST(config_validate) {
    HttpConfig *config = http_config_create_default();
    ASSERT(config != NULL, "create_default failed");

    /* 有效配置 */
    int ret = http_config_validate(config);
    ASSERT(ret == CONFIG_OK, "default config should be valid");

    /* 无效端口 */
    config->port = -1;
    ret = http_config_validate(config);
    ASSERT(ret == CONFIG_ERR_INVALID_PORT, "invalid port should fail");

    config->port = 65536;
    ret = http_config_validate(config);
    ASSERT(ret == CONFIG_ERR_INVALID_PORT, "port > 65535 should fail");

    /* 恢复有效端口 */
    config->port = 8080;
    ret = http_config_validate(config);
    ASSERT(ret == CONFIG_OK, "valid port should pass");

    /* 无效超时 */
    config->connection_timeout_ms = -1;
    ret = http_config_validate(config);
    ASSERT(ret == CONFIG_ERR_INVALID_TIMEOUT, "negative timeout should fail");

    http_config_destroy(config);
}

/**
 * 测试 3: 从 JSON 文件加载配置
 */
TEST(config_load_from_file) {
    /* 创建临时配置文件 */
    const char *config_content = "{\n"
        "  \"port\": 9090,\n"
        "  \"max_connections\": 2048,\n"
        "  \"backlog\": 256,\n"
        "  \"reuseport\": true,\n"
        "  \"connection_timeout_ms\": 30000,\n"
        "  \"keepalive_timeout_ms\": 10000,\n"
        "  \"max_keepalive_requests\": 200\n"
        "}\n";

    const char *temp_file = "/tmp/test_config.json";
    FILE *f = fopen(temp_file, "w");
    ASSERT(f != NULL, "failed to create temp config file");
    fprintf(f, "%s", config_content);
    fclose(f);

    /* 加载配置 */
    HttpConfig *config = http_config_load_from_file(temp_file, NULL);
    ASSERT(config != NULL, "load_from_file failed");

    /* 验证加载的值 */
    ASSERT(config->port == 9090, "port should be 9090");
    ASSERT(config->max_connections == 2048, "max_connections should be 2048");
    ASSERT(config->backlog == 256, "backlog should be 256");
    ASSERT(config->reuseport == true, "reuseport should be true");
    ASSERT(config->connection_timeout_ms == 30000, "connection_timeout should be 30000");
    ASSERT(config->keepalive_timeout_ms == 10000, "keepalive_timeout should be 10000");
    ASSERT(config->max_keepalive_requests == 200, "max_keepalive_requests should be 200");

    ASSERT(config->from_file == true, "should be marked as from file");
    ASSERT(config->config_file_path != NULL, "config_file_path should be set");

    http_config_destroy(config);
    unlink(temp_file);
}

/**
 * 测试 4: 配置转换为 ServerConfig
 */
TEST(config_to_server_config) {
    HttpConfig *http_config = http_config_create_default();
    ASSERT(http_config != NULL, "create_default failed");

    /* 设置一些值 */
    http_config->port = 8888;
    http_config->max_connections = 512;
    http_config->connection_timeout_ms = 45000;

    /* 转换 */
    ServerConfig server_config = http_config_to_server_config(http_config);

    /* 验证转换 */
    ASSERT(server_config.port == 8888, "port should match");
    ASSERT(server_config.max_connections == 512, "max_connections should match");
    ASSERT(server_config.connection_timeout_ms == 45000, "connection_timeout should match");

    http_config_destroy(http_config);
}

/**
 * 测试 5: 配置错误字符串
 */
TEST(config_error_string) {
    const char *msg = http_config_get_error_string(CONFIG_OK);
    ASSERT(msg != NULL, "error string should not be NULL");

    msg = http_config_get_error_string(CONFIG_ERR_INVALID_PORT);
    ASSERT(msg != NULL, "INVALID_PORT message should exist");

    msg = http_config_get_error_string(CONFIG_ERR_FILE_NOT_FOUND);
    ASSERT(msg != NULL, "FILE_NOT_FOUND message should exist");

    msg = http_config_get_error_string(-999);
    ASSERT(msg != NULL, "unknown error message should exist");
}

/**
 * 测试 6: 无效配置文件
 */
TEST(config_load_invalid_file) {
    /* 不存在的文件 */
    HttpConfig *config = http_config_load_from_file("/tmp/nonexistent.json", NULL);
    ASSERT(config == NULL, "loading nonexistent file should fail");
}

/**
 * 测试 7: 配置合并
 */
TEST(config_merge) {
    HttpConfig *dst = http_config_create_default();
    HttpConfig *src = http_config_create_default();

    ASSERT(dst != NULL && src != NULL, "create_default failed");

    /* 设置 src 的值 */
    src->port = 9999;
    src->max_connections = 4096;
    src->keepalive_timeout_ms = 15000;

    /* 合并 */
    int ret = http_config_merge(dst, src);
    ASSERT(ret == 0, "merge failed");

    /* 验证 dst 被覆盖 */
    ASSERT(dst->port == 9999, "port should be merged");
    ASSERT(dst->max_connections == 4096, "max_connections should be merged");
    ASSERT(dst->keepalive_timeout_ms == 15000, "keepalive_timeout should be merged");

    http_config_destroy(dst);
    http_config_destroy(src);
}

/* ========== 主函数 ========== */

int main(void) {
    printf("=== Config 模块测试 ===\n\n");

    test_failed = 0;

    printf("Test 1: Config create default\n");
    test_config_create_default();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 2: Config validate\n");
    test_failed = 0;
    test_config_validate();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 3: Config load from JSON file\n");
    test_failed = 0;
    test_config_load_from_file();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 4: Config to ServerConfig\n");
    test_failed = 0;
    test_config_to_server_config();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 5: Config error string\n");
    test_failed = 0;
    test_config_error_string();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 6: Config load invalid file\n");
    test_failed = 0;
    test_config_load_invalid_file();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 7: Config merge\n");
    test_failed = 0;
    test_config_merge();
    if (!test_failed) printf("  PASSED\n");

    printf("\n=== 测试完成 ===\n");

    return test_failed;
}