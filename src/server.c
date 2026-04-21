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
#include <poll.h>
#include <stdbool.h>
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

    /* 新增：异步响应发送支持 */
    HttpResponse *pending_response;  /* 待发送的响应 */
    ResponseSendStatus send_status;  /* 发送状态 */
    bool response_pending;           /* 是否有待发送响应 */
} ConnCtx;

/* Server 结构体 */
struct Server {
    ServerConfig config;
    EventLoop *loop;
    int server_fd;
    Router *router;
    volatile sig_atomic_t running;
    int signal_pipe[2];         /* 用于信号处理的自管道 */
};

/* 全局 Server 指针（信号处理需要） */
static Server *g_server = NULL;

/* 信号处理启用标志（用于安全关闭） */
static volatile sig_atomic_t g_server_signals_enabled = 0;

/* ========== 连接上下文管理 ========== */

static ConnCtx *connctx_create(Server *server) {
    ConnCtx *ctx = malloc(sizeof(ConnCtx));
    if (!ctx) return NULL;

    ctx->parser = http_parser_create();
    ctx->request = http_request_create();
    ctx->server = server;
    ctx->pending_response = NULL;
    ctx->send_status = RESPONSE_SEND_COMPLETE;
    ctx->response_pending = false;

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

    /* 清理待发送响应 */
    if (ctx->pending_response) {
        response_destroy(ctx->pending_response);
        ctx->pending_response = NULL;
    }

    free(ctx);
}

/* ========== 连接关闭回调 ========== */

static void on_connection_close(int fd, void *user_data) {
    ConnCtx *ctx = (ConnCtx *)user_data;
    if (ctx) {
        connctx_destroy(ctx);
    }
}

/* ========== 信号管道读回调 ========== */

static void on_signal_pipe_read(int fd, uint32_t events, void *user_data) {
    Server *server = (Server *)user_data;
    (void)events;  /* unused */

    char ch;
    while (read(fd, &ch, 1) > 0) {
        /* 收到信号通知，停止服务器 */
        server->running = 0;
        if (server->loop) {
            eventloop_stop(server->loop);
        }
    }
}

/* ========== HTTP 请求处理 ========== */

/**
 * 处理 HTTP 请求并返回响应（不立即发送）
 * @return HttpResponse 指针，需要调用者管理生命周期
 */
static HttpResponse *build_http_response(Server *server, ConnCtx *ctx) {
    HttpResponse *resp = response_create(HTTP_STATUS_OK);
    if (!resp) {
        return NULL;
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

    return resp;
}

/* ========== 写回调（处理异步发送） ========== */

static void on_connection_write(int fd, uint32_t events, void *user_data) {
    ConnCtx *ctx = (ConnCtx *)user_data;
    Server *server = ctx->server;

    if (events & EV_WRITE) {
        if (!ctx->response_pending || !ctx->pending_response) {
            /* 无待发送数据，移除写事件 */
            eventloop_modify(server->loop, fd, EV_READ);
            return;
        }

        /* 继续发送剩余数据 */
        size_t offset, len;
        const char *remaining = response_get_pending(ctx->pending_response, &offset, &len);

        if (remaining && len > 0) {
            ssize_t sent = write(fd, remaining, len);
            if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* 继续等待下次写机会 */
                    return;
                }
                /* 发送错误，关闭连接 */
                eventloop_remove(server->loop, fd);
                close(fd);
                connctx_destroy(ctx);
                return;
            }

            /* 检查是否发送完成 */
            if ((size_t)sent >= len) {
                /* 发送完成 */
                ctx->response_pending = false;
                response_destroy(ctx->pending_response);
                ctx->pending_response = NULL;

                /* 移除写事件，关闭连接（不支持 Keep-Alive） */
                eventloop_remove(server->loop, fd);
                close(fd);
                connctx_destroy(ctx);
            } else {
                /* 部分发送，更新状态并继续等待 */
                /* response_get_pending 返回的是相对于当前发送位置的指针 */
                /* 我们需要通过 response_send_remaining 或内部更新来处理 */
                /* 由于我们已经发送了 sent 字节，需要更新 pending_response 的内部状态 */
                /* 这里通过 response_send_remaining 来完成更新 */
                response_send_remaining(ctx->pending_response, fd, offset + sent, len - sent);
            }
        } else {
            /* 无剩余数据，发送完成 */
            ctx->response_pending = false;
            if (ctx->pending_response) {
                response_destroy(ctx->pending_response);
                ctx->pending_response = NULL;
            }
            eventloop_remove(server->loop, fd);
            close(fd);
            connctx_destroy(ctx);
        }
    }

    if (events & EV_ERROR || events & EV_CLOSE) {
        eventloop_remove(server->loop, fd);
        close(fd);
        connctx_destroy(ctx);
    }
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
            /* 请求解析完成，构建响应 */
            HttpResponse *resp = build_http_response(server, ctx);
            if (!resp) {
                /* 无法创建响应，发送简单错误 */
                const char *error_resp = "HTTP/1.1 500 Internal Error\r\nContent-Length: 0\r\n\r\n";
                write(fd, error_resp, strlen(error_resp));
                eventloop_remove(server->loop, fd);
                close(fd);
                connctx_destroy(ctx);
                return;
            }

            /* 使用扩展发送函数 */
            ResponseSendResult send_result = response_send_ex(resp, fd);

            if (send_result.status == RESPONSE_SEND_COMPLETE) {
                /* 完全发送完成，关闭连接 */
                response_destroy(resp);
                eventloop_remove(server->loop, fd);
                close(fd);
                connctx_destroy(ctx);
            } else if (send_result.status == RESPONSE_SEND_PARTIAL) {
                /* 部分发送，保存响应并注册写事件 */
                ctx->pending_response = resp;
                ctx->response_pending = true;
                ctx->send_status = RESPONSE_SEND_PARTIAL;

                /* 注册写事件回调 */
                eventloop_modify(server->loop, fd, EV_WRITE);
            } else {
                /* 发送错误 */
                response_destroy(resp);
                eventloop_remove(server->loop, fd);
                close(fd);
                connctx_destroy(ctx);
            }
        } else if (result == PARSE_ERROR) {
            /* 解析错误 */
            HttpResponse *resp = response_create(HTTP_STATUS_BAD_REQUEST);
            response_set_body_html(resp, "<h1>400 Bad Request</h1>", 22);

            ResponseSendResult send_result = response_send_ex(resp, fd);

            if (send_result.status == RESPONSE_SEND_COMPLETE) {
                response_destroy(resp);
            } else if (send_result.status == RESPONSE_SEND_PARTIAL) {
                ctx->pending_response = resp;
                ctx->response_pending = true;
                eventloop_modify(server->loop, fd, EV_WRITE);
            } else {
                response_destroy(resp);
            }

            /* 如果发送完成或错误，关闭连接 */
            if (send_result.status != RESPONSE_SEND_PARTIAL) {
                eventloop_remove(server->loop, fd);
                close(fd);
                connctx_destroy(ctx);
            }
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

/* ========== Server 信号处理（仅设置标志，使用self-pipe） ========== */

static void server_signal_handler(int sig) {
    /* 先检查信号处理是否已禁用 */
    if (g_server_signals_enabled == 0) return;
    if (g_server == NULL) return;

    /* 信号处理函数中只做最小操作：写入pipe通知主循环 */
    /* 这符合async-signal-safe要求 */
    char ch = sig;
    /* write是async-signal-safe函数 */
    if (g_server->signal_pipe[1] >= 0) {
        write(g_server->signal_pipe[1], &ch, 1);
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

    /* 启用信号处理 */
    g_server_signals_enabled = 1;

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

    /* 初始化signal_pipe */
    server->signal_pipe[0] = -1;
    server->signal_pipe[1] = -1;
    if (pipe(server->signal_pipe) < 0) {
        perror("pipe");
        free(server);
        return NULL;
    }

    /* 设置管道非阻塞 */
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(server->signal_pipe[i], F_GETFL, 0);
        fcntl(server->signal_pipe[i], F_SETFL, flags | O_NONBLOCK);
    }

    /* 使用传入的 Router 或创建默认 Router */
    if (config->router) {
        server->router = config->router;
    } else {
        server->router = router_create();
        if (!server->router) {
            close(server->signal_pipe[0]);
            close(server->signal_pipe[1]);
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
        close(server->signal_pipe[0]);
        close(server->signal_pipe[1]);
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
        close(server->signal_pipe[0]);
        close(server->signal_pipe[1]);
        if (!config->router) {
            router_destroy(server->router);
        }
        free(server);
        return NULL;
    }

    /* 注册 Accept 回调 */
    eventloop_add(server->loop, server->server_fd, EV_READ, on_accept, server);

    /* 注册信号管道读端回调（用于安全处理信号） */
    eventloop_add(server->loop, server->signal_pipe[0], EV_READ, on_signal_pipe_read, server);

    /* 设置全局指针 */
    g_server = server;

    printf("[Server] Created on port %d (fd=%d, reuseport=%s)\n",
           config->port, server->server_fd,
           config->reuseport ? "true" : "false");

    return server;
}

void server_destroy(Server *server) {
    if (!server) return;

    /* 先禁用信号处理 */
    g_server_signals_enabled = 0;

    /* 阻塞相关信号，防止在销毁过程中收到信号 */
    sigset_t mask, old_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, &old_mask);

    g_server = NULL;

    if (server->loop) {
        eventloop_destroy(server->loop);
    }

    if (server->server_fd >= 0) {
        socket_close(server->server_fd);
    }

    /* 关闭信号管道 */
    if (server->signal_pipe[0] >= 0) close(server->signal_pipe[0]);
    if (server->signal_pipe[1] >= 0) close(server->signal_pipe[1]);

    /* 只销毁自己创建的 Router */
    if (!server->config.router && server->router) {
        router_destroy(server->router);
    }

    free(server);

    /* 恢复信号屏蔽 */
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
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