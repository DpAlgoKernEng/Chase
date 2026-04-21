/**
 * @file    connection.c
 * @brief   TCP 连接管理实现
 *
 * @details
 *          - 使用 Buffer 管理读写缓冲区
 *          - 使用回调模式处理关闭事件
 *          - 支持非阻塞 I/O
 *          - 池管理字段已移除，由 ConnectionPool 内部管理
 *
 * @layer   Core Layer
 *
 * @depends buffer
 * @usedby  server, connection_pool, examples
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "connection.h"
#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

/* 默认缓冲区配置 */
#define DEFAULT_READ_BUF_CAP   BUFFER_DEFAULT_READ_CAP
#define DEFAULT_WRITE_BUF_CAP  BUFFER_DEFAULT_WRITE_CAP

/* Connection 结构体 */
struct Connection {
    int fd;
    Buffer *read_buf;
    Buffer *write_buf;
    ConnState state;

    /* 关闭回调 */
    ConnectionCloseCallback on_close;
    void *close_user_data;

    /* 用户数据（用于存储连接上下文） */
    void *user_data;
};

/* ========== Connection 实现 ========== */

Connection *connection_create(int fd,
                              ConnectionCloseCallback on_close,
                              void *close_user_data) {
    return connection_create_ex(fd, on_close, close_user_data,
                                 DEFAULT_READ_BUF_CAP,
                                 DEFAULT_WRITE_BUF_CAP,
                                 BUFFER_MODE_FIXED);
}

Connection *connection_create_ex(int fd,
                                  ConnectionCloseCallback on_close,
                                  void *close_user_data,
                                  size_t read_buf_cap,
                                  size_t write_buf_cap,
                                  BufferMode mode) {
    Connection *conn = malloc(sizeof(Connection));
    if (!conn) return NULL;

    conn->fd = fd;
    conn->on_close = on_close;
    conn->close_user_data = close_user_data;
    conn->state = CONN_STATE_CONNECTING;
    conn->user_data = NULL;

    conn->read_buf = buffer_create_ex(read_buf_cap, mode, BUFFER_DEFAULT_MAX_CAP);
    if (!conn->read_buf) {
        free(conn);
        return NULL;
    }

    conn->write_buf = buffer_create_ex(write_buf_cap, mode, BUFFER_DEFAULT_MAX_CAP);
    if (!conn->write_buf) {
        buffer_destroy(conn->read_buf);
        free(conn);
        return NULL;
    }

    return conn;
}

void connection_destroy(Connection *conn) {
    if (!conn) return;

    if (conn->fd >= 0) {
        close(conn->fd);
    }

    buffer_destroy(conn->read_buf);
    buffer_destroy(conn->write_buf);
    free(conn);
}

int connection_read(Connection *conn) {
    if (!conn || conn->fd < 0) return -1;

    char temp[4096];
    ssize_t n = read(conn->fd, temp, sizeof(temp));

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* 无数据 */
        }
        return -1;  /* 错误 */
    }

    if (n == 0) {
        connection_close(conn);
        return 0;  /* 连接关闭 */
    }

    if (buffer_write(conn->read_buf, temp, n) < 0) {
        return -1;  /* 缓冲区满 */
    }

    return (int)n;
}

int connection_write(Connection *conn) {
    if (!conn || conn->fd < 0) return -1;

    size_t avail = buffer_available(conn->write_buf);
    if (avail == 0) return 0;

    /* 使用 peek 获取连续数据指针，避免复制 */
    size_t peek_len = 0;
    const char *peek_data = buffer_peek(conn->write_buf, &peek_len);
    if (!peek_data || peek_len == 0) return 0;

    /* 只尝试写入 peek_len（连续部分） */
    ssize_t written = write(conn->fd, peek_data, peek_len);
    if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  /* 等待下次写机会 */
        }
        return -1;  /* 连接错误 */
    }

    /* 成功写入，跳过已发送的数据 */
    buffer_skip(conn->write_buf, written);

    return (int)written;
}

void connection_close(Connection *conn) {
    if (!conn) return;

    connection_set_state(conn, CONN_STATE_CLOSING);

    /* 调用关闭回调（如果设置） */
    if (conn->on_close) {
        conn->on_close(conn->fd, conn->close_user_data);
    }

    /* 关闭 fd */
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }

    connection_set_state(conn, CONN_STATE_CLOSED);
}

void connection_set_state(Connection *conn, ConnState state) {
    if (!conn) return;
    conn->state = state;
}

ConnState connection_get_state(Connection *conn) {
    if (!conn) return CONN_STATE_CLOSED;
    return conn->state;
}

int connection_get_fd(Connection *conn) {
    if (!conn) return -1;
    return conn->fd;
}

Buffer *connection_get_read_buffer(Connection *conn) {
    if (!conn) return NULL;
    return conn->read_buf;
}

Buffer *connection_get_write_buffer(Connection *conn) {
    if (!conn) return NULL;
    return conn->write_buf;
}

void *connection_get_user_data(Connection *conn) {
    if (!conn) return NULL;
    return conn->user_data;
}

void connection_set_user_data(Connection *conn, void *user_data) {
    if (!conn) return;
    conn->user_data = user_data;
}

/* ========== Connection Pool 重用 API ========== */

void connection_reset(Connection *conn) {
    if (!conn) return;

    /* 重置缓冲区状态（不释放内存，只重置索引） */
    buffer_clear(conn->read_buf);
    buffer_clear(conn->write_buf);

    /* 重置状态 */
    conn->state = CONN_STATE_CONNECTING;

    /* 保留 user_data 和回调，由调用者管理 */
}

int connection_init_from_pool(Connection *conn, int fd,
                               ConnectionCloseCallback on_close,
                               void *close_user_data) {
    if (!conn || fd < 0) return -1;

    /* 重置连接状态 */
    connection_reset(conn);

    /* 设置新的 fd 和回调 */
    conn->fd = fd;
    conn->on_close = on_close;
    conn->close_user_data = close_user_data;

    /* 设置非阻塞 */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    conn->state = CONN_STATE_READING;

    return 0;
}

void connection_dissociate_fd(Connection *conn) {
    if (!conn) return;

    /* 关闭 fd */
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }

    /* 清除回调（避免重复调用） */
    conn->on_close = NULL;
    conn->close_user_data = NULL;

    /* 重置状态 */
    conn->state = CONN_STATE_CLOSED;
}