/**
 * @file    test_connection.c
 * @brief   Connection 模块测试
 *
 * @details
 *          - 测试 Connection 创建和销毁
 *          - 测试 Connection 状态管理
 *          - 测试 Buffer 功能（通过 Connection）
 *
 * @layer   Test
 *
 * @depends connection, buffer
 * @usedby  测试框架
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "connection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

/* 测试 1: 创建和销毁缓冲区 */
static void test_buffer_create(void) {
    printf("Test 1: Buffer create/destroy\n");

    Buffer *buf = buffer_create(1024);
    assert(buf != NULL);
    assert(buffer_capacity(buf) == 1024);
    assert(buffer_available(buf) == 0);

    buffer_destroy(buf);
    printf("  PASS\n");
}

/* 测试 2: 缓冲区写入和读取 */
static void test_buffer_write_read(void) {
    printf("Test 2: Buffer write/read\n");

    Buffer *buf = buffer_create(1024);

    const char *data = "Hello, World!";
    int result = buffer_write(buf, data, strlen(data));
    assert(result == 0);
    assert(buffer_available(buf) == strlen(data));

    char read_buf[256];
    int read_len = buffer_read(buf, read_buf, strlen(data));
    assert(read_len == (int)strlen(data));
    assert(strcmp(read_buf, data) == 0);
    assert(buffer_available(buf) == 0);

    buffer_destroy(buf);
    printf("  PASS\n");
}

/* 测试 3: 缓冲区扩容模式 */
static void test_buffer_mode(void) {
    printf("Test 3: Buffer mode\n");

    /* 固定容量模式 */
    Buffer *fixed = buffer_create_ex(100, BUFFER_MODE_FIXED, 1000);
    assert(fixed != NULL);

    /* 写入不超过容量 */
    char data[100];
    memset(data, 'X', 100);
    int result = buffer_write(fixed, data, 100);
    assert(result == 0);

    /* 超过容量应失败 */
    result = buffer_write(fixed, data, 100);
    assert(result == -1);  /* ERR_BUFFER_OVERFLOW */

    buffer_destroy(fixed);
    printf("  PASS\n");
}

/* 测试 4: 环形缓冲区环绕 */
static void test_buffer_wrap(void) {
    printf("Test 4: Buffer wrap\n");

    Buffer *buf = buffer_create(16);

    /* 写入 10 字节 */
    buffer_write(buf, "ABCDEFGHIJ", 10);

    /* 读取 8 字节 */
    char read_buf[8];
    buffer_read(buf, read_buf, 8);

    /* 再写入 10 字节（触发环绕） */
    buffer_write(buf, "KLMNOPQRST", 10);

    assert(buffer_available(buf) == 12);  /* 10 - 8 + 10 */

    /* 读取剩余 */
    char remaining[20];
    int len = buffer_read(buf, remaining, 12);
    assert(len == 12);

    buffer_destroy(buf);
    printf("  PASS\n");
}

/* 测试 5: 创建和销毁连接 */
static void test_connection_create(void) {
    printf("Test 5: Connection create/destroy\n");

    int pipefd[2];
    pipe(pipefd);

    /* 创建连接（新的 API） */
    Connection *conn = connection_create(pipefd[0], NULL, NULL);
    assert(conn != NULL);
    assert(connection_get_fd(conn) == pipefd[0]);

    connection_destroy(conn);
    close(pipefd[0]);
    close(pipefd[1]);
    printf("  PASS\n");
}

/* 测试 6: 连接状态 */
static void test_connection_state(void) {
    printf("Test 6: Connection state\n");

    int pipefd[2];
    pipe(pipefd);

    Connection *conn = connection_create(pipefd[0], NULL, NULL);
    assert(connection_get_state(conn) == CONN_STATE_CONNECTING);

    connection_set_state(conn, CONN_STATE_READING);
    assert(connection_get_state(conn) == CONN_STATE_READING);

    connection_set_state(conn, CONN_STATE_CLOSING);
    assert(connection_get_state(conn) == CONN_STATE_CLOSING);

    connection_destroy(conn);
    close(pipefd[0]);
    close(pipefd[1]);
    printf("  PASS\n");
}

/* 测试 7: 连接扩展参数 */
static void test_connection_create_ex(void) {
    printf("Test 7: Connection create with extended params\n");

    int pipefd[2];
    pipe(pipefd);

    Connection *conn = connection_create_ex(pipefd[0], NULL, NULL,
                                             8192,  /* read_buf_cap */
                                             65536, /* write_buf_cap */
                                             BUFFER_MODE_FIXED);
    assert(conn != NULL);

    connection_destroy(conn);
    close(pipefd[0]);
    close(pipefd[1]);
    printf("  PASS\n");
}

/* 测试 8: 连接关闭回调测试 */
static int close_callback_count = 0;
static void close_callback_func(int fd, void *user_data) {
    close_callback_count++;
    printf("  Close callback triggered for fd=%d, count=%d\n", fd, close_callback_count);
}

static void test_connection_close_callback(void) {
    printf("Test 8: Connection close callback\n");

    close_callback_count = 0;

    int pipefd[2];
    pipe(pipefd);

    /* 创建带回调的连接 */
    Connection *conn = connection_create(pipefd[0], close_callback_func, NULL);
    assert(conn != NULL);

    /* 关闭连接，触发回调 */
    connection_close(conn);

    /* 验证回调被调用 */
    assert(close_callback_count == 1);

    close(pipefd[0]);
    close(pipefd[1]);
    printf("  PASS\n");
}

/* 测试 9: 连接读/写操作测试 */
static void test_connection_read_write(void) {
    printf("Test 9: Connection read/write operations\n");

    int pipefd[2];
    pipe(pipefd);

    /* 设置非阻塞 */
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);

    Connection *conn = connection_create(pipefd[0], NULL, NULL);
    assert(conn != NULL);

    /* 写入数据到 pipe 写端 */
    const char *data = "TestData123";
    ssize_t written = write(pipefd[1], data, strlen(data));
    assert(written == strlen(data));

    /* 通过 connection_read 读取 */
    int len = connection_read(conn);
    assert(len == strlen(data));

    /* 验证读缓冲区有数据 */
    Buffer *buf = connection_get_read_buffer(conn);
    assert(buf != NULL);
    assert(buffer_available(buf) == strlen(data));

    /* 验证数据内容 */
    char read_buf[256];
    int read_len = buffer_read(buf, read_buf, strlen(data));
    assert(read_len == strlen(data));
    assert(strcmp(read_buf, data) == 0);

    connection_destroy(conn);
    close(pipefd[0]);
    close(pipefd[1]);
    printf("  PASS\n");
}

/* 测试 10: 连接 reset 测试 */
static void test_connection_reset(void) {
    printf("Test 10: Connection reset\n");

    int pipefd[2];
    pipe(pipefd);

    /* 设置非阻塞 */
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);

    Connection *conn = connection_create(pipefd[0], NULL, NULL);
    assert(conn != NULL);

    /* 写入一些数据 */
    const char *data = "TestData";
    write(pipefd[1], data, strlen(data));

    /* 读取数据 */
    connection_read(conn);

    /* 验证缓冲区有数据 */
    Buffer *buf = connection_get_read_buffer(conn);
    assert(buf != NULL);
    assert(buffer_available(buf) == strlen(data));

    /* 验证状态 */
    connection_set_state(conn, CONN_STATE_READING);
    assert(connection_get_state(conn) == CONN_STATE_READING);

    /* 重置连接 */
    connection_reset(conn);

    /* 验证缓冲区已清空 */
    assert(buffer_available(buf) == 0);

    /* 验证状态回到 CONNECTING */
    assert(connection_get_state(conn) == CONN_STATE_CONNECTING);

    connection_destroy(conn);
    close(pipefd[0]);
    close(pipefd[1]);
    printf("  PASS\n");
}

int main(void) {
    printf("=== Connection Module Tests ===\n\n");

    test_buffer_create();
    test_buffer_write_read();
    test_buffer_mode();
    test_buffer_wrap();
    test_connection_create();
    test_connection_state();
    test_connection_create_ex();
    test_connection_close_callback();
    test_connection_read_write();
    test_connection_reset();

    printf("\n=== All tests passed ===\n");
    return 0;
}