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

int main(void) {
    printf("=== EventLoop Module Tests ===\n\n");

    test_eventloop_create();
    test_eventloop_add();
    test_eventloop_remove();
    test_eventloop_poll();

    printf("\n=== All tests passed ===\n");
    return 0;
}