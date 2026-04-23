/**
 * @file    test_logger.c
 * @brief   Logger 模块测试
 *
 * @details
 *          - 基础生命周期测试
 *          - 日志级别测试
 *          - Ring Buffer 测试
 *          - 请求/安全审计测试
 *
 * @layer   Test Layer
 *
 * @depends logger
 *
 * @author  minghui.liu
 * @date    2026-04-23
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

#include "logger.h"

#define TEST_LOG_FILE "/tmp/chase_test_logger.log"
#define TEST_AUDIT_FILE "/tmp/chase_test_audit.log"

static int test_count = 0;
static int test_passed = 0;

#define TEST(name) static void test_##name()
#define RUN_TEST(name) do { \
    printf("Test %d: %s\n", ++test_count, #name); \
    test_##name(); \
    test_passed++; \
} while(0)

/* 清理测试文件 */
static void cleanup_test_files(void) {
    unlink(TEST_LOG_FILE);
    unlink(TEST_AUDIT_FILE);
}

/* ============== 测试用例 ============== */

TEST(create_destroy) {
    cleanup_test_files();

    LoggerConfig config = {
        .log_file = TEST_LOG_FILE,
        .audit_file = TEST_AUDIT_FILE,
        .min_level = LOG_INFO,
        .format = LOG_FORMAT_TEXT,
        .ring_buffer_size = LOGGER_DEFAULT_RING_BUFFER_SIZE,
        .flush_interval_ms = LOGGER_DEFAULT_FLUSH_INTERVAL,
        .enable_stdout = false
    };

    Logger *logger = logger_create(&config);
    assert(logger != NULL);

    logger_destroy(logger);

    /* 验证文件已创建 */
    FILE *f = fopen(TEST_LOG_FILE, "r");
    assert(f != NULL);
    fclose(f);

    cleanup_test_files();
    printf("  PASS\n");
}

TEST(log_levels) {
    cleanup_test_files();

    LoggerConfig config = {
        .log_file = TEST_LOG_FILE,
        .min_level = LOG_DEBUG,
        .format = LOG_FORMAT_TEXT,
        .enable_stdout = false
    };

    Logger *logger = logger_create(&config);
    assert(logger != NULL);

    /* 测试所有日志级别 */
    logger_log(logger, LOG_DEBUG, "Debug message");
    logger_log(logger, LOG_INFO, "Info message");
    logger_log(logger, LOG_WARN, "Warn message");
    logger_log(logger, LOG_ERROR, "Error message");
    logger_log(logger, LOG_SECURITY, "Security message");

    /* 等待写入完成 */
    usleep(100000);  /* 100ms */
    logger_flush(logger);

    /* 验证日志文件内容 */
    FILE *f = fopen(TEST_LOG_FILE, "r");
    assert(f != NULL);

    char line[1024];
    bool found_debug = false, found_info = false, found_warn = false;
    bool found_error = false, found_security = false;
    int line_count = 0;

    while (fgets(line, sizeof(line), f)) {
        line_count++;
        if (strstr(line, "DEBUG")) found_debug = true;
        if (strstr(line, "INFO") && !strstr(line, "SECURITY")) found_info = true;
        if (strstr(line, "WARN")) found_warn = true;
        if (strstr(line, "ERROR")) found_error = true;
        if (strstr(line, "SECURITY")) found_security = true;
    }
    fclose(f);

    assert(line_count == 5);  /* 应有5行日志 */
    assert(found_debug == true);
    assert(found_info == true);
    assert(found_warn == true);
    assert(found_error == true);
    assert(found_security == true);

    logger_destroy(logger);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(debug_filtered) {
    cleanup_test_files();

    LoggerConfig config = {
        .log_file = TEST_LOG_FILE,
        .min_level = LOG_INFO,  /* DEBUG 应被过滤 */
        .format = LOG_FORMAT_TEXT,
        .enable_stdout = false
    };

    Logger *logger = logger_create(&config);
    assert(logger != NULL);

    /* DEBUG 级别应被过滤 */
    logger_log(logger, LOG_DEBUG, "This debug should be filtered");
    logger_log(logger, LOG_INFO, "This info should appear");

    usleep(100000);
    logger_flush(logger);

    /* 验证日志文件 */
    FILE *f = fopen(TEST_LOG_FILE, "r");
    assert(f != NULL);

    char line[1024];
    bool found_debug = false;
    bool found_info = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "DEBUG") && strstr(line, "filtered")) {
            found_debug = true;
        }
        if (strstr(line, "INFO") && strstr(line, "appear")) {
            found_info = true;
        }
    }
    fclose(f);

    assert(found_debug == false);  /* DEBUG 应被过滤 */
    assert(found_info == true);     /* INFO 应出现 */

    logger_destroy(logger);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(request_logging) {
    cleanup_test_files();

    LoggerConfig config = {
        .log_file = TEST_LOG_FILE,
        .min_level = LOG_INFO,
        .format = LOG_FORMAT_TEXT,
        .enable_stdout = false
    };

    Logger *logger = logger_create(&config);
    assert(logger != NULL);

    RequestLogContext ctx = {
        .method = "GET",
        .path = "/api/test",
        .query = "id=123",
        .status_code = 200,
        .latency_ms = 15,
        .client_ip = "192.168.1.1",
        .bytes_sent = 1024,
        .bytes_received = 256
    };

    logger_log_request(logger, &ctx);

    usleep(100000);
    logger_flush(logger);

    /* 验证日志文件 */
    FILE *f = fopen(TEST_LOG_FILE, "r");
    assert(f != NULL);

    char line[2048];
    bool found_method = false;
    bool found_path = false;
    bool found_latency = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "GET")) found_method = true;
        if (strstr(line, "/api/test")) found_path = true;
        if (strstr(line, "15ms")) found_latency = true;
    }
    fclose(f);

    assert(found_method == true);
    assert(found_path == true);
    assert(found_latency == true);

    logger_destroy(logger);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(security_audit) {
    cleanup_test_files();

    LoggerConfig config = {
        .log_file = TEST_LOG_FILE,
        .audit_file = TEST_AUDIT_FILE,  /* 审计日志独立文件 */
        .min_level = LOG_INFO,
        .format = LOG_FORMAT_TEXT,
        .enable_stdout = false
    };

    Logger *logger = logger_create(&config);
    assert(logger != NULL);

    SecurityLogContext ctx = {
        .event_type = "test_event",
        .client_ip = "10.0.0.1",
        .details = "Test security event",
        .severity = 3,
        .blocked = true
    };

    logger_log_security(logger, &ctx);

    usleep(100000);
    logger_flush(logger);

    /* 验证审计日志文件 */
    FILE *f = fopen(TEST_AUDIT_FILE, "r");
    assert(f != NULL);

    char line[1024];
    bool found_security = false;
    bool found_event = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "SECURITY")) found_security = true;
        if (strstr(line, "test_event")) found_event = true;
    }
    fclose(f);

    assert(found_security == true);
    assert(found_event == true);

    logger_destroy(logger);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(path_traversal_audit) {
    cleanup_test_files();

    LoggerConfig config = {
        .log_file = TEST_LOG_FILE,
        .audit_file = TEST_AUDIT_FILE,
        .min_level = LOG_INFO,
        .format = LOG_FORMAT_TEXT,
        .enable_stdout = false
    };

    Logger *logger = logger_create(&config);
    assert(logger != NULL);

    logger_log_path_traversal(logger, "192.168.1.100",
                               "/../../../etc/passwd",
                               "/var/www");

    usleep(100000);
    logger_flush(logger);

    /* 验证审计日志 */
    FILE *f = fopen(TEST_AUDIT_FILE, "r");
    assert(f != NULL);

    char line[2048];
    bool found_traversal = false;
    bool found_path = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "path_traversal")) found_traversal = true;
        if (strstr(line, "../")) found_path = true;
    }
    fclose(f);

    assert(found_traversal == true);
    assert(found_path == true);

    logger_destroy(logger);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(rate_limit_audit) {
    cleanup_test_files();

    LoggerConfig config = {
        .log_file = TEST_LOG_FILE,
        .audit_file = TEST_AUDIT_FILE,
        .min_level = LOG_INFO,
        .format = LOG_FORMAT_TEXT,
        .enable_stdout = false
    };

    Logger *logger = logger_create(&config);
    assert(logger != NULL);

    logger_log_rate_limit(logger, "10.20.30.40",
                          "connection_rate",
                          25, 20);

    usleep(100000);
    logger_flush(logger);

    /* 验证审计日志 */
    FILE *f = fopen(TEST_AUDIT_FILE, "r");
    assert(f != NULL);

    char line[1024];
    bool found_rate = false;
    bool found_count = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "rate_limit") || strstr(line, "connection_rate")) found_rate = true;
        if (strstr(line, "count=25")) found_count = true;
    }
    fclose(f);

    assert(found_rate == true);
    assert(found_count == true);

    logger_destroy(logger);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(json_format) {
    cleanup_test_files();

    LoggerConfig config = {
        .log_file = TEST_LOG_FILE,
        .min_level = LOG_INFO,
        .format = LOG_FORMAT_JSON,
        .enable_stdout = false
    };

    Logger *logger = logger_create(&config);
    assert(logger != NULL);

    logger_log(logger, LOG_INFO, "JSON test message");

    usleep(100000);
    logger_flush(logger);

    /* 验证 JSON 格式 */
    FILE *f = fopen(TEST_LOG_FILE, "r");
    assert(f != NULL);

    char line[1024];
    bool found_json = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "{\"timestamp\"")) found_json = true;
        if (strstr(line, "\"level\":\"INFO\"")) found_json = true;
        if (strstr(line, "\"message\"")) found_json = true;
    }
    fclose(f);

    assert(found_json == true);

    logger_destroy(logger);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(text_format) {
    cleanup_test_files();

    LoggerConfig config = {
        .log_file = TEST_LOG_FILE,
        .min_level = LOG_INFO,
        .format = LOG_FORMAT_TEXT,
        .enable_stdout = false
    };

    Logger *logger = logger_create(&config);
    assert(logger != NULL);

    logger_log(logger, LOG_INFO, "Text test message");

    usleep(100000);
    logger_flush(logger);

    /* 验证文本格式 */
    FILE *f = fopen(TEST_LOG_FILE, "r");
    assert(f != NULL);

    char line[1024];
    bool found_text = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "[INFO]")) found_text = true;
        if (strstr(line, "Text test message")) found_text = true;
    }
    fclose(f);

    assert(found_text == true);

    logger_destroy(logger);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(ring_buffer_overflow) {
    cleanup_test_files();

    /* 小 Ring Buffer 容易溢出 */
    LoggerConfig config = {
        .log_file = TEST_LOG_FILE,
        .min_level = LOG_DEBUG,
        .format = LOG_FORMAT_TEXT,
        .ring_buffer_size = 4096,  /* 小缓冲区 */
        .enable_stdout = false
    };

    Logger *logger = logger_create(&config);
    assert(logger != NULL);

    /* 大量日志，测试溢出 */
    for (int i = 0; i < 1000; i++) {
        logger_log(logger, LOG_DEBUG, "Overflow test message %d", i);
    }

    /* ERROR 级别即使溢出也会直接写入 */
    logger_log(logger, LOG_ERROR, "Critical error message");

    usleep(200000);  /* 200ms */
    logger_flush(logger);

    /* 验证关键日志存在 */
    FILE *f = fopen(TEST_LOG_FILE, "r");
    assert(f != NULL);

    char line[1024];
    bool found_error = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "ERROR") && strstr(line, "Critical")) {
            found_error = true;
        }
    }
    fclose(f);

    assert(found_error == true);

    logger_destroy(logger);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(flush) {
    cleanup_test_files();

    LoggerConfig config = {
        .log_file = TEST_LOG_FILE,
        .min_level = LOG_INFO,
        .format = LOG_FORMAT_TEXT,
        .enable_stdout = false
    };

    Logger *logger = logger_create(&config);
    assert(logger != NULL);

    logger_log(logger, LOG_INFO, "Before flush");

    /* 立即刷新 */
    logger_flush(logger);

    /* 验证日志已写入 */
    FILE *f = fopen(TEST_LOG_FILE, "r");
    assert(f != NULL);

    char line[1024];
    bool found = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "Before flush")) found = true;
    }
    fclose(f);

    assert(found == true);

    logger_log(logger, LOG_INFO, "After flush");
    logger_destroy(logger);

    cleanup_test_files();
    printf("  PASS\n");
}

TEST(stdout_dual) {
    cleanup_test_files();

    LoggerConfig config = {
        .log_file = TEST_LOG_FILE,
        .min_level = LOG_INFO,
        .format = LOG_FORMAT_TEXT,
        .enable_stdout = true  /* 同时输出到 stdout */
    };

    Logger *logger = logger_create(&config);
    assert(logger != NULL);

    logger_log(logger, LOG_INFO, "Stdout dual test");

    usleep(100000);
    logger_flush(logger);

    /* 验证文件存在 */
    FILE *f = fopen(TEST_LOG_FILE, "r");
    assert(f != NULL);

    char line[1024];
    bool found = false;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "Stdout dual test")) found = true;
    }
    fclose(f);

    assert(found == true);

    logger_destroy(logger);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(set_get_level) {
    cleanup_test_files();

    LoggerConfig config = {
        .log_file = TEST_LOG_FILE,
        .min_level = LOG_INFO,
        .format = LOG_FORMAT_TEXT,
        .enable_stdout = false
    };

    Logger *logger = logger_create(&config);
    assert(logger != NULL);

    assert(logger_get_level(logger) == LOG_INFO);

    logger_set_level(logger, LOG_DEBUG);
    assert(logger_get_level(logger) == LOG_DEBUG);

    logger_set_level(logger, LOG_ERROR);
    assert(logger_get_level(logger) == LOG_ERROR);

    logger_destroy(logger);
    cleanup_test_files();
    printf("  PASS\n");
}

TEST(level_name) {
    assert(strcmp(logger_level_name(LOG_DEBUG), "DEBUG") == 0);
    assert(strcmp(logger_level_name(LOG_INFO), "INFO") == 0);
    assert(strcmp(logger_level_name(LOG_WARN), "WARN") == 0);
    assert(strcmp(logger_level_name(LOG_ERROR), "ERROR") == 0);
    assert(strcmp(logger_level_name(LOG_SECURITY), "SECURITY") == 0);
    printf("  PASS\n");
}

TEST(timestamp_format) {
    struct timespec ts = {
        .tv_sec = 1700000000,
        .tv_nsec = 123000000
    };

    char buffer[64];
    int len = logger_format_timestamp(&ts, buffer, sizeof(buffer));

    assert(len > 0);
    assert(strstr(buffer, "2023") != NULL);  /* 年份 */
    assert(strstr(buffer, ".123") != NULL);  /* 毫秒 */

    printf("  PASS\n");
}

TEST(get_current_ms) {
    uint64_t t1 = logger_get_current_ms();
    usleep(10000);  /* 10ms */
    uint64_t t2 = logger_get_current_ms();

    assert(t2 > t1);
    assert(t2 - t1 >= 8);  /* 至少 8ms（考虑系统精度） */
    assert(t2 - t1 <= 20); /* 不超过 20ms */

    printf("  PASS\n");
}

/* ============== 主函数 ============== */

int main(void) {
    printf("=== Logger Module Tests ===\n\n");

    RUN_TEST(create_destroy);
    RUN_TEST(log_levels);
    RUN_TEST(debug_filtered);
    RUN_TEST(request_logging);
    RUN_TEST(security_audit);
    RUN_TEST(path_traversal_audit);
    RUN_TEST(rate_limit_audit);
    RUN_TEST(json_format);
    RUN_TEST(text_format);
    RUN_TEST(ring_buffer_overflow);
    RUN_TEST(flush);
    RUN_TEST(stdout_dual);
    RUN_TEST(set_get_level);
    RUN_TEST(level_name);
    RUN_TEST(timestamp_format);
    RUN_TEST(get_current_ms);

    printf("\n=== Test Summary ===\n");
    printf("Total: %d, Passed: %d, Failed: %d\n",
           test_count, test_passed, test_count - test_passed);

    cleanup_test_files();

    return (test_count == test_passed) ? 0 : 1;
}