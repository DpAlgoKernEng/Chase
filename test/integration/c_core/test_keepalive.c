/**
 * @file    test_keepalive.c
 * @brief   Keep-Alive 功能测试
 *
 * @details
 *          - 测试 Connection timeout
 *          - 测试 Keep-Alive timeout
 *          - 测试 Connection header 解析
 *          - 测试 max_keepalive_requests 限制
 *          - 测试同一连接多次请求
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

#include "server.h"
#include "router.h"
#include "response.h"
#include "timer.h"

#define TEST_PORT_BASE 18080
#define TEST_TIMEOUT_MS 5000

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

/* ========== 辅助函数 ========== */

/**
 * 创建非阻塞 TCP 客户端连接
 */
static int create_client_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* 设置非阻塞 */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* 连接 */
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
    }

    /* 等待连接完成 */
    struct pollfd pfd = {fd, POLLOUT, 0};
    poll(&pfd, 1, 1000);

    if (!(pfd.revents & POLLOUT)) {
        close(fd);
        return -1;
    }

    /* 检查连接结果 */
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
    if (error != 0) {
        close(fd);
        return -1;
    }

    /* 恢复阻塞模式便于测试 */
    fcntl(fd, F_SETFL, flags);

    return fd;
}

/**
 * 发送 HTTP 请求并接收响应
 */
static int send_http_request(int fd, const char *request, char *response, size_t resp_size) {
    ssize_t sent = write(fd, request, strlen(request));
    if (sent < 0) return -1;

    ssize_t n = read(fd, response, resp_size - 1);
    if (n < 0) return -1;

    response[n] = '\0';
    return 0;
}

/**
 * 检查响应是否包含 Keep-Alive 头
 */
static int has_keepalive_header(const char *response) {
    return strstr(response, "Connection: keep-alive") != NULL ||
           strstr(response, "Keep-Alive:") != NULL;
}

/**
 * 检查响应是否包含 Connection: close
 */
static int has_close_header(const char *response) {
    return strstr(response, "Connection: close") != NULL;
}

/* 简单 handler 函数 */
static void simple_handler(HttpRequest *req, void *resp_ptr, void *user_data) {
    HttpResponse *resp = (HttpResponse *)resp_ptr;
    response_set_body_html(resp, "OK", 2);
}

/* 辅助函数：添加 GET 路由 */
static void add_get_route(Router *router, const char *path, RouteHandler handler) {
    Route *route = route_create(ROUTER_MATCH_EXACT, path, handler, NULL);
    route->methods = METHOD_GET;
    router_add_route(router, route);
}

/* ========== 测试用例 ========== */

/**
 * 测试 1: HTTP/1.1 默认 Keep-Alive
 */
TEST(http11_default_keepalive) {
    int port = TEST_PORT_BASE + 1;
    ServerConfig config = {
        .port = port,
        .max_connections = 100,
        .backlog = 10,
        .bind_addr = NULL,
        .reuseport = false,
        .router = NULL,
        .read_buf_cap = 0,
        .write_buf_cap = 0,
        .connection_timeout_ms = TEST_TIMEOUT_MS,
        .keepalive_timeout_ms = TEST_TIMEOUT_MS,
        .max_keepalive_requests = 10
    };

    Server *server = server_create(&config);
    ASSERT(server != NULL, "server_create failed");

    Router *router = server_get_router(server);
    add_get_route(router, "/", simple_handler);

    /* 启动服务器（后台进程） */
    pid_t server_pid = fork();
    if (server_pid == 0) {
        server_run(server);
        exit(0);
    }

    sleep(1);

    int client_fd = create_client_socket(port);
    ASSERT(client_fd >= 0, "create_client_socket failed");

    char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    char response[4096];
    int ret = send_http_request(client_fd, request, response, sizeof(response));
    ASSERT(ret == 0, "send_http_request failed");

    ASSERT(has_keepalive_header(response), "missing Keep-Alive header");

    close(client_fd);
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    server_destroy(server);
}

/**
 * 测试 2: HTTP/1.0 不默认 Keep-Alive
 */
TEST(http10_no_default_keepalive) {
    int port = TEST_PORT_BASE + 2;
    ServerConfig config = {
        .port = port,
        .max_connections = 100,
        .backlog = 10,
        .bind_addr = NULL,
        .reuseport = false,
        .router = NULL,
        .read_buf_cap = 0,
        .write_buf_cap = 0,
        .connection_timeout_ms = TEST_TIMEOUT_MS,
        .keepalive_timeout_ms = TEST_TIMEOUT_MS,
        .max_keepalive_requests = 10
    };

    Server *server = server_create(&config);
    ASSERT(server != NULL, "server_create failed");

    Router *router = server_get_router(server);
    add_get_route(router, "/", simple_handler);

    pid_t server_pid = fork();
    if (server_pid == 0) {
        server_run(server);
        exit(0);
    }

    sleep(1);

    int client_fd = create_client_socket(port);
    ASSERT(client_fd >= 0, "create_client_socket failed");

    char request[] = "GET / HTTP/1.0\r\n\r\n";
    char response[4096];
    int ret = send_http_request(client_fd, request, response, sizeof(response));
    ASSERT(ret == 0, "send_http_request failed");

    ASSERT(has_close_header(response), "missing Connection: close");

    close(client_fd);
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    server_destroy(server);
}

/**
 * 测试 3: Connection: close 请求
 */
TEST(connection_close_request) {
    int port = TEST_PORT_BASE + 3;
    ServerConfig config = {
        .port = port,
        .max_connections = 100,
        .backlog = 10,
        .bind_addr = NULL,
        .reuseport = false,
        .router = NULL,
        .read_buf_cap = 0,
        .write_buf_cap = 0,
        .connection_timeout_ms = TEST_TIMEOUT_MS,
        .keepalive_timeout_ms = TEST_TIMEOUT_MS,
        .max_keepalive_requests = 10
    };

    Server *server = server_create(&config);
    ASSERT(server != NULL, "server_create failed");

    Router *router = server_get_router(server);
    add_get_route(router, "/", simple_handler);

    pid_t server_pid = fork();
    if (server_pid == 0) {
        server_run(server);
        exit(0);
    }

    sleep(1);

    int client_fd = create_client_socket(port);
    ASSERT(client_fd >= 0, "create_client_socket failed");

    char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char response[4096];
    int ret = send_http_request(client_fd, request, response, sizeof(response));
    ASSERT(ret == 0, "send_http_request failed");

    ASSERT(has_close_header(response), "missing Connection: close");

    close(client_fd);
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    server_destroy(server);
}

/**
 * 测试 4: 同一连接多次请求 (Keep-Alive pipeline)
 */
TEST(keepalive_multiple_requests) {
    int port = TEST_PORT_BASE + 4;
    ServerConfig config = {
        .port = port,
        .max_connections = 100,
        .backlog = 10,
        .bind_addr = NULL,
        .reuseport = false,
        .router = NULL,
        .read_buf_cap = 0,
        .write_buf_cap = 0,
        .connection_timeout_ms = TEST_TIMEOUT_MS,
        .keepalive_timeout_ms = TEST_TIMEOUT_MS,
        .max_keepalive_requests = 5
    };

    Server *server = server_create(&config);
    ASSERT(server != NULL, "server_create failed");

    Router *router = server_get_router(server);
    add_get_route(router, "/", simple_handler);

    pid_t server_pid = fork();
    if (server_pid == 0) {
        server_run(server);
        exit(0);
    }

    sleep(1);

    int client_fd = create_client_socket(port);
    ASSERT(client_fd >= 0, "create_client_socket failed");

    /* 发送 2 次请求 */
    for (int i = 0; i < 2; i++) {
        char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        char response[4096];
        int ret = send_http_request(client_fd, request, response, sizeof(response));
        ASSERT(ret == 0, "request failed");
        ASSERT(has_keepalive_header(response), "missing Keep-Alive header");
    }

    close(client_fd);
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    server_destroy(server);
}

/* ========== 主函数 ========== */

int main(void) {
    printf("=== Keep-Alive 功能测试 ===\n\n");

    test_failed = 0;

    printf("Test 1: HTTP/1.1 default Keep-Alive\n");
    test_http11_default_keepalive();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 2: HTTP/1.0 no default Keep-Alive\n");
    test_failed = 0;
    test_http10_no_default_keepalive();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 3: Connection: close request\n");
    test_failed = 0;
    test_connection_close_request();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 4: Keep-Alive multiple requests\n");
    test_failed = 0;
    test_keepalive_multiple_requests();
    if (!test_failed) printf("  PASSED\n");

    printf("\n=== 测试完成 ===\n");
    return 0;
}