#ifndef CHASE_CONNECTION_H
#define CHASE_CONNECTION_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "eventloop.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 连接状态 */
typedef enum {
    CONN_STATE_CONNECTING,
    CONN_STATE_SSL_HANDSHAKING,
    CONN_STATE_READING,
    CONN_STATE_PROCESSING,
    CONN_STATE_WRITING,
    CONN_STATE_CLOSING,
    CONN_STATE_CLOSED
} ConnState;

/* 缓冲区模式 */
typedef enum {
    BUFFER_MODE_FIXED,     /* 固定容量（安全优先） */
    BUFFER_MODE_AUTO       /* 自动扩容（灵活性优先） */
} BufferMode;

/* Buffer 结构体 */
typedef struct Buffer Buffer;

/* Connection 结构体 */
typedef struct Connection Connection;

/**
 * 创建缓冲区
 * @param capacity 初始容量
 * @return Buffer 指针，失败返回 NULL
 */
Buffer *buffer_create(size_t capacity);

/**
 * 创建缓冲区（扩展参数）
 * @param capacity 初始容量
 * @param mode 扩容模式
 * @param max_cap 最大容量
 * @return Buffer 指针，失败返回 NULL
 */
Buffer *buffer_create_ex(size_t capacity, BufferMode mode, size_t max_cap);

/**
 * 销毁缓冲区
 * @param buf Buffer 指针
 */
void buffer_destroy(Buffer *buf);

/**
 * 写入数据到缓冲区
 * @param buf Buffer 指针
 * @param data 数据
 * @param len 长度
 * @return 0 成功，-1 失败（溢出）
 */
int buffer_write(Buffer *buf, const char *data, size_t len);

/**
 * 从缓冲区读取数据
 * @param buf Buffer 指针
 * @param data 目标缓冲区
 * @param len 要读取的长度
 * @return 实际读取的字节数
 */
int buffer_read(Buffer *buf, char *data, size_t len);

/**
 * 获取可读数据量
 * @param buf Buffer 指针
 * @return 可读字节数
 */
size_t buffer_available(Buffer *buf);

/**
 * 获取缓冲区容量
 * @param buf Buffer 指针
 * @return 容量
 */
size_t buffer_capacity(Buffer *buf);

/**
 * 创建连接
 * @param fd 文件描述符
 * @param loop EventLoop 指针
 * @return Connection 指针，失败返回 NULL
 */
Connection *connection_create(int fd, EventLoop *loop);

/**
 * 创建连接（扩展参数）
 * @param fd 文件描述符
 * @param loop EventLoop 指针
 * @param read_buf_cap 读缓冲区容量
 * @param write_buf_cap 写缓冲区容量
 * @param mode 缓冲区模式
 * @return Connection 指针，失败返回 NULL
 */
Connection *connection_create_ex(int fd, EventLoop *loop,
                                  size_t read_buf_cap,
                                  size_t write_buf_cap,
                                  BufferMode mode);

/**
 * 销毁连接
 * @param conn Connection 指针
 */
void connection_destroy(Connection *conn);

/**
 * 从连接读取数据
 * @param conn Connection 指针
 * @return 读取的字节数，-1 表示错误
 */
int connection_read(Connection *conn);

/**
 * 向连接写入数据
 * @param conn Connection 指针
 * @return 写入的字节数，-1 表示错误
 */
int connection_write(Connection *conn);

/**
 * 关闭连接
 * @param conn Connection 指针
 */
void connection_close(Connection *conn);

/**
 * 设置连接状态
 * @param conn Connection 指针
 * @param state 新状态
 */
void connection_set_state(Connection *conn, ConnState state);

/**
 * 获取连接状态
 * @param conn Connection 指针
 * @return 当前状态
 */
ConnState connection_get_state(Connection *conn);

/**
 * 获取连接的文件描述符
 * @param conn Connection 指针
 * @return 文件描述符
 */
int connection_get_fd(Connection *conn);

/* ========== 连接池管理字段 API（内部使用） ========== */

/**
 * 获取连接的下一个指针（池管理）
 * @param conn Connection 指针
 * @return 下一个 Connection 指针
 */
Connection *connection_get_next(Connection *conn);

/**
 * 设置连接的下一个指针（池管理）
 * @param conn Connection 指针
 * @param next 下一个 Connection 指针
 */
void connection_set_next(Connection *conn, Connection *next);

/**
 * 获取连接的上一个指针（池管理）
 * @param conn Connection 指针
 * @return 上一个 Connection 指针
 */
Connection *connection_get_prev(Connection *conn);

/**
 * 设置连接的上一个指针（池管理）
 * @param conn Connection 指针
 * @param prev 上一个 Connection 指针
 */
void connection_set_prev(Connection *conn, Connection *prev);

/**
 * 获取连接的释放时间（池管理）
 * @param conn Connection 指针
 * @return 释放时间（毫秒）
 */
uint64_t connection_get_release_time(Connection *conn);

/**
 * 设置连接的释放时间（池管理）
 * @param conn Connection 指针
 * @param time 释放时间（毫秒）
 */
void connection_set_release_time(Connection *conn, uint64_t time);

/**
 * 检查连接是否是临时分配（池管理）
 * @param conn Connection 指针
 * @return 1 是临时分配，0 是预分配
 */
int connection_is_temp_allocated(Connection *conn);

/**
 * 设置连接的临时分配标记（池管理）
 * @param conn Connection 指针
 * @param is_temp 1 是临时分配，0 是预分配
 */
void connection_set_temp_allocated(Connection *conn, int is_temp);

/**
 * 获取连接的用户数据
 * @param conn Connection 指针
 * @return 用户数据指针
 */
void *connection_get_user_data(Connection *conn);

/**
 * 设置连接的用户数据
 * @param conn Connection 指针
 * @param user_data 用户数据指针
 */
void connection_set_user_data(Connection *conn, void *user_data);

/**
 * 重置连接（清空缓冲区，重置状态）
 * @param conn Connection 指针
 */
void connection_reset(Connection *conn);

/**
 * 从池初始化连接（设置新 fd 和 loop）
 * @param conn Connection 指针（从池获取）
 * @param fd 新文件描述符
 * @param loop EventLoop 指针
 * @return 0 成功，-1 失败
 */
int connection_init_from_pool(Connection *conn, int fd, EventLoop *loop);

/**
 * 解除 fd 关联（释放到池前调用）
 * @param conn Connection 指针
 */
void connection_dissociate_fd(Connection *conn);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_CONNECTION_H */