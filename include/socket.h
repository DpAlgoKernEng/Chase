/**
 * @file    socket.h
 * @brief   Socket 操作封装，提供统一的 socket 创建和配置接口
 *
 * @details
 *          - 创建服务端监听 socket
 *          - 配置 SO_REUSEADDR、SO_REUSEPORT、TCP_NODELAY
 *          - 设置非阻塞模式
 *          - 跨平台支持（Linux/macOS）
 *
 * @layer   Core Layer
 *
 * @depends 无依赖
 * @usedby  server, worker, connection
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#ifndef CHASE_SOCKET_H
#define CHASE_SOCKET_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Socket 选项配置 */
typedef struct SocketOptions {
    bool nonblock;          /* 是否设置非阻塞 */
    bool reuseaddr;         /* 是否设置 SO_REUSEADDR */
    bool reuseport;         /* 是否设置 SO_REUSEPORT */
    bool tcp_nodelay;       /* 是否设置 TCP_NODELAY */
    int backlog;            /* listen backlog */
} SocketOptions;

/* 默认选项配置 */
#define SOCKET_OPTIONS_DEFAULT { \
    .nonblock = true, \
    .reuseaddr = true, \
    .reuseport = true, \
    .tcp_nodelay = true, \
    .backlog = 128 \
}

/**
 * 创建服务端监听 socket
 * @param port 监听端口
 * @param bind_addr 绑定地址（NULL = 0.0.0.0）
 * @param options Socket 选项配置
 * @return socket fd，失败返回 -1
 */
int socket_create_server(int port, const char *bind_addr, const SocketOptions *options);

/**
 * 创建服务端监听 socket（使用默认选项）
 * @param port 监听端口
 * @param bind_addr 绑定地址（NULL = 0.0.0.0）
 * @param backlog listen backlog
 * @return socket fd，失败返回 -1
 */
int socket_create_server_default(int port, const char *bind_addr, int backlog);

/**
 * 设置 socket 非阻塞
 * @param fd socket 文件描述符
 * @return 0 成功，-1 失败
 */
int socket_set_nonblock(int fd);

/**
 * 设置 SO_REUSEADDR
 * @param fd socket 文件描述符
 * @return 0 成功，-1 失败
 */
int socket_set_reuseaddr(int fd);

/**
 * 设置 SO_REUSEPORT
 * @param fd socket 文件描述符
 * @return 0 成功，-1 失败
 */
int socket_set_reuseport(int fd);

/**
 * 设置 TCP_NODELAY
 * @param fd socket 文件描述符
 * @return 0 成功，-1 失败
 */
int socket_set_tcp_nodelay(int fd);

/**
 * 设置 TCP_CORK（Linux only）
 * @param fd socket 文件描述符
 * @param enable 是否启用
 * @return 0 成功，-1 失败
 */
int socket_set_tcp_cork(int fd, bool enable);

/**
 * 设置发送缓冲区大小
 * @param fd socket 文件描述符
 * @param size 缓冲区大小
 * @return 0 成功，-1 失败
 */
int socket_set_send_buffer(int fd, int size);

/**
 * 设置接收缓冲区大小
 * @param fd socket 文件描述符
 * @param size 缓冲区大小
 * @return 0 成功，-1 失败
 */
int socket_set_recv_buffer(int fd, int size);

/**
 * 关闭 socket
 * @param fd socket 文件描述符
 */
void socket_close(int fd);

/**
 * 检查 SO_REUSEPORT 是否支持
 * @return true 支持，false 不支持
 */
bool socket_has_reuseport(void);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_SOCKET_H */