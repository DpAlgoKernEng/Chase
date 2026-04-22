/**
 * @file    test_range.c
 * @brief   HTTP Range 请求功能测试
 *
 * @details
 *          - 测试有效 Range: bytes=start-end
 *          - 测试开放 Range: bytes=start-
 *          - 测试后缀 Range: bytes=-suffix
 *          - 测试无效 Range (超出文件)
 *          - 测试无 Range 头的正常请求
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
#include "handler.h"
#include "fileserve.h"

#define TEST_PORT_BASE 19080
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
 * 检查响应状态码
 */
static int has_status_code(const char *response, int code) {
    char status_line[32];
    snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d", code);
    return strstr(response, status_line) != NULL;
}

/**
 * 检查响应是否包含 Content-Range 头
 */
static int has_content_range(const char *response) {
    return strstr(response, "Content-Range:") != NULL;
}

/* wrapper: 将 handler_static_file 包装为 RouteHandler 签名 */
static void static_file_wrapper(HttpRequest *req, void *resp_ptr, void *user_data) {
    HttpResponse *resp = (HttpResponse *)resp_ptr;
    handler_static_file(req, resp, user_data);
}

/* 辅助函数：添加 GET 路由（带 user_data） */
static void add_get_route(Router *router, const char *path, RouteHandler handler, void *user_data) {
    Route *route = route_create(ROUTER_MATCH_EXACT, path, handler, user_data);
    route->methods = METHOD_GET;
    router_add_route(router, route);
}

/* ========== 测试用例 ========== */

/**
 * 测试 1: 有效 Range bytes=0-4
 */
TEST(valid_range_start_end) {
    int port = TEST_PORT_BASE + 1;

    pid_t server_pid = fork();
    if (server_pid == 0) {
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
        if (!server) exit(1);

        Router *router = server_get_router(server);

        FileServe *fs = fileserve_create("/tmp");
        if (!fs) {
            server_destroy(server);
            exit(1);
        }

        FILE *f = fopen("/tmp/range_test.txt", "w");
        if (f) {
            fprintf(f, "Hello World Range Test File");
            fclose(f);
        }

        add_get_route(router, "/range_test.txt", static_file_wrapper, fs);

        server_run(server);
        server_destroy(server);
        fileserve_destroy(fs);
        exit(0);
    }

    ASSERT(server_pid > 0, "fork failed");
    sleep(1);

    int client_fd = create_client_socket(port);
    ASSERT(client_fd >= 0, "create_client_socket failed");

    char request[] = "GET /range_test.txt HTTP/1.1\r\nHost: localhost\r\nRange: bytes=0-4\r\n\r\n";
    char response[4096];
    int ret = send_http_request(client_fd, request, response, sizeof(response));
    ASSERT(ret == 0, "send_http_request failed");

    ASSERT(has_status_code(response, 206), "expected 206 Partial Content");
    ASSERT(has_content_range(response), "missing Content-Range header");

    close(client_fd);
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
}

/**
 * 测试 2: 开放 Range bytes=6-
 */
TEST(valid_range_open_end) {
    int port = TEST_PORT_BASE + 2;

    pid_t server_pid = fork();
    if (server_pid == 0) {
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
        if (!server) exit(1);

        Router *router = server_get_router(server);
        FileServe *fs = fileserve_create("/tmp");
        if (!fs) {
            server_destroy(server);
            exit(1);
        }

        FILE *f = fopen("/tmp/range_test.txt", "w");
        if (f) {
            fprintf(f, "Hello World Range Test File");
            fclose(f);
        }

        add_get_route(router, "/range_test.txt", static_file_wrapper, fs);

        server_run(server);
        server_destroy(server);
        fileserve_destroy(fs);
        exit(0);
    }

    ASSERT(server_pid > 0, "fork failed");
    sleep(1);

    int client_fd = create_client_socket(port);
    ASSERT(client_fd >= 0, "create_client_socket failed");

    char request[] = "GET /range_test.txt HTTP/1.1\r\nHost: localhost\r\nRange: bytes=6-\r\n\r\n";
    char response[4096];
    int ret = send_http_request(client_fd, request, response, sizeof(response));
    ASSERT(ret == 0, "send_http_request failed");

    ASSERT(has_status_code(response, 206), "expected 206 Partial Content");
    ASSERT(has_content_range(response), "missing Content-Range header");

    close(client_fd);
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
}

/**
 * 测试 3: 后缀 Range bytes=-5
 */
TEST(valid_range_suffix) {
    int port = TEST_PORT_BASE + 3;

    pid_t server_pid = fork();
    if (server_pid == 0) {
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
        if (!server) exit(1);

        Router *router = server_get_router(server);
        FileServe *fs = fileserve_create("/tmp");
        if (!fs) {
            server_destroy(server);
            exit(1);
        }

        FILE *f = fopen("/tmp/range_test.txt", "w");
        if (f) {
            fprintf(f, "Hello World Range Test File");
            fclose(f);
        }

        add_get_route(router, "/range_test.txt", static_file_wrapper, fs);

        server_run(server);
        server_destroy(server);
        fileserve_destroy(fs);
        exit(0);
    }

    ASSERT(server_pid > 0, "fork failed");
    sleep(1);

    int client_fd = create_client_socket(port);
    ASSERT(client_fd >= 0, "create_client_socket failed");

    char request[] = "GET /range_test.txt HTTP/1.1\r\nHost: localhost\r\nRange: bytes=-5\r\n\r\n";
    char response[4096];
    int ret = send_http_request(client_fd, request, response, sizeof(response));
    ASSERT(ret == 0, "send_http_request failed");

    ASSERT(has_status_code(response, 206), "expected 206 Partial Content");
    ASSERT(has_content_range(response), "missing Content-Range header");

    close(client_fd);
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
}

/**
 * 测试 4: 无效 Range (超出文件大小)
 */
TEST(invalid_range_out_of_bounds) {
    int port = TEST_PORT_BASE + 4;

    pid_t server_pid = fork();
    if (server_pid == 0) {
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
        if (!server) exit(1);

        Router *router = server_get_router(server);
        FileServe *fs = fileserve_create("/tmp");
        if (!fs) {
            server_destroy(server);
            exit(1);
        }

        FILE *f = fopen("/tmp/range_test.txt", "w");
        if (f) {
            fprintf(f, "Hello World Range Test File");
            fclose(f);
        }

        add_get_route(router, "/range_test.txt", static_file_wrapper, fs);

        server_run(server);
        server_destroy(server);
        fileserve_destroy(fs);
        exit(0);
    }

    ASSERT(server_pid > 0, "fork failed");
    sleep(1);

    int client_fd = create_client_socket(port);
    ASSERT(client_fd >= 0, "create_client_socket failed");

    char request[] = "GET /range_test.txt HTTP/1.1\r\nHost: localhost\r\nRange: bytes=99999-100000\r\n\r\n";
    char response[4096];
    int ret = send_http_request(client_fd, request, response, sizeof(response));
    ASSERT(ret == 0, "send_http_request failed");

    ASSERT(has_status_code(response, 416), "expected 416 Range Not Satisfiable");
    ASSERT(has_content_range(response), "missing Content-Range header");

    close(client_fd);
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
}

/**
 * 测试 5: 无 Range 头的正常请求
 */
TEST(no_range_normal_request) {
    int port = TEST_PORT_BASE + 5;

    pid_t server_pid = fork();
    if (server_pid == 0) {
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
        if (!server) exit(1);

        Router *router = server_get_router(server);
        FileServe *fs = fileserve_create("/tmp");
        if (!fs) {
            server_destroy(server);
            exit(1);
        }

        FILE *f = fopen("/tmp/range_test.txt", "w");
        if (f) {
            fprintf(f, "Hello World Range Test File");
            fclose(f);
        }

        add_get_route(router, "/range_test.txt", static_file_wrapper, fs);

        server_run(server);
        server_destroy(server);
        fileserve_destroy(fs);
        exit(0);
    }

    ASSERT(server_pid > 0, "fork failed");
    sleep(1);

    int client_fd = create_client_socket(port);
    ASSERT(client_fd >= 0, "create_client_socket failed");

    char request[] = "GET /range_test.txt HTTP/1.1\r\nHost: localhost\r\n\r\n";
    char response[4096];
    int ret = send_http_request(client_fd, request, response, sizeof(response));
    ASSERT(ret == 0, "send_http_request failed");

    ASSERT(has_status_code(response, 200), "expected 200 OK");
    ASSERT(!has_content_range(response), "should not have Content-Range for normal request");

    close(client_fd);
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
}

/* ========== 主函数 ========== */

int main(void) {
    printf("=== Range 请求功能测试 ===\n\n");

    test_failed = 0;

    printf("Test 1: Valid Range bytes=0-4\n");
    test_valid_range_start_end();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 2: Open Range bytes=6-\n");
    test_failed = 0;
    test_valid_range_open_end();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 3: Suffix Range bytes=-5\n");
    test_failed = 0;
    test_valid_range_suffix();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 4: Invalid Range (out of bounds)\n");
    test_failed = 0;
    test_invalid_range_out_of_bounds();
    if (!test_failed) printf("  PASSED\n");

    printf("Test 5: No Range (normal request)\n");
    test_failed = 0;
    test_no_range_normal_request();
    if (!test_failed) printf("  PASSED\n");

    printf("\n=== 测试完成 ===\n");

    unlink("/tmp/range_test.txt");

    return 0;
}