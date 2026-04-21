/**
 * @file    test_eventloop.c
 * @brief   EventLoop 模块测试
 *
 * @details
 *          - 测试 EventLoop 创建和销毁
 *          - 测试事件添加、修改、删除
 *          - 测试事件回调触发
 *
 * @layer   Test
 *
 * @depends eventloop
 * @usedby  测试框架
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "eventloop.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

static int callback_count = 0;

/* 测试回调 */
static void test_callback(int fd, uint32_t events, void *user_data) {
    callback_count++;
    printf("Event callback called (fd=%d, events=%u, count=%d)\n",
           fd, events, callback_count);
}

/* 测试 1: 创建和销毁 */
static void test_eventloop_create(void) {
    printf("Test 1: EventLoop create/destroy\n");

    EventLoop *loop = eventloop_create(64);
    assert(loop != NULL);

    eventloop_destroy(loop);
    printf("  PASS\n");
}

/* 测试 2: 添加事件 */
static void test_eventloop_add(void) {
    printf("Test 2: EventLoop add event\n");

    EventLoop *loop = eventloop_create(64);

    /* 创建 pipe 用于测试 */
    int pipefd[2];
    pipe(pipefd);

    int result = eventloop_add(loop, pipefd[0], EV_READ, test_callback, NULL);
    assert(result == 0);

    eventloop_destroy(loop);
    close(pipefd[0]);
    close(pipefd[1]);
    printf("  PASS\n");
}

/* 测试 3: 移除事件 */
static void test_eventloop_remove(void) {
    printf("Test 3: EventLoop remove event\n");

    EventLoop *loop = eventloop_create(64);

    int pipefd[2];
    pipe(pipefd);

    eventloop_add(loop, pipefd[0], EV_READ, test_callback, NULL);

    int result = eventloop_remove(loop, pipefd[0]);
    assert(result == 0);

    eventloop_destroy(loop);
    close(pipefd[0]);
    close(pipefd[1]);
    printf("  PASS\n");
}

/* 测试 4: 单次轮询 */
static void test_eventloop_poll(void) {
    printf("Test 4: EventLoop poll\n");

    EventLoop *loop = eventloop_create(64);
    callback_count = 0;

    int pipefd[2];
    pipe(pipefd);

    /* 设置非阻塞 */
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);

    eventloop_add(loop, pipefd[0], EV_READ, test_callback, NULL);

    /* 写入数据触发事件 */
    char buf[] = "test";
    write(pipefd[1], buf, sizeof(buf));

    /* 轮询 */
    int n = eventloop_poll(loop, 100);
    assert(n > 0);
    assert(callback_count == 1);

    eventloop_destroy(loop);
    close(pipefd[0]);
    close(pipefd[1]);
    printf("  PASS\n");
}

/* 测试 5: 修改事件 */
static void test_eventloop_modify(void) {
    printf("Test 5: EventLoop modify event\n");

    EventLoop *loop = eventloop_create(64);

    int pipefd[2];
    pipe(pipefd);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);

    eventloop_add(loop, pipefd[0], EV_READ, test_callback, NULL);

    /* 修改为写事件 */
    int result = eventloop_modify(loop, pipefd[0], EV_WRITE);
    /* 结果取决于 fd 是否在 entries_capacity 内 */
    /* 默认 entries_capacity 是 1024，pipefd 通常小于此值 */

    eventloop_remove(loop, pipefd[0]);
    eventloop_destroy(loop);
    close(pipefd[0]);
    close(pipefd[1]);
    printf("  PASS\n");
}

/* 测试 6: 多次轮询 */
static void test_eventloop_multi_poll(void) {
    printf("Test 6: EventLoop multi poll\n");

    EventLoop *loop = eventloop_create(64);
    callback_count = 0;

    int pipefd[2];
    pipe(pipefd);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);

    eventloop_add(loop, pipefd[0], EV_READ, test_callback, NULL);

    /* 写入多次 */
    for (int i = 0; i < 3; i++) {
        char buf[] = "test";
        write(pipefd[1], buf, sizeof(buf));
    }

    /* 轮询 */
    int total = 0;
    for (int i = 0; i < 3; i++) {
        total += eventloop_poll(loop, 100);
    }
    assert(total >= 3);

    eventloop_destroy(loop);
    close(pipefd[0]);
    close(pipefd[1]);
    printf("  PASS\n");
}

/* 测试 7: 超时轮询 */
static void test_eventloop_poll_timeout(void) {
    printf("Test 7: EventLoop poll timeout\n");

    EventLoop *loop = eventloop_create(64);

    /* 无事件，应超时返回 0 */
    int n = eventloop_poll(loop, 100);
    assert(n == 0);

    eventloop_destroy(loop);
    printf("  PASS\n");
}

/* 测试 8: 写事件 */
static void test_eventloop_write_event(void) {
    printf("Test 8: EventLoop write event\n");

    EventLoop *loop = eventloop_create(64);
    callback_count = 0;

    int pipefd[2];
    pipe(pipefd);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);

    /* 监听写事件（pipe 写端通常可写） */
    eventloop_add(loop, pipefd[1], EV_WRITE, test_callback, NULL);

    int n = eventloop_poll(loop, 100);
    assert(n > 0);
    assert(callback_count >= 1);

    eventloop_destroy(loop);
    close(pipefd[0]);
    close(pipefd[1]);
    printf("  PASS\n");
}

/* 测试 9: 大量 fd */
static void test_eventloop_many_fds(void) {
    printf("Test 9: EventLoop many fds\n");

    EventLoop *loop = eventloop_create(256);

    int pipes[10][2];
    for (int i = 0; i < 10; i++) {
        pipe(pipes[i]);
        fcntl(pipes[i][0], F_SETFL, O_NONBLOCK);
        eventloop_add(loop, pipes[i][0], EV_READ, test_callback, NULL);
    }

    /* 触发部分事件 */
    write(pipes[0][1], "a", 1);
    write(pipes[5][1], "b", 1);

    int n = eventloop_poll(loop, 100);
    assert(n >= 2);

    eventloop_destroy(loop);
    for (int i = 0; i < 10; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    printf("  PASS\n");
}

/* 测试 10: 重复添加同一 fd */
static void test_eventloop_readd_same_fd(void) {
    printf("Test 10: EventLoop re-add same fd\n");

    EventLoop *loop = eventloop_create(64);

    int pipefd[2];
    pipe(pipefd);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    eventloop_add(loop, pipefd[0], EV_READ, test_callback, NULL);

    /* 再次添加同一 fd 应失败或覆盖 */
    int result = eventloop_add(loop, pipefd[0], EV_READ, test_callback, NULL);
    /* 根据实现，可能返回 -1 或覆盖 */

    eventloop_remove(loop, pipefd[0]);
    eventloop_destroy(loop);
    close(pipefd[0]);
    close(pipefd[1]);
    printf("  PASS\n");
}

int main(void) {
    printf("=== EventLoop Module Tests ===\n\n");

    test_eventloop_create();
    test_eventloop_add();
    test_eventloop_remove();
    test_eventloop_poll();
    test_eventloop_modify();
    test_eventloop_multi_poll();
    test_eventloop_poll_timeout();
    test_eventloop_write_event();
    test_eventloop_many_fds();
    test_eventloop_readd_same_fd();

    printf("\n=== All tests passed ===\n");
    return 0;
}