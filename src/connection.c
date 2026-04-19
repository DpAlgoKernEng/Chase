#include "connection.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* 默认缓冲区配置 */
#define DEFAULT_READ_BUF_CAP   (16 * 1024)   /* 16KB */
#define DEFAULT_WRITE_BUF_CAP  (64 * 1024)   /* 64KB */
#define DEFAULT_MAX_CAPACITY   (1024 * 1024) /* 1MB */

/* Buffer 结构体 */
struct Buffer {
    char *data;
    size_t capacity;
    size_t size;
    size_t head;
    size_t tail;
    BufferMode mode;
    size_t max_capacity;
};

/* Connection 结构体 */
struct Connection {
    int fd;
    EventLoop *loop;
    Buffer *read_buf;
    Buffer *write_buf;
    ConnState state;
};

/* ========== Buffer 实现 ========== */

Buffer *buffer_create(size_t capacity) {
    return buffer_create_ex(capacity, BUFFER_MODE_FIXED, DEFAULT_MAX_CAPACITY);
}

Buffer *buffer_create_ex(size_t capacity, BufferMode mode, size_t max_cap) {
    Buffer *buf = malloc(sizeof(Buffer));
    if (!buf) return NULL;

    buf->data = malloc(capacity);
    if (!buf->data) {
        free(buf);
        return NULL;
    }

    buf->capacity = capacity;
    buf->size = 0;
    buf->head = 0;
    buf->tail = 0;
    buf->mode = mode;
    buf->max_capacity = max_cap;

    return buf;
}

void buffer_destroy(Buffer *buf) {
    if (!buf) return;
    free(buf->data);
    free(buf);
}

int buffer_write(Buffer *buf, const char *data, size_t len) {
    if (!buf || !data) return -1;

    /* 检查容量 */
    if (buf->size + len > buf->capacity) {
        if (buf->mode == BUFFER_MODE_FIXED) {
            return -1;  /* 固定模式，溢出 */
        }
        /* AUTO 模式扩容 */
        size_t new_cap = buf->capacity * 2;
        while (new_cap < buf->size + len) {
            new_cap *= 2;
        }
        if (new_cap > buf->max_capacity) {
            new_cap = buf->max_capacity;
        }
        if (buf->size + len > new_cap) {
            return -1;  /* 达到最大容量 */
        }

        /* 线性化并扩容 */
        char *new_data = malloc(new_cap);
        if (!new_data) return -1;

        if (buf->size > 0) {
            if (buf->tail > buf->head) {
                memcpy(new_data, buf->data + buf->head, buf->size);
            } else {
                size_t first_part = buf->capacity - buf->head;
                memcpy(new_data, buf->data + buf->head, first_part);
                memcpy(new_data + first_part, buf->data, buf->tail);
            }
        }

        free(buf->data);
        buf->data = new_data;
        buf->capacity = new_cap;
        buf->head = 0;
        buf->tail = buf->size;
    }

    /* 写入数据 */
    size_t first_write = len;
    if (buf->tail + len > buf->capacity) {
        first_write = buf->capacity - buf->tail;
    }

    memcpy(buf->data + buf->tail, data, first_write);

    if (first_write < len) {
        memcpy(buf->data, data + first_write, len - first_write);
        buf->tail = len - first_write;
    } else {
        buf->tail = (buf->tail + first_write) % buf->capacity;
    }

    buf->size += len;
    return 0;
}

int buffer_read(Buffer *buf, char *data, size_t len) {
    if (!buf || !data) return -1;
    if (buf->size == 0) return 0;

    size_t to_read = len > buf->size ? buf->size : len;

    size_t first_read = to_read;
    if (buf->head + to_read > buf->capacity) {
        first_read = buf->capacity - buf->head;
    }

    memcpy(data, buf->data + buf->head, first_read);

    if (first_read < to_read) {
        memcpy(data + first_read, buf->data, to_read - first_read);
        buf->head = to_read - first_read;
    } else {
        buf->head = (buf->head + first_read) % buf->capacity;
    }

    buf->size -= to_read;
    return (int)to_read;
}

size_t buffer_available(Buffer *buf) {
    if (!buf) return 0;
    return buf->size;
}

size_t buffer_capacity(Buffer *buf) {
    if (!buf) return 0;
    return buf->capacity;
}

/* ========== Connection 实现 ========== */

Connection *connection_create(int fd, EventLoop *loop) {
    return connection_create_ex(fd, loop, DEFAULT_READ_BUF_CAP,
                                 DEFAULT_WRITE_BUF_CAP, BUFFER_MODE_FIXED);
}

Connection *connection_create_ex(int fd, EventLoop *loop,
                                  size_t read_buf_cap,
                                  size_t write_buf_cap,
                                  BufferMode mode) {
    Connection *conn = malloc(sizeof(Connection));
    if (!conn) return NULL;

    conn->fd = fd;
    conn->loop = loop;
    conn->state = CONN_STATE_CONNECTING;

    conn->read_buf = buffer_create_ex(read_buf_cap, mode, DEFAULT_MAX_CAPACITY);
    if (!conn->read_buf) {
        free(conn);
        return NULL;
    }

    conn->write_buf = buffer_create_ex(write_buf_cap, mode, DEFAULT_MAX_CAPACITY);
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

    char temp[4096];
    int to_read = avail > sizeof(temp) ? sizeof(temp) : avail;
    int n = buffer_read(conn->write_buf, temp, to_read);

    if (n <= 0) return -1;

    ssize_t written = write(conn->fd, temp, n);
    if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 重新放回缓冲区 */
            buffer_write(conn->write_buf, temp, n);
            return 0;
        }
        return -1;
    }

    return (int)written;
}

void connection_close(Connection *conn) {
    if (!conn) return;

    connection_set_state(conn, CONN_STATE_CLOSING);

    if (conn->loop) {
        eventloop_remove(conn->loop, conn->fd);
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