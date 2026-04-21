/**
 * @file    connection.h
 * @brief   TCP 连接管理，处理连接的读/写/状态管理
 *
 * @details
 *          - 创建和销毁连接
 *          - 管理连接状态（CONNECTING/READING/WRITING/CLOSING/CLOSED）
 *          - 管理读写缓冲区
 *          - 使用回调模式替代 EventLoop 直接依赖
 *
 * @layer   Core Layer
 *
 * @depends buffer
 * @usedby  server, connection_pool, examples
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#ifndef CHASE_CONNECTION_H
#define CHASE_CONNECTION_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "buffer.h"

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

/* Connection 结构体（opaque） */
typedef struct Connection Connection;

/**
 * 连接关闭回调函数类型
 * @param fd 文件描述符
 * @param user_data 用户数据
 */
typedef void (*ConnectionCloseCallback)(int fd, void *user_data);

/**
 * 创建连接
 * @param fd 文件描述符
 * @param on_close 关闭回调函数（可 NULL）
 * @param close_user_data 关闭回调的用户数据
 * @return Connection 指针，失败返回 NULL
 */
Connection *connection_create(int fd,
                              ConnectionCloseCallback on_close,
                              void *close_user_data);

/**
 * 创建连接（扩展参数）
 * @param fd 文件描述符
 * @param on_close 关闭回调函数（可 NULL）
 * @param close_user_data 关闭回调的用户数据
 * @param read_buf_cap 读缓冲区容量
 * @param write_buf_cap 写缓冲区容量
 * @param mode 缓冲区模式
 * @return Connection 指针，失败返回 NULL
 */
Connection *connection_create_ex(int fd,
                                  ConnectionCloseCallback on_close,
                                  void *close_user_data,
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

/**
 * 获取读缓冲区
 * @param conn Connection 指针
 * @return Buffer 指针
 */
Buffer *connection_get_read_buffer(Connection *conn);

/**
 * 获取写缓冲区
 * @param conn Connection 指针
 * @return Buffer 指针
 */
Buffer *connection_get_write_buffer(Connection *conn);

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
 * 从池初始化连接（设置新 fd 和回调）
 * @param conn Connection 指针（从池获取）
 * @param fd 新文件描述符
 * @param on_close 关闭回调函数
 * @param close_user_data 关闭回调的用户数据
 * @return 0 成功，-1 失败
 */
int connection_init_from_pool(Connection *conn, int fd,
                               ConnectionCloseCallback on_close,
                               void *close_user_data);

/**
 * 解除 fd 关联（释放到池前调用）
 * @param conn Connection 指针
 */
void connection_dissociate_fd(Connection *conn);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_CONNECTION_H */