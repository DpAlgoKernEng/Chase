#include "eventloop.h"
#include "http_parser.h"
#include "router.h"
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_PORT 8080
#define BUFFER_SIZE 8192

/* HTTP 响应结构 */
typedef struct {
    int status;
    const char *body;
    size_t body_len;
} HttpResponse;

/* 默认处理器 */
static void default_handler(HttpRequest *req, void *resp_data, void *user_data) {
    (void)req;
    (void)user_data;
    HttpResponse *resp = (HttpResponse *)resp_data;
    resp->status = 200;
    resp->body = "Hello World from Chase HTTP Server!";
    resp->body_len = strlen(resp->body);
}

/* 生成 HTTP 响应 */
static void build_response(char *buf, size_t buf_size, HttpResponse *resp) {
    const char *status_text = http_status_get_description(resp->status);

    snprintf(buf, buf_size,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        resp->status, status_text, resp->body_len, resp->body);
}

/* 连接回调 */
static void on_read(int fd, uint32_t events, void *user_data) {
    (void)events;
    EventLoop *loop = (EventLoop *)user_data;

    /* 直接读取数据 */
    char buffer[BUFFER_SIZE];
    ssize_t n = read(fd, buffer, BUFFER_SIZE - 1);

    if (n <= 0) {
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("read");
        }
        eventloop_remove(loop, fd);
        close(fd);
        return;
    }

    buffer[n] = '\0';

    /* 解析 HTTP 请求 */
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    size_t consumed = 0;
    ParseResult result = http_parser_parse(parser, req, buffer, n, &consumed);

    if (result != PARSE_COMPLETE) {
        /* 返回 400 */
        HttpResponse resp = {400, "Bad Request", 12};

        char response_buf[256];
        build_response(response_buf, sizeof(response_buf), &resp);
        write(fd, response_buf, strlen(response_buf));

        http_request_destroy(req);
        http_parser_destroy(parser);
        eventloop_remove(loop, fd);
        close(fd);
        return;
    }

    /* 路由匹配 */
    Router *router = router_create();
    Route *route = route_create(ROUTER_MATCH_EXACT, "/", default_handler, NULL);
    router_add_route(router, route);

    Route *matched = router_match(router, req->path, req->method);

    HttpResponse resp = {404, "Not Found", 9};

    if (matched) {
        matched->handler(req, &resp, matched->user_data);
    }

    /* 发送响应 */
    char response_buf[2048];
    build_response(response_buf, sizeof(response_buf), &resp);

    write(fd, response_buf, strlen(response_buf));

    /* 清理 */
    router_destroy(router);
    http_request_destroy(req);
    http_parser_destroy(parser);
    eventloop_remove(loop, fd);
    close(fd);
}

/* Accept 回调 */
static void on_accept(int fd, uint32_t events, void *user_data) {
    (void)events;
    EventLoop *loop = (EventLoop *)user_data;

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        perror("accept");
        return;
    }

    /* 设置非阻塞 */
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    printf("Accepted connection from %s:%d\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    /* 添加到事件循环 */
    eventloop_add(loop, client_fd, EV_READ, on_read, loop);
}

/* 创建服务器 socket */
static int create_server_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* 设置非阻塞 */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* 设置 SO_REUSEADDR */
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 绑定地址 */
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    /* 监听 */
    if (listen(fd, 128) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

int main(int argc, char *argv[]) {
    int port = SERVER_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    printf("Chase HTTP Server - Minimal Example\n");
    printf("Starting server on port %d...\n", port);

    /* 创建服务器 socket */
    int server_fd = create_server_socket(port);
    if (server_fd < 0) {
        return 1;
    }

    printf("Server socket created (fd=%d)\n", server_fd);

    /* 创建事件循环 */
    EventLoop *loop = eventloop_create(1024);
    if (!loop) {
        close(server_fd);
        return 1;
    }

    /* 添加监听 socket */
    eventloop_add(loop, server_fd, EV_READ, on_accept, loop);

    printf("Server running. Press Ctrl+C to stop.\n");

    /* 运行事件循环 */
    eventloop_run(loop);

    /* 清理 */
    eventloop_destroy(loop);
    close(server_fd);

    return 0;
}