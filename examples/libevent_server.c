/**
 * @file    libevent_server.c
 * @brief   libevent 对照 HTTP 服务器实现
 *
 * @details
 *          - 使用 libevent API 实现 HTTP 服务器
 *          - 与 Chase minimal_server 功能对等
 *          - 单进程、单线程架构
 *          - 用于 EventLoop 性能对照测试
 *
 * @author  minghui.liu
 * @date    2026-04-24
 */

#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_PORT 8081
#define BUFFER_SIZE 8192

/* 全局事件循环 */
static struct event_base *g_base = NULL;

/* HTTP 响应结构 */
typedef struct {
    int status;
    const char *body;
    size_t body_len;
} HttpResponse;

/* HTTP 状态码描述 */
static const char *get_status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}

/* 默认处理器 - 返回 Hello World */
static void default_handler(void *resp_data) {
    HttpResponse *resp = (HttpResponse *)resp_data;
    resp->status = 200;
    resp->body = "Hello World from libevent HTTP Server!";
    resp->body_len = strlen(resp->body);
}

/* 简单 HTTP 请求解析和路由 */
static void process_http_request(struct bufferevent *bev, const char *data, size_t len) {
    HttpResponse resp = {404, "Not Found", 9};

    /* 简单解析 HTTP 请求行 */
    /* 格式: METHOD PATH VERSION\r\n */
    const char *method_end = strchr(data, ' ');
    if (!method_end) {
        resp.status = 400;
        resp.body = "Bad Request";
        resp.body_len = strlen(resp.body);
    } else {
        /* 提取路径 */
        const char *path_start = method_end + 1;
        const char *path_end = strchr(path_start, ' ');
        if (path_end) {
            size_t path_len = path_end - path_start;

            /* 路由匹配 */
            if (path_len == 1 && path_start[0] == '/') {
                default_handler(&resp);
            }
        }
    }

    /* 构建 HTTP 响应 */
    char response_buf[2048];
    snprintf(response_buf, sizeof(response_buf),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        resp.status, get_status_text(resp.status),
        resp.body_len, resp.body);

    /* 发送响应 */
    bufferevent_write(bev, response_buf, strlen(response_buf));

    /* 启用写入完成后关闭连接 */
    bufferevent_enable(bev, EV_WRITE);
}

/* 读取回调 */
static void on_read(struct bufferevent *bev, void *ctx) {
    (void)ctx;

    struct evbuffer *input = bufferevent_get_input(bev);
    size_t len = evbuffer_get_length(input);

    if (len == 0) {
        return;
    }

    /* 获取数据 */
    char *data = malloc(len + 1);
    if (!data) {
        bufferevent_free(bev);
        return;
    }

    evbuffer_remove(input, data, len);
    data[len] = '\0';

    /* 处理请求 */
    process_http_request(bev, data, len);

    free(data);
}

/* 写入完成回调 - 关闭连接 */
static void on_write(struct bufferevent *bev, void *ctx) {
    (void)ctx;
    bufferevent_free(bev);
}

/* 事件回调 */
static void on_event(struct bufferevent *bev, short events, void *ctx) {
    (void)ctx;

    if (events & BEV_EVENT_ERROR) {
        perror("bufferevent error");
    }

    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        bufferevent_free(bev);
    }
}

/* 接受连接回调 */
static void on_accept(struct evconnlistener *listener, evutil_socket_t fd,
                      struct sockaddr *addr, int socklen, void *user_data) {
    (void)listener;
    (void)user_data;

    struct event_base *base = g_base;
    struct bufferevent *bev;

    /* 获取客户端地址信息 */
    struct sockaddr_in *client_addr = (struct sockaddr_in *)addr;
    printf("Accepted connection from %s:%d\n",
           inet_ntoa(client_addr->sin_addr), ntohs(client_addr->sin_port));

    /* 创建 bufferevent */
    bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
    if (!bev) {
        perror("bufferevent_socket_new");
        close(fd);
        return;
    }

    /* 设置回调 */
    bufferevent_setcb(bev, on_read, on_write, on_event, NULL);

    /* 启用读取 */
    bufferevent_enable(bev, EV_READ);
}

/* 信号处理回调 */
static void on_signal(evutil_socket_t fd, short events, void *user_data) {
    (void)fd;
    (void)events;
    struct event_base *base = (struct event_base *)user_data;

    printf("\nReceived signal, shutting down...\n");
    event_base_loopbreak(base);
}

/* 创建服务器监听 socket */
static struct evconnlistener *create_listener(struct event_base *base, int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    /* 创建监听器 */
    struct evconnlistener *listener = evconnlistener_new_bind(
        base,
        on_accept,
        NULL,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
        1024,  /* backlog */
        (struct sockaddr *)&addr,
        sizeof(addr)
    );

    if (!listener) {
        perror("evconnlistener_new_bind");
        return NULL;
    }

    return listener;
}

int main(int argc, char *argv[]) {
    int port = SERVER_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    printf("libevent HTTP Server - Comparison Example\n");
    printf("Using libevent version: %s\n", event_get_version());
    printf("Starting server on port %d...\n", port);

    /* 创建事件循环 */
    g_base = event_base_new();
    if (!g_base) {
        fprintf(stderr, "Failed to create event base\n");
        return 1;
    }

    printf("Event base created (method: %s)\n", event_base_get_method(g_base));

    /* 创建监听器 */
    struct evconnlistener *listener = create_listener(g_base, port);
    if (!listener) {
        event_base_free(g_base);
        return 1;
    }

    printf("Server socket created and listening\n");

    /* 注册信号处理 */
    struct event *sigint_ev = evsignal_new(g_base, SIGINT, on_signal, g_base);
    struct event *sigterm_ev = evsignal_new(g_base, SIGTERM, on_signal, g_base);

    if (sigint_ev) event_add(sigint_ev, NULL);
    if (sigterm_ev) event_add(sigterm_ev, NULL);

    printf("Server running. Press Ctrl+C to stop.\n");

    /* 运行事件循环 */
    event_base_dispatch(g_base);

    /* 清理 */
    printf("Shutting down...\n");

    if (sigint_ev) event_free(sigint_ev);
    if (sigterm_ev) event_free(sigterm_ev);
    evconnlistener_free(listener);
    event_base_free(g_base);

    printf("Server stopped.\n");

    return 0;
}