/**
 * @file    test_hot_config.c
 * @brief   Config 热更新测试
 *
 * @details
 *          - 版本追踪测试
 *          - checksum 计算
 *          - 原子更新测试
 *          - 渐进更新测试
 *          - 变更检测测试
 *          - 延迟计算测试
 *          - 连接注册测试
 *          - 部分更新测试
 *          - 验证更新测试
 *          - 回滚测试
 *
 * @layer   Test Layer
 *
 * @depends config
 *
 * @author  minghui.liu
 * @date    2026-04-23
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "config.h"

static int test_count = 0;
static int test_passed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("Test %d: %s\n", ++test_count, #name); \
    fflush(stdout); \
    test_##name(); \
    test_passed++; \
} while(0)

#define TEST_CONFIG_FILE "/tmp/chase_test_config.json"
#define TEST_CONFIG_FILE2 "/tmp/chase_test_config2.json"

/* 创建测试配置文件 */
static void create_test_config(const char *path, int port, int max_conn) {
    FILE *f = fopen(path, "w");
    assert(f != NULL);

    fprintf(f, "{\n");
    fprintf(f, "  \"port\": %d,\n", port);
    fprintf(f, "  \"max_connections\": %d,\n", max_conn);
    fprintf(f, "  \"connection_timeout_ms\": 60000,\n");
    fprintf(f, "  \"keepalive_timeout_ms\": 5000,\n");
    fprintf(f, "  \"max_keepalive_requests\": 100\n");
    fprintf(f, "}\n");

    fclose(f);
}

/* 清理测试文件 */
static void cleanup_test_files(void) {
    unlink(TEST_CONFIG_FILE);
    unlink(TEST_CONFIG_FILE2);
}

/* ============== 测试用例 ============== */

TEST(version_tracking) {
    HttpConfig *config = http_config_create_default();
    assert(config != NULL);

    /* 初始版本为 1 */
    assert(http_config_get_version(config) == 1);

    /* 启用热更新 */
    int ret = http_config_enable_hot_update(config, CONFIG_UPDATE_ATOMIC);
    assert(ret == 0);

    /* 版本仍是 1 */
    assert(http_config_get_version(config) == 1);

    /* 创建测试配置文件 */
    cleanup_test_files();
    create_test_config(TEST_CONFIG_FILE, 8080, 1024);

    /* 加载配置 */
    ConfigLoadOptions options = { .validate_required = false, .load_defaults = true };
    HttpConfig *loaded = http_config_load_from_file(TEST_CONFIG_FILE, &options);
    assert(loaded != NULL);

    /* 合并配置 */
    http_config_merge(config, loaded);

    /* 启用热更新后再更新版本 */
    config->version.version++;
    assert(http_config_get_version(config) == 2);

    http_config_destroy(loaded);
    http_config_destroy(config);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(checksum_calculation) {
    HttpConfig *config1 = http_config_create_default();
    HttpConfig *config2 = http_config_create_default();

    assert(config1 != NULL);
    assert(config2 != NULL);

    /* 相同配置应有相同 checksum */
    uint64_t checksum1 = http_config_calculate_checksum(config1);
    uint64_t checksum2 = http_config_calculate_checksum(config2);
    assert(checksum1 == checksum2);

    /* 修改配置后 checksum 应不同 */
    config1->port = 9090;
    checksum1 = http_config_calculate_checksum(config1);
    assert(checksum1 != checksum2);

    /* 再次修改 config2 到相同值 */
    config2->port = 9090;
    checksum2 = http_config_calculate_checksum(config2);
    assert(checksum1 == checksum2);

    http_config_destroy(config1);
    http_config_destroy(config2);
    printf("  PASS\n");
}

TEST(atomic_update) {
    cleanup_test_files();

    HttpConfig *config = http_config_create_default();
    assert(config != NULL);

    /* 启用原子更新 */
    int ret = http_config_enable_hot_update(config, CONFIG_UPDATE_ATOMIC);
    assert(ret == 0);

    /* 创建初始配置文件 */
    create_test_config(TEST_CONFIG_FILE, 8080, 1024);

    ConfigLoadOptions options = { .validate_required = false, .load_defaults = true };
    HttpConfig *loaded = http_config_load_from_file(TEST_CONFIG_FILE, &options);
    assert(loaded != NULL);
    http_config_merge(config, loaded);
    http_config_destroy(loaded);

    /* 创建新配置文件（不同端口） */
    create_test_config(TEST_CONFIG_FILE2, 9090, 2048);

    /* 执行热更新 */
    ConfigUpdateResult result;
    ret = http_config_hot_update(config, TEST_CONFIG_FILE2, &result);
    assert(ret == 0);
    assert(result.success == true);
    assert(result.new_version > result.old_version);
    assert(result.delay_ms == 0);  /* 原子更新延迟为 0 */

    /* 验证配置已更新 */
    assert(config->port == 9090);
    assert(config->max_connections == 2048);

    http_config_update_result_free(&result);
    http_config_destroy(config);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(gradual_update) {
    cleanup_test_files();

    HttpConfig *config = http_config_create_default();
    assert(config != NULL);

    /* 启用渐进更新 */
    int ret = http_config_enable_hot_update(config, CONFIG_UPDATE_GRADUAL);
    assert(ret == 0);

    /* 创建初始配置文件 */
    create_test_config(TEST_CONFIG_FILE, 8080, 1024);

    ConfigLoadOptions options = { .validate_required = false, .load_defaults = true };
    HttpConfig *loaded = http_config_load_from_file(TEST_CONFIG_FILE, &options);
    assert(loaded != NULL);
    http_config_merge(config, loaded);
    http_config_destroy(loaded);

    /* 注册一些活跃连接 */
    http_config_register_connection(config);
    http_config_register_connection(config);
    assert(config->active_connections == 2);

    /* 创建新配置文件 */
    create_test_config(TEST_CONFIG_FILE2, 9090, 2048);

    /* 执行热更新 */
    ConfigUpdateResult result;
    ret = http_config_hot_update(config, TEST_CONFIG_FILE2, &result);
    assert(ret == 0);
    assert(result.success == true);
    assert(result.active_connections == 2);

    /* 渐进更新应有延迟 */
    int expected_delay = http_config_calculate_update_delay(config, 2);
    assert(result.delay_ms == expected_delay);
    assert(result.delay_ms > 0);

    /* 验证配置已更新 */
    assert(config->port == 9090);

    /* 注销连接 */
    http_config_unregister_connection(config);
    http_config_unregister_connection(config);
    assert(config->active_connections == 0);

    http_config_update_result_free(&result);
    http_config_destroy(config);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(change_detection) {
    cleanup_test_files();

    HttpConfig *config = http_config_create_default();
    assert(config != NULL);

    http_config_enable_hot_update(config, CONFIG_UPDATE_ATOMIC);

    /* 创建配置文件 */
    create_test_config(TEST_CONFIG_FILE, 8080, 1024);

    ConfigLoadOptions options = { .validate_required = false, .load_defaults = true };
    HttpConfig *loaded = http_config_load_from_file(TEST_CONFIG_FILE, &options);
    assert(loaded != NULL);
    http_config_merge(config, loaded);
    config->version.checksum = http_config_calculate_checksum(config);
    http_config_destroy(loaded);

    /* 相同配置文件不应检测到变更 */
    bool changed = http_config_has_changed(config, TEST_CONFIG_FILE);
    assert(changed == false);

    /* 修改配置文件 */
    create_test_config(TEST_CONFIG_FILE, 9090, 2048);

    /* 应检测到变更 */
    changed = http_config_has_changed(config, TEST_CONFIG_FILE);
    assert(changed == true);

    http_config_destroy(config);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(delay_calculation) {
    HttpConfig *config = http_config_create_default();
    assert(config != NULL);

    /* 原子更新：延迟为 0 */
    config->update_policy = CONFIG_UPDATE_ATOMIC;
    int delay = http_config_calculate_update_delay(config, 100);
    assert(delay == 0);

    /* 渐进更新：有连接时应有延迟 */
    config->update_policy = CONFIG_UPDATE_GRADUAL;
    config->keepalive_timeout_ms = 5000;

    delay = http_config_calculate_update_delay(config, 0);
    assert(delay == 100);  /* 基础延迟 */

    delay = http_config_calculate_update_delay(config, 10);
    assert(delay == 100 + 10 * 10);  /* 基础 + 每连接 */

    delay = http_config_calculate_update_delay(config, 1000);
    assert(delay == 5000);  /* 不超过 keepalive_timeout */

    http_config_destroy(config);
    printf("  PASS\n");
}

TEST(connection_registration) {
    HttpConfig *config = http_config_create_default();
    assert(config != NULL);

    assert(config->active_connections == 0);

    /* 注册连接 */
    http_config_register_connection(config);
    assert(config->active_connections == 1);

    http_config_register_connection(config);
    http_config_register_connection(config);
    assert(config->active_connections == 3);

    /* 注销连接 */
    http_config_unregister_connection(config);
    assert(config->active_connections == 2);

    http_config_unregister_connection(config);
    http_config_unregister_connection(config);
    assert(config->active_connections == 0);

    /* 过度注销不应变为负数 */
    http_config_unregister_connection(config);
    assert(config->active_connections == 0);

    http_config_destroy(config);
    printf("  PASS\n");
}

TEST(connection_wait) {
    HttpConfig *config = http_config_create_default();
    assert(config != NULL);

    /* 无连接时应立即返回成功 */
    int ret = http_config_wait_connections_close(config, 1000);
    assert(ret == 0);

    /* 有连接时应等待 */
    http_config_register_connection(config);
    ret = http_config_wait_connections_close(config, 200);
    assert(ret == -1);  /* 超时 */

    /* 异步注销（模拟连接关闭） */
    /* 在实际测试中这里需要多线程 */
    /* 简化测试：先注销再等待 */
    http_config_unregister_connection(config);
    ret = http_config_wait_connections_close(config, 1000);
    assert(ret == 0);

    http_config_destroy(config);
    printf("  PASS\n");
}

TEST(partial_update) {
    cleanup_test_files();

    HttpConfig *config = http_config_create_default();
    assert(config != NULL);

    http_config_enable_hot_update(config, CONFIG_UPDATE_ATOMIC);

    /* 创建初始配置 */
    create_test_config(TEST_CONFIG_FILE, 8080, 1024);
    ConfigLoadOptions options = { .validate_required = false, .load_defaults = true };
    HttpConfig *loaded = http_config_load_from_file(TEST_CONFIG_FILE, &options);
    assert(loaded != NULL);
    http_config_merge(config, loaded);
    http_config_destroy(loaded);

    /* 记录初始值 */
    int old_timeout = config->connection_timeout_ms;
    int old_keepalive = config->keepalive_timeout_ms;

    /* 创建新配置（修改多个字段） */
    create_test_config(TEST_CONFIG_FILE, 9090, 2048);

    /* 只更新 port 字段 */
    const char *fields[] = { "port" };
    int ret = http_config_partial_update(config, TEST_CONFIG_FILE, fields, 1);
    assert(ret == 0);

    /* port 应更新 */
    assert(config->port == 9090);

    /* 其他字段应保持不变 */
    assert(config->max_connections == 1024);  /* 未更新 */
    assert(config->connection_timeout_ms == old_timeout);  /* 未更新 */

    http_config_destroy(config);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(validation_on_update) {
    cleanup_test_files();

    HttpConfig *config = http_config_create_default();
    assert(config != NULL);

    http_config_enable_hot_update(config, CONFIG_UPDATE_ATOMIC);

    /* 创建有效配置文件 */
    create_test_config(TEST_CONFIG_FILE, 8080, 1024);

    /* 创建无效配置文件（端口为负） */
    FILE *f = fopen(TEST_CONFIG_FILE2, "w");
    assert(f != NULL);
    fprintf(f, "{\n");
    fprintf(f, "  \"port\": -1,\n");  /* 无效端口 */
    fprintf(f, "  \"max_connections\": 1024\n");
    fprintf(f, "}\n");
    fclose(f);

    /* 尝试使用无效配置更新 */
    ConfigUpdateResult result;
    int ret = http_config_hot_update(config, TEST_CONFIG_FILE2, &result);
    assert(ret == -1);
    assert(result.success == false);
    assert(result.error_message != NULL);

    /* 原配置应保持不变 */
    assert(config->port == DEFAULT_PORT);  /* 未被更新 */

    http_config_update_result_free(&result);
    http_config_destroy(config);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(rollback) {
    HttpConfig *config = http_config_create_default();
    assert(config != NULL);

    http_config_enable_hot_update(config, CONFIG_UPDATE_ATOMIC);

    /* 版本为 1 */
    assert(http_config_get_version(config) == 1);

    /* 模拟更新 */
    config->version.version = 2;

    /* 回滚 */
    int ret = http_config_rollback(config);
    assert(ret == 0);
    assert(http_config_get_version(config) == 1);

    /* 无法回滚到 0 */
    ret = http_config_rollback(config);
    assert(ret == -1);  /* 无历史版本 */

    http_config_destroy(config);
    printf("  PASS\n");
}

TEST(update_disabled) {
    cleanup_test_files();

    HttpConfig *config = http_config_create_default();
    assert(config != NULL);

    /* 不启用热更新 */
    create_test_config(TEST_CONFIG_FILE, 8080, 1024);

    /* 尝试热更新 */
    ConfigUpdateResult result;
    int ret = http_config_hot_update(config, TEST_CONFIG_FILE, &result);
    assert(ret == -1);
    assert(result.success == false);
    assert(result.error_message != NULL);

    http_config_update_result_free(&result);
    http_config_destroy(config);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(null_handling) {
    HttpConfig *config = http_config_create_default();

    /* NULL 参数测试 */
    assert(http_config_enable_hot_update(NULL, CONFIG_UPDATE_ATOMIC) == -1);
    assert(http_config_get_version(NULL) == 0);
    assert(http_config_calculate_checksum(NULL) == 0);
    assert(http_config_has_changed(NULL, "test.json") == false);
    assert(http_config_has_changed(config, NULL) == false);
    assert(http_config_calculate_update_delay(NULL, 0) == 0);

    http_config_register_connection(NULL);
    http_config_unregister_connection(NULL);
    assert(http_config_wait_connections_close(NULL, 1000) == -1);

    ConfigUpdateResult result;
    assert(http_config_hot_update(NULL, "test.json", &result) == -1);
    assert(http_config_hot_update(config, NULL, &result) == -1);

    const char *fields[] = { "port" };
    assert(http_config_partial_update(NULL, "test.json", fields, 1) == -1);
    assert(http_config_partial_update(config, NULL, fields, 1) == -1);
    assert(http_config_partial_update(config, "test.json", NULL, 1) == -1);
    assert(http_config_partial_update(config, "test.json", fields, 0) == -1);

    assert(http_config_rollback(NULL) == -1);

    http_config_update_result_free(NULL);

    http_config_destroy(config);
    printf("  PASS\n");
}

/* ============== 主函数 ============== */

int main(void) {
    printf("=== Config Hot Update Tests ===\n\n");

    RUN_TEST(version_tracking);
    RUN_TEST(checksum_calculation);
    RUN_TEST(atomic_update);
    RUN_TEST(gradual_update);
    RUN_TEST(change_detection);
    RUN_TEST(delay_calculation);
    RUN_TEST(connection_registration);
    RUN_TEST(connection_wait);
    RUN_TEST(partial_update);
    RUN_TEST(validation_on_update);
    RUN_TEST(rollback);
    RUN_TEST(update_disabled);
    RUN_TEST(null_handling);

    printf("\n=== Test Summary ===\n");
    printf("Total: %d, Passed: %d, Failed: %d\n",
           test_count, test_passed, test_count - test_passed);

    cleanup_test_files();

    return (test_count == test_passed) ? 0 : 1;
}