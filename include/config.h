/**
 * @file    config.h
 * @brief   HTTP 服务器配置模块
 *
 * @details
 *          - HttpConfig 结构体定义（服务器配置）
 *          - JSON 配置文件加载
 *          - 配置验证（必填字段检查）
 *          - 默认配置值
 *
 * @layer   Core Layer
 *
 * @depends server.h, ssl_wrap.h, vhost.h
 * @usedby  server, main
 *
 * @author  minghui.liu
 * @date    2026-04-22
 */

#ifndef CHASE_CONFIG_H
#define CHASE_CONFIG_H

#include "server.h"
#include "ssl_wrap.h"
#include "vhost.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP 服务器完整配置 */
typedef struct HttpConfig {
    /* 基础配置 */
    int port;                    /* 监听端口 */
    char *bind_address;          /* 绑定地址（可选） */
    int max_connections;         /* 最大连接数 */
    int backlog;                 /* listen backlog */
    bool reuseport;              /* 是否启用 SO_REUSEPORT */

    /* 缓冲区配置 */
    int read_buffer_capacity;    /* 读缓冲区容量 */
    int write_buffer_capacity;   /* 写缓冲区容量 */

    /* Keep-Alive 配置 */
    int connection_timeout_ms;   /* 连接空闲超时（毫秒） */
    int keepalive_timeout_ms;    /* Keep-Alive 超时（毫秒） */
    int max_keepalive_requests;  /* 单连接最大请求数 */

    /* SSL/TLS 配置 */
    bool ssl_enabled;            /* 是否启用 SSL */
    SslConfig ssl_config;        /* SSL 配置 */

    /* 虚拟主机配置 */
    VHostManager *vhost_manager; /* 虚拟主机管理器 */

    /* 配置来源 */
    char *config_file_path;      /* 配置文件路径 */
    bool from_file;              /* 是否从文件加载 */
} HttpConfig;

/* 配置加载选项 */
typedef struct ConfigLoadOptions {
    const char *config_file;     /* 配置文件路径 */
    bool validate_required;      /* 是否验证必填字段 */
    bool load_defaults;          /* 未设置字段是否使用默认值 */
} ConfigLoadOptions;

/**
 * 创建默认配置
 * @return HttpConfig 指针，失败返回 NULL
 */
HttpConfig *http_config_create_default(void);

/**
 * 从 JSON 文件加载配置
 * @param file_path JSON 配置文件路径
 * @param options 加载选项
 * @return HttpConfig 指针，失败返回 NULL
 */
HttpConfig *http_config_load_from_file(const char *file_path, const ConfigLoadOptions *options);

/**
 * 销毁配置
 * @param config HttpConfig 指针
 */
void http_config_destroy(HttpConfig *config);

/**
 * 验证配置有效性
 * @param config HttpConfig 指针
 * @return 0 有效，负值无效（返回错误码）
 */
int http_config_validate(HttpConfig *config);

/**
 * 获取配置验证错误字符串
 * @param error_code 错误码
 * @return 错误字符串
 */
const char *http_config_get_error_string(int error_code);

/**
 * 将 HttpConfig 转换为 ServerConfig
 * @param http_config HttpConfig 指针
 * @return ServerConfig 结构体
 */
ServerConfig http_config_to_server_config(HttpConfig *http_config);

/**
 * 合并两个配置（src 覆盖 dst）
 * @param dst 目标配置
 * @param src 源配置
 * @return 0 成功，负值错误
 */
int http_config_merge(HttpConfig *dst, HttpConfig *src);

/* 配置错误码 */
#define CONFIG_OK                    0
#define CONFIG_ERR_INVALID_PORT     -1
#define CONFIG_ERR_INVALID_ADDR     -2
#define CONFIG_ERR_INVALID_TIMEOUT  -3
#define CONFIG_ERR_MISSING_CERT     -4
#define CONFIG_ERR_MISSING_KEY      -5
#define CONFIG_ERR_CERT_KEY_MISMATCH -6
#define CONFIG_ERR_FILE_NOT_FOUND   -7
#define CONFIG_ERR_PARSE_FAILED     -8
#define CONFIG_ERR_INVALID_JSON     -9

/* 默认值 */
#define DEFAULT_PORT               8080
#define DEFAULT_MAX_CONNECTIONS    1024
#define DEFAULT_BACKLOG            128
#define DEFAULT_CONNECTION_TIMEOUT 60000
#define DEFAULT_KEEPALIVE_TIMEOUT  5000
#define DEFAULT_MAX_KEEPALIVE_REQS 100

#ifdef __cplusplus
}
#endif

#endif /* CHASE_CONFIG_H */