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
    assert(read_len == strlen(data));
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

    /* 创建连接（需要 EventLoop） */
    /* Connection 需要 EventLoop，这里简化测试 */
    Connection *conn = connection_create(pipefd[0], NULL);
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

    Connection *conn = connection_create(pipefd[0], NULL);
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

    Connection *conn = connection_create_ex(pipefd[0], NULL,
                                             8192,  /* read_buf_cap */
                                             65536, /* write_buf_cap */
                                             BUFFER_MODE_FIXED);
    assert(conn != NULL);

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

    printf("\n=== All tests passed ===\n");
    return 0;
}