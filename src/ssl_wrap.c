/**
 * @file    ssl_wrap.c
 * @brief   SSL/TLS 包装模块实现，支持 OpenSSL 1.1.1 和 3.x
 *
 * @details
 *          - OpenSSL 版本兼容层
 *          - SSL 握手状态管理（SSL_HANDSHAKING）
 *          - SSL_read/write 包装（处理 WANT_READ/WANT_WRITE）
 *          - 会话缓存 + TLS 1.3 Session Ticket
 *
 * @layer   Core Layer
 *
 * @depends OpenSSL, ssl_wrap.h
 * @usedby  server, connection
 *
 * @author  minghui.liu
 * @date    2026-04-22
 */

#include "ssl_wrap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

/* ========== OpenSSL 版本兼容层 ========== */

/* OpenSSL 3.x 使用不同的 API */
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    /* OpenSSL 3.x */
    #define SSL_CTX_USE_CERT_CHAIN_FILE(ctx, file) SSL_CTX_use_certificate_chain_file(ctx, file)
#else
    /* OpenSSL 1.1.1 */
    #define SSL_CTX_USE_CERT_CHAIN_FILE(ctx, file) SSL_CTX_use_certificate_file(ctx, file, SSL_FILETYPE_PEM)
#endif

/* ========== 内部结构体定义 ========== */

struct SslServerCtx {
    SSL_CTX *ssl_ctx;
    bool tickets_enabled;
    int session_timeout;
};

struct SslConnection {
    SSL *ssl;
    SslState state;
    int last_error;
    int fd;
};

/* ========== 全局初始化 ========== */

static bool ssl_initialized = false;

static void ssl_global_init(void) {
    if (!ssl_initialized) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        ssl_initialized = true;
    }
}

/* ========== SSL 服务器上下文 ========== */

SslServerCtx *ssl_server_ctx_create(const SslConfig *config) {
    if (!config || !config->cert_file || !config->key_file) {
        return NULL;
    }

    ssl_global_init();

    SslServerCtx *ctx = malloc(sizeof(SslServerCtx));
    if (!ctx) return NULL;

    /* 创建 SSL_CTX */
    const SSL_METHOD *method = TLS_server_method();
    ctx->ssl_ctx = SSL_CTX_new(method);
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    /* 设置最低 TLS 版本为 TLS 1.2 */
    if (SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION) != 1) {
        SSL_CTX_free(ctx->ssl_ctx);
        free(ctx);
        return NULL;
    }

    /* 加载证书 */
    if (SSL_CTX_use_certificate_file(ctx->ssl_ctx, config->cert_file, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "[SSL] Failed to load certificate: %s\n", config->cert_file);
        SSL_CTX_free(ctx->ssl_ctx);
        free(ctx);
        return NULL;
    }

    /* 加载私钥 */
    if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, config->key_file, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "[SSL] Failed to load private key: %s\n", config->key_file);
        SSL_CTX_free(ctx->ssl_ctx);
        free(ctx);
        return NULL;
    }

    /* 验证证书和私钥匹配 */
    if (SSL_CTX_check_private_key(ctx->ssl_ctx) != 1) {
        fprintf(stderr, "[SSL] Certificate and private key do not match\n");
        SSL_CTX_free(ctx->ssl_ctx);
        free(ctx);
        return NULL;
    }

    /* 加载 CA 证书（可选） */
    if (config->ca_file) {
        if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, config->ca_file, NULL) != 1) {
            fprintf(stderr, "[SSL] Failed to load CA certificate: %s\n", config->ca_file);
        }
    }

    /* 客户端证书验证 */
    if (config->verify_peer) {
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_PEER, NULL);
        SSL_CTX_set_verify_depth(ctx->ssl_ctx, config->verify_depth > 0 ? config->verify_depth : 1);
    } else {
        SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_NONE, NULL);
    }

    /* 会话缓存配置 */
    ctx->session_timeout = config->session_timeout > 0 ? config->session_timeout : 300;
    SSL_CTX_set_session_cache_mode(ctx->ssl_ctx, SSL_SESS_CACHE_SERVER);
    SSL_CTX_set_timeout(ctx->ssl_ctx, ctx->session_timeout);

    /* Session Ticket（TLS 1.3） */
    ctx->tickets_enabled = config->enable_tickets;
    if (ctx->tickets_enabled) {
        /* OpenSSL 3.x 和 1.1.1 的 Session Ticket API 不同 */
        #if OPENSSL_VERSION_NUMBER >= 0x30000000L
            /* OpenSSL 3.x: 使用自动 ticket 生成 */
            SSL_CTX_set_options(ctx->ssl_ctx, SSL_OP_NO_TICKET);
            SSL_CTX_clear_options(ctx->ssl_ctx, SSL_OP_NO_TICKET);
        #else
            /* OpenSSL 1.1.1 */
            SSL_CTX_set_tlsext_ticket_keys(ctx->ssl_ctx, NULL, 0);
        #endif
    }

    /* 设置密码套件（安全配置） */
    const char *cipher_list = "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
                              "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
                              "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305";
    if (SSL_CTX_set_cipher_list(ctx->ssl_ctx, cipher_list) != 1) {
        fprintf(stderr, "[SSL] Failed to set cipher list\n");
    }

    return ctx;
}

void ssl_server_ctx_destroy(SslServerCtx *ctx) {
    if (!ctx) return;
    if (ctx->ssl_ctx) {
        SSL_CTX_free(ctx->ssl_ctx);
    }
    free(ctx);
}

/* ========== SSL 连接 ========== */

SslConnection *ssl_connection_create(SslServerCtx *server_ctx, int fd) {
    if (!server_ctx || !server_ctx->ssl_ctx || fd < 0) {
        return NULL;
    }

    SslConnection *conn = malloc(sizeof(SslConnection));
    if (!conn) return NULL;

    conn->ssl = SSL_new(server_ctx->ssl_ctx);
    if (!conn->ssl) {
        free(conn);
        return NULL;
    }

    /* 设置 socket */
    if (SSL_set_fd(conn->ssl, fd) != 1) {
        SSL_free(conn->ssl);
        free(conn);
        return NULL;
    }

    conn->state = SSL_STATE_INIT;
    conn->last_error = 0;
    conn->fd = fd;

    return conn;
}

void ssl_connection_destroy(SslConnection *conn) {
    if (!conn) return;
    if (conn->ssl) {
        if (conn->state == SSL_STATE_CONNECTED) {
            /* 尝试优雅关闭 */
            SSL_shutdown(conn->ssl);
        }
        SSL_free(conn->ssl);
    }
    free(conn);
}

/* ========== SSL 握手 ========== */

SslState ssl_handshake(SslConnection *conn) {
    if (!conn || !conn->ssl) {
        return SSL_STATE_ERROR;
    }

    if (conn->state == SSL_STATE_CONNECTED || conn->state == SSL_STATE_CLOSED) {
        return conn->state;
    }

    int ret = SSL_accept(conn->ssl);
    conn->last_error = SSL_get_error(conn->ssl, ret);

    if (ret == 1) {
        /* 握手成功 */
        conn->state = SSL_STATE_CONNECTED;
        return SSL_STATE_CONNECTED;
    } else if (conn->last_error == SSL_ERROR_WANT_READ) {
        /* 需要更多数据 */
        conn->state = SSL_STATE_HANDSHAKING;
        return SSL_STATE_WANT_READ;
    } else if (conn->last_error == SSL_ERROR_WANT_WRITE) {
        /* 需要写缓冲区 */
        conn->state = SSL_STATE_HANDSHAKING;
        return SSL_STATE_WANT_WRITE;
    } else {
        /* 握手失败 */
        conn->state = SSL_STATE_ERROR;
        fprintf(stderr, "[SSL] Handshake failed: error=%d, ssl_error=%d\n",
                ret, conn->last_error);
        ERR_print_errors_fp(stderr);
        return SSL_STATE_ERROR;
    }
}

/* ========== SSL 读/写 ========== */

int ssl_read(SslConnection *conn, void *buf, size_t len) {
    if (!conn || !conn->ssl || !buf || len == 0) {
        return -1;
    }

    if (conn->state != SSL_STATE_CONNECTED) {
        return -1;
    }

    int ret = SSL_read(conn->ssl, buf, (int)len);
    conn->last_error = SSL_get_error(conn->ssl, ret);

    if (ret > 0) {
        /* 读成功 */
        return ret;
    } else if (conn->last_error == SSL_ERROR_WANT_READ) {
        /* 需要更多数据 */
        conn->state = SSL_STATE_WANT_READ;
        return 0;  /* 返回 0 表示需要继续读 */
    } else if (conn->last_error == SSL_ERROR_WANT_WRITE) {
        /* 需要写（ renegotiation） */
        conn->state = SSL_STATE_WANT_WRITE;
        return 0;
    } else if (conn->last_error == SSL_ERROR_ZERO_RETURN) {
        /* SSL 连接正常关闭 */
        conn->state = SSL_STATE_CLOSED;
        return 0;
    } else if (conn->last_error == SSL_ERROR_SYSCALL) {
        /* 系统错误 */
        if (errno == 0) {
            /* EOF */
            conn->state = SSL_STATE_CLOSED;
            return 0;
        }
        conn->state = SSL_STATE_ERROR;
        return -1;
    } else {
        /* 其他错误 */
        conn->state = SSL_STATE_ERROR;
        return -1;
    }
}

int ssl_write(SslConnection *conn, const void *buf, size_t len) {
    if (!conn || !conn->ssl || !buf || len == 0) {
        return -1;
    }

    if (conn->state != SSL_STATE_CONNECTED &&
        conn->state != SSL_STATE_WANT_READ &&
        conn->state != SSL_STATE_WANT_WRITE) {
        return -1;
    }

    int ret = SSL_write(conn->ssl, buf, (int)len);
    conn->last_error = SSL_get_error(conn->ssl, ret);

    if (ret > 0) {
        /* 写成功 */
        conn->state = SSL_STATE_CONNECTED;
        return ret;
    } else if (conn->last_error == SSL_ERROR_WANT_READ) {
        /* 需要 renegotiation 读 */
        conn->state = SSL_STATE_WANT_READ;
        return 0;
    } else if (conn->last_error == SSL_ERROR_WANT_WRITE) {
        /* 需要更多写缓冲区 */
        conn->state = SSL_STATE_WANT_WRITE;
        return 0;
    } else if (conn->last_error == SSL_ERROR_ZERO_RETURN) {
        /* SSL 连接关闭 */
        conn->state = SSL_STATE_CLOSED;
        return 0;
    } else {
        /* 其他错误 */
        conn->state = SSL_STATE_ERROR;
        return -1;
    }
}

/* ========== SSL 状态/错误 ========== */

SslState ssl_get_state(SslConnection *conn) {
    if (!conn) return SSL_STATE_ERROR;
    return conn->state;
}

int ssl_get_error(SslConnection *conn) {
    if (!conn) return -1;
    return conn->last_error;
}

const char *ssl_get_error_string(int error_code) {
    static char err_buf[256];

    switch (error_code) {
    case SSL_ERROR_NONE:
        return "No error";
    case SSL_ERROR_SSL:
        return "SSL protocol error";
    case SSL_ERROR_WANT_READ:
        return "Need more data to read";
    case SSL_ERROR_WANT_WRITE:
        return "Need to write more data";
    case SSL_ERROR_WANT_X509_LOOKUP:
        return "Want X509 lookup";
    case SSL_ERROR_SYSCALL:
        return "System call error";
    case SSL_ERROR_ZERO_RETURN:
        return "SSL connection closed";
    case SSL_ERROR_WANT_CONNECT:
        return "Want connect";
    case SSL_ERROR_WANT_ACCEPT:
        return "Want accept";
    default:
        snprintf(err_buf, sizeof(err_buf), "Unknown SSL error: %d", error_code);
        return err_buf;
    }
}

/* ========== SSL 关闭 ========== */

int ssl_shutdown(SslConnection *conn) {
    if (!conn || !conn->ssl) {
        return -1;
    }

    if (conn->state == SSL_STATE_CLOSED) {
        return 0;
    }

    int ret = SSL_shutdown(conn->ssl);
    conn->last_error = SSL_get_error(conn->ssl, ret);

    if (ret == 1) {
        /* 关闭成功 */
        conn->state = SSL_STATE_CLOSED;
        return 0;
    } else if (ret == 0) {
        /* 需要第二次调用 */
        conn->state = SSL_STATE_CLOSED;
        return 0;
    } else {
        /* 关闭失败 */
        conn->state = SSL_STATE_ERROR;
        return -1;
    }
}

/* ========== 辅助函数 ========== */

bool ssl_is_session_ticket_enabled(SslServerCtx *server_ctx) {
    if (!server_ctx) return false;
    return server_ctx->tickets_enabled;
}

int ssl_get_peer_verify_result(SslConnection *conn) {
    if (!conn || !conn->ssl) return -1;
    return SSL_get_verify_result(conn->ssl);
}

char *ssl_get_peer_cert_subject(SslConnection *conn) {
    if (!conn || !conn->ssl) return NULL;

    X509 *cert = SSL_get_peer_certificate(conn->ssl);
    if (!cert) return NULL;

    X509_NAME *name = X509_get_subject_name(cert);
    if (!name) {
        X509_free(cert);
        return NULL;
    }

    char *subject = X509_NAME_oneline(name, NULL, 0);
    X509_free(cert);

    return subject;  /* 调用者需要释放 */
}