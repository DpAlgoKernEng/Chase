/**
 * @file    ssl_wrap.h
 * @brief   SSL/TLS 包装模块，支持 OpenSSL 1.1.1 和 3.x
 *
 * @details
 *          - OpenSSL 版本兼容层
 *          - SSL 握手状态管理
 *          - SSL_read/write 包装（处理 WANT_READ/WANT_WRITE）
 *          - 会话缓存 + TLS 1.3 Session Ticket
 *
 * @layer   Core Layer
 *
 * @depends OpenSSL
 * @usedby  server, connection
 *
 * @author  minghui.liu
 * @date    2026-04-22
 */

#ifndef CHASE_SSL_WRAP_H
#define CHASE_SSL_WRAP_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SSL 连接状态 */
typedef enum {
    SSL_STATE_INIT,           /* 初始状态 */
    SSL_STATE_HANDSHAKING,    /* SSL 握手进行中 */
    SSL_STATE_CONNECTED,      /* SSL 连接已建立 */
    SSL_STATE_WANT_READ,      /* 需要更多数据来完成操作 */
    SSL_STATE_WANT_WRITE,     /* 需要写缓冲区可用 */
    SSL_STATE_CLOSED,         /* SSL 连接已关闭 */
    SSL_STATE_ERROR           /* SSL 错误 */
} SslState;

/* SSL 配置 */
typedef struct SslConfig {
    const char *cert_file;    /* 证书文件路径 */
    const char *key_file;     /* 私钥文件路径 */
    const char *ca_file;      /* CA 证书文件路径（可选） */
    bool verify_peer;         /* 是否验证客户端证书 */
    int verify_depth;         /* 证书链验证深度 */
    int session_timeout;      /* 会话缓存超时（秒） */
    bool enable_tickets;      /* 是否启用 Session Ticket */
} SslConfig;

/* SSL 连接上下文（不透明指针） */
typedef struct SslConnection SslConnection;

/* SSL 服务器上下文（不透明指针） */
typedef struct SslServerCtx SslServerCtx;

/**
 * 创建 SSL 服务器上下文
 * @param config SSL 配置
 * @return SslServerCtx 指针，失败返回 NULL
 */
SslServerCtx *ssl_server_ctx_create(const SslConfig *config);

/**
 * 销毁 SSL 服务器上下文
 * @param ctx SslServerCtx 指针
 */
void ssl_server_ctx_destroy(SslServerCtx *ctx);

/**
 * 创建 SSL 连接
 * @param server_ctx SSL 服务器上下文
 * @param fd socket 文件描述符
 * @return SslConnection 指针，失败返回 NULL
 */
SslConnection *ssl_connection_create(SslServerCtx *server_ctx, int fd);

/**
 * 销毁 SSL 连接
 * @param conn SslConnection 指针
 */
void ssl_connection_destroy(SslConnection *conn);

/**
 * 执行 SSL 握手
 * @param conn SslConnection 指针
 * @return SSL 状态
 */
SslState ssl_handshake(SslConnection *conn);

/**
 * SSL 读数据
 * @param conn SslConnection 指针
 * @param buf 缓冲区
 * @param len 缓冲区大小
 * @return 读入的字节数，负值表示需要更多数据或错误
 */
int ssl_read(SslConnection *conn, void *buf, size_t len);

/**
 * SSL 写数据
 * @param conn SslConnection 指针
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @return 写入的字节数，负值表示需要更多空间或错误
 */
int ssl_write(SslConnection *conn, const void *buf, size_t len);

/**
 * 获取 SSL 连接状态
 * @param conn SslConnection 指针
 * @return SSL 状态
 */
SslState ssl_get_state(SslConnection *conn);

/**
 * 获取 SSL 错误码
 * @param conn SslConnection 指针
 * @return 错误码
 */
int ssl_get_error(SslConnection *conn);

/**
 * 获取 SSL 错误字符串
 * @param error_code 错误码
 * @return 错误字符串
 */
const char *ssl_get_error_string(int error_code);

/**
 * 关闭 SSL 连接
 * @param conn SslConnection 指针
 * @return 0 成功，负值错误
 */
int ssl_shutdown(SslConnection *conn);

/**
 * 检查是否启用了 Session Ticket
 * @param server_ctx SSL 服务器上下文
 * @return true 启用，false 未启用
 */
bool ssl_is_session_ticket_enabled(SslServerCtx *server_ctx);

/**
 * 获取客户端证书验证结果
 * @param conn SslConnection 指针
 * @return 验证结果（X509_V_* 常量）
 */
int ssl_get_peer_verify_result(SslConnection *conn);

/**
 * 获取客户端证书主题名称
 * @param conn SslConnection 指针
 * @return 主题名称字符串（需调用者释放）
 */
char *ssl_get_peer_cert_subject(SslConnection *conn);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_SSL_WRAP_H */