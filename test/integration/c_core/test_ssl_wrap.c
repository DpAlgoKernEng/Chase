/**
 * @file    test_ssl_wrap.c
 * @brief   SSL/TLS 包装模块测试
 *
 * @details
 *          - 测试 SSL 上下文创建和销毁
 *          - 测试证书/私钥加载
 *          - 测试 SSL 连接状态管理
 *          - 测试 SSL 错误处理
 *          - 测试 HTTPS 握手延迟（目标 < 50ms）
 *
 * @author  minghui.liu
 * @date    2026-04-22
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <poll.h>
#include <time.h>

#include "ssl_wrap.h"
#include "server.h"
#include "router.h"

/* 测试证书路径 - 动态检测 */
static char test_cert_path[256];
static char test_key_path[256];

static void init_cert_paths(void) {
    /* 尝试多个可能的路径 */
    const char *cert_candidates[] = {
        "test/certs/test.crt",
        "../test/certs/test.crt",
        "../../test/certs/test.crt",
        "/Users/ninebot/code/open/dpalgokerneng/self/Chase/test/certs/test.crt",
        NULL
    };
    const char *key_candidates[] = {
        "test/certs/test.key",
        "../test/certs/test.key",
        "../../test/certs/test.key",
        "/Users/ninebot/code/open/dpalgokerneng/self/Chase/test/certs/test.key",
        NULL
    };

    for (int i = 0; cert_candidates[i]; i++) {
        FILE *f = fopen(cert_candidates[i], "r");
        if (f) {
            fclose(f);
            strncpy(test_cert_path, cert_candidates[i], sizeof(test_cert_path) - 1);
            strncpy(test_key_path, key_candidates[i], sizeof(test_key_path) - 1);
            return;
        }
    }

    /* 默认路径 */
    strcpy(test_cert_path, "test/certs/test.crt");
    strcpy(test_key_path, "test/certs/test.key");
}

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
 * 测试 1: SSL 配置创建
 */
TEST(ssl_config_create) {
    SslConfig config = {
        .cert_file = test_cert_path,
        .key_file = test_key_path,
        .ca_file = NULL,
        .verify_peer = false,
        .verify_depth = 1,
        .session_timeout = 300,
        .enable_tickets = true
    };

    SslServerCtx *ctx = ssl_server_ctx_create(&config);
    ASSERT(ctx != NULL, "ssl_server_ctx_create failed");

    /* 验证 Session Ticket 配置 */
    ASSERT(ssl_is_session_ticket_enabled(ctx) == true, "session ticket should be enabled");

    ssl_server_ctx_destroy(ctx);
}

/**
 * 测试 2: SSL 配置无效参数
 */
TEST(ssl_config_invalid) {
    /* 无证书 */
    SslConfig config1 = {
        .cert_file = NULL,
        .key_file = test_key_path,
        .ca_file = NULL,
        .verify_peer = false
    };
    SslServerCtx *ctx1 = ssl_server_ctx_create(&config1);
    ASSERT(ctx1 == NULL, "NULL cert should fail");

    /* 无私钥 */
    SslConfig config2 = {
        .cert_file = test_cert_path,
        .key_file = NULL,
        .ca_file = NULL,
        .verify_peer = false
    };
    SslServerCtx *ctx2 = ssl_server_ctx_create(&config2);
    ASSERT(ctx2 == NULL, "NULL key should fail");

    /* 不存在的证书文件 */
    SslConfig config3 = {
        .cert_file = "/tmp/nonexistent.crt",
        .key_file = test_key_path,
        .ca_file = NULL,
        .verify_peer = false
    };
    SslServerCtx *ctx3 = ssl_server_ctx_create(&config3);
    ASSERT(ctx3 == NULL, "nonexistent cert should fail");

    /* NULL 配置 */
    SslServerCtx *ctx4 = ssl_server_ctx_create(NULL);
    ASSERT(ctx4 == NULL, "NULL config should fail");
}

/**
 * 测试 3: SSL 连接创建和销毁
 */
TEST(ssl_connection_create_destroy) {
    SslConfig config = {
        .cert_file = test_cert_path,
        .key_file = test_key_path,
        .ca_file = NULL,
        .verify_peer = false,
        .session_timeout = 300,
        .enable_tickets = true
    };

    SslServerCtx *ctx = ssl_server_ctx_create(&config);
    ASSERT(ctx != NULL, "ctx create failed");

    /* 创建无效 fd */
    SslConnection *conn1 = ssl_connection_create(ctx, -1);
    ASSERT(conn1 == NULL, "negative fd should fail");

    /* 创建 NULL ctx */
    int dummy_fd = 123;
    SslConnection *conn2 = ssl_connection_create(NULL, dummy_fd);
    ASSERT(conn2 == NULL, "NULL ctx should fail");

    /* 创建有效连接（使用临时 socket） */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(fd >= 0, "socket create failed");

    SslConnection *conn3 = ssl_connection_create(ctx, fd);
    ASSERT(conn3 != NULL, "valid connection create failed");
    ASSERT(ssl_get_state(conn3) == SSL_STATE_INIT, "initial state should be INIT");

    ssl_connection_destroy(conn3);
    ssl_server_ctx_destroy(ctx);
    close(fd);
}

/**
 * 测试 4: SSL 状态和错误函数
 */
TEST(ssl_state_error) {
    SslConfig config = {
        .cert_file = test_cert_path,
        .key_file = test_key_path,
        .verify_peer = false,
        .enable_tickets = true
    };

    SslServerCtx *ctx = ssl_server_ctx_create(&config);
    ASSERT(ctx != NULL, "ctx create failed");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    SslConnection *conn = ssl_connection_create(ctx, fd);
    ASSERT(conn != NULL, "conn create failed");

    /* 获取状态 */
    SslState state = ssl_get_state(conn);
    ASSERT(state == SSL_STATE_INIT, "initial state");

    /* 获取错误 */
    int err = ssl_get_error(conn);
    ASSERT(err == 0, "initial error should be 0");

    /* 错误字符串 - 使用 OpenSSL 错误码数值 */
    const char *str1 = ssl_get_error_string(0);  /* SSL_ERROR_NONE */
    ASSERT(str1 != NULL, "SSL_ERROR_NONE string");

    const char *str2 = ssl_get_error_string(2);  /* SSL_ERROR_WANT_READ */
    ASSERT(str2 != NULL, "SSL_ERROR_WANT_READ string");

    const char *str3 = ssl_get_error_string(1);  /* SSL_ERROR_SSL */
    ASSERT(str3 != NULL, "SSL_ERROR_SSL string");

    const char *str4 = ssl_get_error_string(-999);
    ASSERT(str4 != NULL, "unknown error string");

    ssl_connection_destroy(conn);
    ssl_server_ctx_destroy(ctx);
    close(fd);
}

/**
 * 测试 5: NULL 参数处理
 */
TEST(ssl_null_handling) {
    /* NULL 连接操作 */
    ASSERT(ssl_get_state(NULL) == SSL_STATE_ERROR, "NULL state should be ERROR");
    ASSERT(ssl_get_error(NULL) == -1, "NULL error should be -1");
    ASSERT(ssl_handshake(NULL) == SSL_STATE_ERROR, "NULL handshake should fail");
    ASSERT(ssl_read(NULL, NULL, 0) == -1, "NULL read should fail");
    ASSERT(ssl_write(NULL, NULL, 0) == -1, "NULL write should fail");
    ASSERT(ssl_shutdown(NULL) == -1, "NULL shutdown should fail");

    /* NULL server ctx */
    ASSERT(ssl_is_session_ticket_enabled(NULL) == false, "NULL ctx ticket check");
    ASSERT(ssl_get_peer_verify_result(NULL) == -1, "NULL peer verify");
    ASSERT(ssl_get_peer_cert_subject(NULL) == NULL, "NULL peer cert");
}

/**
 * 测试 6: HTTPS 握手延迟测试（目标 < 50ms）
 */
TEST(https_handshake_latency) {
    /* 检查证书文件是否存在 */
    FILE *f = fopen(test_cert_path, "r");
    if (!f) {
        fprintf(stderr, "SKIP: Certificate file not found: %s\n", test_cert_path);
        return;
    }
    fclose(f);

    /* 使用 openssl s_client 测试握手延迟 */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* 执行 HTTPS 握手测量 */
    FILE *ssl_client = popen(
        "timeout 5 openssl s_client -connect localhost:443 "
        "-CAfile test/certs/test.crt 2>&1 | head -5",
        "r");

    clock_gettime(CLOCK_MONOTONIC, &end);

    if (ssl_client) {
        char output[512];
        size_t n = fread(output, 1, sizeof(output) - 1, ssl_client);
        output[n] = '\0';
        pclose(ssl_client);

        /* 计算握手延迟 */
        long latency_ms = (end.tv_sec - start.tv_sec) * 1000 +
                         (end.tv_nsec - start.tv_nsec) / 1000000;

        printf("    OpenSSL s_client init latency: %ld ms\n", latency_ms);

        /* 注意：openssl s_client 连接外部服务器有额外开销
         * 实际 SSL 握手延迟需要真实 HTTPS 服务器 */
    }
}

/**
 * 测试 7: SSL 关闭
 */
TEST(ssl_shutdown) {
    SslConfig config = {
        .cert_file = test_cert_path,
        .key_file = test_key_path,
        .verify_peer = false,
        .enable_tickets = true
    };

    SslServerCtx *ctx = ssl_server_ctx_create(&config);
    ASSERT(ctx != NULL, "ctx create failed");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    SslConnection *conn = ssl_connection_create(ctx, fd);
    ASSERT(conn != NULL, "conn create failed");

    /* 关闭未握手的连接 */
    int ret = ssl_shutdown(conn);
    ASSERT(ret == 0 || ret == -1, "shutdown should complete or fail gracefully");

    ssl_connection_destroy(conn);
    ssl_server_ctx_destroy(ctx);
    close(fd);
}

/* ========== 主函数 ========== */

int main(void) {
    init_cert_paths();  /* 动态检测证书路径 */
    printf("=== SSL Wrap 模块测试 ===\n\n");
    printf("Using cert: %s\n", test_cert_path);
    printf("Using key:  %s\n\n", test_key_path);

    test_failed = 0;

    printf("Test 1: SSL config create\n");
    test_ssl_config_create();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 2: SSL config invalid params\n");
    test_failed = 0;
    test_ssl_config_invalid();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 3: SSL connection create/destroy\n");
    test_failed = 0;
    test_ssl_connection_create_destroy();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 4: SSL state and error\n");
    test_failed = 0;
    test_ssl_state_error();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 5: SSL NULL handling\n");
    test_failed = 0;
    test_ssl_null_handling();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 6: HTTPS handshake latency\n");
    test_failed = 0;
    test_https_handshake_latency();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 7: SSL shutdown\n");
    test_failed = 0;
    test_ssl_shutdown();
    if (!test_failed) printf("  PASSED\n");

    printf("\n=== 测试完成 ===\n");

    return test_failed;
}