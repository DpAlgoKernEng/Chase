/*
 * Chase HTTP Server - 多线程示例
 * 使用 ThreadPoolManager 和 ConnectionPool
 */

#include "eventloop.h"
#include "http_parser.h"
#include "router.h"
#include "error.h"
#include "thread_pool_manager.h"
#include "connection_pool.h"
#include "connection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_PORT 8080
#define WORKER_COUNT 4
#define POOL_CAPACITY 100
#define MAX_EVENTS 1024
#define BUFFER_SIZE 8192

/* 全局服务器状态 */
static volatile bool g_running = true;
static EventLoop *g_main_loop = NULL;  /* 用于信号处理中停止 */

/* 连接上下文 */
typedef struct {
    WorkerThread *worker;
    HttpParser *parser;
    HttpRequest *request;
    Router *router;
    Connection *conn;
} ConnContext;

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
    resp->body = "Hello from Chase Multi-threaded Server!";
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

/* 连接关闭并释放 */
static void connection_close_and_release(ConnContext *ctx, int client_fd) {
    WorkerThread *worker = NULL;
    EventLoop *loop = NULL;

    /* 先保存 worker 和 loop，避免 free 后访问 */
    if (ctx) {
        worker = ctx->worker;
        if (worker) {
            loop = worker_thread_get_eventloop(worker);
        }
    }

    /* 从 EventLoop 移除 */
    if (loop && client_fd >= 0) {
        eventloop_remove(loop, client_fd);
    }

    /* 关闭 fd */
    if (client_fd >= 0) {
        close(client_fd);
    }

    /* 减少连接计数 */
    if (worker) {
        worker_thread_decrement_connections(worker);
    }

    /* 释放上下文 */
    if (ctx) {
        if (ctx->parser) http_parser_destroy(ctx->parser);
        if (ctx->request) http_request_destroy(ctx->request);
        free(ctx);
    }
}

/* 连接读回调 */
static void on_connection_read(int fd, uint32_t events, void *user_data) {
    ConnContext *ctx = (ConnContext *)user_data;

    if (events & EV_READ) {
        /* 读取数据 */
        char buffer[BUFFER_SIZE];
        ssize_t n = read(fd, buffer, BUFFER_SIZE - 1);

        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("read");
            }
            connection_close_and_release(ctx, fd);
            return;
        }

        buffer[n] = '\0';

        /* 解析 HTTP 请求 */
        size_t consumed = 0;
        ParseResult result = http_parser_parse(ctx->parser, ctx->request, buffer, n, &consumed);

        if (result != PARSE_COMPLETE) {
            /* 返回 400 */
            HttpResponse resp = {400, "Bad Request", 12};
            char response_buf[256];
            build_response(response_buf, sizeof(response_buf), &resp);
            write(fd, response_buf, strlen(response_buf));
            connection_close_and_release(ctx, fd);
            return;
        }

        /* 路由匹配 */
        HttpResponse resp = {404, "Not Found", 9};

        Route *matched = router_match(ctx->router, ctx->request->path, ctx->request->method);
        if (matched) {
            matched->handler(ctx->request, &resp, matched->user_data);
        }

        /* 发送响应 */
        char response_buf[2048];
        build_response(response_buf, sizeof(response_buf), &resp);
        write(fd, response_buf, strlen(response_buf));

        /* 关闭连接 */
        connection_close_and_release(ctx, fd);
    }
}

/* Worker 连接处理回调 */
static void worker_connection_handler(int client_fd, EventLoop *loop, void *user_data) {
    WorkerThread *worker = (WorkerThread *)user_data;

    /* 创建连接上下文 */
    ConnContext *ctx = malloc(sizeof(ConnContext));
    if (!ctx) {
        close(client_fd);
        worker_thread_decrement_connections(worker);
        return;
    }

    ctx->worker = worker;
    ctx->parser = http_parser_create();
    ctx->request = http_request_create();
    ctx->router = worker_thread_get_router(worker);
    ctx->conn = NULL;  /* 当前版本不使用 ConnectionPool 的 Connection */

    if (!ctx->parser || !ctx->request) {
        connection_close_and_release(ctx, client_fd);
        return;
    }

    /* 注册读事件回调 */
    eventloop_add(loop, client_fd, EV_READ, on_connection_read, ctx);
}

/* 创建共享 Router */
static Router *create_shared_router(void) {
    Router *router = router_create();
    if (!router) return NULL;

    /* 添加默认路由 */
    Route *route = route_create(ROUTER_MATCH_EXACT, "/", default_handler, NULL);
    router_add_route(router, route);

    return router;
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
    if (listen(fd, 1024) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

/* 主线程 Accept 回调 */
static void on_accept(int fd, uint32_t events, void *user_data) {
    (void)events;
    ThreadPoolManager *manager = (ThreadPoolManager *)user_data;

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

    /* 分发到 Worker */
    if (thread_pool_manager_dispatch(manager, client_fd) < 0) {
        close(client_fd);
    }
}

/* 信号处理 */
static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\nReceived signal %d, shutting down...\n", sig);
        g_running = false;
        if (g_main_loop) {
            eventloop_stop(g_main_loop);
        }
    }
}

int main(int argc, char *argv[]) {
    int port = SERVER_PORT;
    int worker_count = WORKER_COUNT;

    if (argc > 1) {
        port = atoi(argv[1]);
    }
    if (argc > 2) {
        worker_count = atoi(argv[2]);
    }

    printf("Chase HTTP Server - Multi-threaded Example\n");
    printf("Port: %d, Workers: %d, Pool capacity: %d\n", port, worker_count, POOL_CAPACITY);

    /* 设置信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 创建共享 Router */
    Router *router = create_shared_router();
    if (!router) {
        fprintf(stderr, "Failed to create router\n");
        return 1;
    }

    /* 创建 ThreadPoolManager */
    ThreadPoolManager *manager = thread_pool_manager_create(worker_count, MAX_EVENTS);
    if (!manager) {
        fprintf(stderr, "Failed to create thread pool manager\n");
        router_destroy(router);
        return 1;
    }

    /* 配置每个 Worker */
    for (int i = 0; i < worker_count; i++) {
        WorkerThread *worker = thread_pool_manager_get_worker(manager, i);
        worker_thread_set_router(worker, router);
        worker_thread_set_connection_handler(worker, worker_connection_handler, worker);
    }

    /* 启动 Worker 线程 */
    printf("Starting worker threads...\n");
    if (thread_pool_manager_start(manager) < 0) {
        fprintf(stderr, "Failed to start worker threads\n");
        thread_pool_manager_destroy(manager);
        router_destroy(router);
        return 1;
    }

    /* 创建主线程 EventLoop（用于 accept） */
    EventLoop *main_loop = eventloop_create(1024);
    if (!main_loop) {
        fprintf(stderr, "Failed to create main event loop\n");
        thread_pool_manager_stop(manager);
        thread_pool_manager_destroy(manager);
        router_destroy(router);
        return 1;
    }
    g_main_loop = main_loop;  /* 保存给信号处理器使用 */

    /* 创建服务器 socket */
    int server_fd = create_server_socket(port);
    if (server_fd < 0) {
        fprintf(stderr, "Failed to create server socket\n");
        eventloop_destroy(main_loop);
        thread_pool_manager_stop(manager);
        thread_pool_manager_destroy(manager);
        router_destroy(router);
        return 1;
    }

    printf("Server socket created (fd=%d)\n", server_fd);

    /* 注册 accept 事件 */
    eventloop_add(main_loop, server_fd, EV_READ, on_accept, manager);

    printf("Server running on port %d. Press Ctrl+C to stop.\n", port);

    /* 主线程事件循环（阻塞式） */
    eventloop_run(main_loop);

    /* 清理 */
    printf("Shutting down...\n");

    eventloop_remove(main_loop, server_fd);
    close(server_fd);
    eventloop_destroy(main_loop);

    thread_pool_manager_stop(manager);
    thread_pool_manager_destroy(manager);

    router_destroy(router);

    printf("Server stopped.\n");

    return 0;
}