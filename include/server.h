/**
 * @file    server.h
 * @brief   HTTP 服务器封装层，管理完整服务器生命周期
 *
 * @details
 *          - 封装 EventLoop、Router、监听 Socket
 *          - 处理 Accept、HTTP 解析、路由匹配、响应发送
 *          - 支持 SO_REUSEPORT 多进程架构
 *          - 一行代码启动服务器
 *
 * @layer   Server Layer
 *
 * @depends eventloop, connection, http_parser, router, socket, response, handler
 * @usedby  worker, examples
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#ifndef CHASE_SERVER_H
#define CHASE_SERVER_H

#include <stddef.h>
#include <stdbool.h>
#include "error.h"
#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Server 配置结构
 */
typedef struct ServerConfig {
    int port;                   /* 监听端口 */
    int max_connections;        /* 最大连接数 */
    int backlog;                /* listen backlog */
    const char *bind_addr;      /* 绑定地址（NULL = 0.0.0.0） */
    bool reuseport;             /* 是否启用 SO_REUSEPORT */
    Router *router;             /* 外部传入的 Router */
    size_t read_buf_cap;        /* 读缓冲区容量 */
    size_t write_buf_cap;       /* 写缓冲区容量 */

    /* Phase 3: Keep-Alive 支持 */
    int connection_timeout_ms;  /* 连接空闲超时（毫秒，默认 60000） */
    int keepalive_timeout_ms;   /* Keep-Alive 超时（毫秒，默认 5000） */
    int max_keepalive_requests; /* 单连接最大请求数（默认 100，0=无限制） */
} ServerConfig;

/**
 * Server 结构体（opaque）
 */
typedef struct Server Server;

/**
 * 创建 Server
 * @param config Server 配置
 * @return Server 指针，失败返回 NULL
 */
Server *server_create(const ServerConfig *config);

/**
 * 销毁 Server
 * @param server Server 指针
 */
void server_destroy(Server *server);

/**
 * 运行 Server（阻塞）
 * @param server Server 指针
 * @return 0 正常退出，-1 错误
 */
int server_run(Server *server);

/**
 * 停止 Server
 * @param server Server 指针
 */
void server_stop(Server *server);

/**
 * 获取 Server 的监听 fd
 * @param server Server 指针
 * @return 监听 fd，-1 表示无效
 */
int server_get_fd(Server *server);

/**
 * 获取 Server 的 EventLoop
 * @param server Server 指针
 * @return EventLoop 指针
 */
typedef struct EventLoop EventLoop;
EventLoop *server_get_eventloop(Server *server);

/**
 * 获取 Server 的 Router
 * @param server Server 指针
 * @return Router 指针
 */
Router *server_get_router(Server *server);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_SERVER_H */