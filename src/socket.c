/**
 * @file    socket.c
 * @brief   Socket 操作封装实现
 *
 * @details
 *          - 创建服务端监听 socket
 *          - 配置 SO_REUSEADDR、SO_REUSEPORT、TCP_NODELAY
 *          - 跨平台支持
 *
 * @layer   Core Layer
 *
 * @depends 无依赖
 * @usedby  server, worker, connection
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* 平台检测 */
#ifdef __linux__
    #define HAS_SO_REUSEPORT 1
#elif defined(__APPLE__)
    /* macOS 10.11+ 支持 SO_REUSEPORT */
    #define HAS_SO_REUSEPORT 1
#else
    #define HAS_SO_REUSEPORT 0
#endif

/* ========== 公共 API 实现 ========== */

bool socket_has_reuseport(void) {
    return HAS_SO_REUSEPORT;
}

int socket_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl F_GETFL");
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL O_NONBLOCK");
        return -1;
    }

    return 0;
}

int socket_set_reuseaddr(int fd) {
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        return -1;
    }
    return 0;
}

int socket_set_reuseport(int fd) {
#if HAS_SO_REUSEPORT
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEPORT");
        return -1;
    }
    return 0;
#else
    /* 不支持 SO_REUSEPORT，静默忽略 */
    return 0;
#endif
}

int socket_set_tcp_nodelay(int fd) {
    int opt = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        perror("setsockopt TCP_NODELAY");
        return -1;
    }
    return 0;
}

int socket_set_tcp_cork(int fd, bool enable) {
#ifdef TCP_CORK
    int opt = enable ? 1 : 0;
    if (setsockopt(fd, IPPROTO_TCP, TCP_CORK, &opt, sizeof(opt)) < 0) {
        perror("setsockopt TCP_CORK");
        return -1;
    }
    return 0;
#else
    /* macOS/其他平台不支持 TCP_CORK */
    (void)fd;
    (void)enable;
    return 0;
#endif
}

int socket_set_send_buffer(int fd, int size) {
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0) {
        perror("setsockopt SO_SNDBUF");
        return -1;
    }
    return 0;
}

int socket_set_recv_buffer(int fd, int size) {
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0) {
        perror("setsockopt SO_RCVBUF");
        return -1;
    }
    return 0;
}

void socket_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

int socket_create_server(int port, const char *bind_addr, const SocketOptions *options) {
    /* 创建 socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* 设置选项 */
    if (options->nonblock && socket_set_nonblock(fd) < 0) {
        close(fd);
        return -1;
    }

    if (options->reuseaddr && socket_set_reuseaddr(fd) < 0) {
        close(fd);
        return -1;
    }

    if (options->reuseport && socket_set_reuseport(fd) < 0) {
        close(fd);
        return -1;
    }

    if (options->tcp_nodelay && socket_set_tcp_nodelay(fd) < 0) {
        close(fd);
        return -1;
    }

    /* 绑定地址 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (bind_addr == NULL || strcmp(bind_addr, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) <= 0) {
            perror("inet_pton");
            close(fd);
            return -1;
        }
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    /* 监听 */
    if (listen(fd, options->backlog) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

int socket_create_server_default(int port, const char *bind_addr, int backlog) {
    SocketOptions options = SOCKET_OPTIONS_DEFAULT;
    options.backlog = backlog;
    return socket_create_server(port, bind_addr, &options);
}