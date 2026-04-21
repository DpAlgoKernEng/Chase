/**
 * @file    server.c
 * @brief   HTTP 服务器封装层实现
 *
 * @details
 *          - 内部管理 EventLoop、Router、监听 Socket
 *          - 使用 ConnCtx 存储每个连接的 HTTP 解析状态
 *          - Accept → Read → Parse → Match → Handler → Send 流程
 *          - 支持 SO_REUSEPORT 多 Worker 进程架构
 *
 * @layer   Server Layer
 *
 * @depends eventloop, connection, http_parser, router, socket, response, handler, buffer
 * @usedby  worker, examples
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "server.h"
#include "eventloop.h"
#include "connection.h"
#include "http_parser.h"
#include "router.h"
#include "socket.h"
#include "response.h"
#include "handler.h"
#include "buffer.h"
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* 默认配置 */
#define DEFAULT_MAX_CONNECTIONS  1024
#define DEFAULT_BACKLOG          1024
#define DEFAULT_READ_BUF_CAP     BUFFER_DEFAULT_READ_CAP
#define DEFAULT_WRITE_BUF_CAP    BUFFER_DEFAULT_WRITE_CAP

/* 连接上下文（每个 HTTP 连接的解析状态） */
typedef struct ConnCtx {
    HttpParser *parser;
    HttpRequest *request;
    Server *server;
} ConnCtx;

/* Server 结构体 */
struct Server {
    ServerConfig config;
    EventLoop *loop;
    int server_fd;
    Router *router;
    volatile sig_atomic_t running;
};

/* 全局 Server 指针（信号处理需要） */
static Server *g_server = NULL;

/* ========== 连接上下文管理 ========== */

static ConnCtx *connctx_create(Server *server) {
    ConnCtx *ctx = malloc(sizeof(ConnCtx));
    if (!ctx) return NULL;

    ctx->parser = http_parser_create();
    ctx->request = http_request_create();
    ctx->server = server;

    if (!ctx->parser || !ctx->request) {
        http_parser_destroy(ctx->parser);
        http_request_destroy(ctx->request);
        free(ctx);
        return NULL;
    }

    return ctx;
}

static void connctx_destroy(ConnCtx *ctx) {
    if (!ctx) return;

    http_parser_destroy(ctx->parser);
    http_request_destroy(ctx->request);
    free(ctx);
}

/* ========== 连接关闭回调 ========== */

static void on_connection_close(int fd, void *user_data) {
    ConnCtx *ctx = (ConnCtx *)user_data;
    if (ctx) {
        connctx_destroy(ctx);
    }
}

/* ========== HTTP 请求处理 ========== */

static void handle_http_request(Server *server, ConnCtx *ctx, int client_fd) {
    HttpResponse *resp = response_create(HTTP_STATUS_OK);
    if (!resp) {
        /* 无法创建响应，发送简单错误 */
        const char *error_resp = "HTTP/1.1 500 Internal Error\r\nContent-Length: 0\r\n\r\n";
        write(client_fd, error_resp, strlen(error_resp));
        return;
    }

    /* 路由匹配 */
    Route *matched = router_match(server->router, ctx->request->path, ctx->request->method);

    if (matched && matched->handler) {
        /* 调用 Handler */
        matched->handler(ctx->request, resp, matched->user_data);
    } else {
        /* 404 */
        response_set_status(resp, HTTP_STATUS_NOT_FOUND);
        response_set_body_html(resp, "<h1>404 Not Found</h1>", 22);
    }

    /* 发送响应 */
    response_send(resp, client_fd);

    response_destroy(resp);
}

/* ========== 读回调 ========== */

static void on_connection_read(int fd, uint32_t events, void *user_data) {
    ConnCtx *ctx = (ConnCtx *)user_data;
    Server *server = ctx->server;

    if (events & EV_READ) {
        /* 读取数据 */
        char buffer[8192];
        ssize_t n = read(fd, buffer, sizeof(buffer) - 1);

        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("read");
            }
            /* 连接关闭 */
            eventloop_remove(server->loop, fd);
            close(fd);
            connctx_destroy(ctx);
            return;
        }

        buffer[n] = '\0';

        /* 解析 HTTP 请求 */
        size_t consumed = 0;
        ParseResult result = http_parser_parse(ctx->parser, ctx->request, buffer, n, &consumed);

        if (result == PARSE_COMPLETE) {
            /* 请求解析完成，处理请求 */
            handle_http_request(server, ctx, fd);

            /* 关闭连接（简单服务器，不支持 Keep-Alive） */
            eventloop_remove(server->loop, fd);
            close(fd);
            connctx_destroy(ctx);
        } else if (result == PARSE_ERROR) {
            /* 解析错误 */
            HttpResponse *resp = response_create(HTTP_STATUS_BAD_REQUEST);
            response_set_body_html(resp, "<h1>400 Bad Request</h1>", 22);
            response_send(resp, fd);
            response_destroy(resp);

            eventloop_remove(server->loop, fd);
            close(fd);
            connctx_destroy(ctx);
        }
        /* PARSE_NEED_MORE: 继续等待更多数据 */
    }

    if (events & EV_ERROR || events & EV_CLOSE) {
        eventloop_remove(server->loop, fd);
        close(fd);
        connctx_destroy(ctx);
    }
}

/* ========== Accept 回调 ========== */

static void on_accept(int fd, uint32_t events, void *user_data) {
    Server *server = (Server *)user_data;
    (void)events;  /* unused */

    while (server->running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  /* 无更多连接 */
            }
            perror("accept");
            break;
        }

        /* 设置非阻塞 */
        socket_set_nonblock(client_fd);

        /* 创建连接上下文 */
        ConnCtx *ctx = connctx_create(server);
        if (!ctx) {
            close(client_fd);
            continue;
        }

        /* 注册读事件回调 */
        eventloop_add(server->loop, client_fd, EV_READ, on_connection_read, ctx);
    }
}

/* ========== Server 信号处理 ========== */

static void server_signal_handler(int sig) {
    if (g_server == NULL) return;

    switch (sig) {
        case SIGINT:
        case SIGTERM:
            g_server->running = 0;
            if (g_server->loop) {
                eventloop_stop(g_server->loop);
            }
            break;
        default:
            break;
    }
}

static int install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = server_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGINT, &sa, NULL) < 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0) return -1;

    /* 忽略 SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    return 0;
}

/* ========== API 实现 ========== */

Server *server_create(const ServerConfig *config) {
    if (!config) return NULL;

    Server *server = malloc(sizeof(Server));
    if (!server) return NULL;

    /* 复制配置 */
    memcpy(&server->config, config, sizeof(ServerConfig));
    server->running = 1;

    /* 使用传入的 Router 或创建默认 Router */
    if (config->router) {
        server->router = config->router;
    } else {
        server->router = router_create();
        if (!server->router) {
            free(server);
            return NULL;
        }
    }

    /* 创建监听 socket */
    SocketOptions sock_opts = {
        .nonblock = true,
        .reuseaddr = true,
        .reuseport = config->reuseport,
        .tcp_nodelay = true,
        .backlog = config->backlog > 0 ? config->backlog : DEFAULT_BACKLOG
    };
    server->server_fd = socket_create_server(config->port, config->bind_addr, &sock_opts);

    if (server->server_fd < 0) {
        if (!config->router) {
            router_destroy(server->router);
        }
        free(server);
        return NULL;
    }

    /* 创建 EventLoop */
    int max_conn = config->max_connections > 0 ? config->max_connections : DEFAULT_MAX_CONNECTIONS;
    server->loop = eventloop_create(max_conn);
    if (!server->loop) {
        socket_close(server->server_fd);
        if (!config->router) {
            router_destroy(server->router);
        }
        free(server);
        return NULL;
    }

    /* 注册 Accept 回调 */
    eventloop_add(server->loop, server->server_fd, EV_READ, on_accept, server);

    /* 设置全局指针 */
    g_server = server;

    printf("[Server] Created on port %d (fd=%d, reuseport=%s)\n",
           config->port, server->server_fd,
           config->reuseport ? "true" : "false");

    return server;
}

void server_destroy(Server *server) {
    if (!server) return;

    g_server = NULL;

    if (server->loop) {
        eventloop_destroy(server->loop);
    }

    if (server->server_fd >= 0) {
        socket_close(server->server_fd);
    }

    /* 只销毁自己创建的 Router */
    if (!server->config.router && server->router) {
        router_destroy(server->router);
    }

    free(server);
}

int server_run(Server *server) {
    if (!server) return -1;

    /* 安装信号处理器 */
    if (install_signal_handlers() < 0) {
        fprintf(stderr, "[Server] Failed to install signal handlers\n");
        return -1;
    }

    printf("[Server] Running event loop\n");

    /* 运行事件循环 */
    eventloop_run(server->loop);

    printf("[Server] Event loop stopped\n");

    return 0;
}

void server_stop(Server *server) {
    if (!server) return;

    server->running = 0;

    if (server->loop) {
        eventloop_stop(server->loop);
    }
}

int server_get_fd(Server *server) {
    if (!server) return -1;
    return server->server_fd;
}

EventLoop *server_get_eventloop(Server *server) {
    if (!server) return NULL;
    return server->loop;
}

Router *server_get_router(Server *server) {
    if (!server) return NULL;
    return server->router;
}