/**
 * @file    config.c
 * @brief   HTTP 服务器配置模块实现
 *
 * @details
 *          - 简化 JSON 解析（手动解析，无外部依赖）
 *          - 配置验证
 *          - 默认值设置
 *          - Phase 5: 热更新支持（版本追踪、checksum、原子/渐进更新）
 *
 * @author  minghui.liu
 * @date    2026-04-22
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>  /* Phase 5: 用于等待连接关闭 */

/* ========== 辅助函数 ========== */

static char *strdup_safe(const char *s) {
    if (!s) return NULL;
    return strdup(s);
}

/* 简化的 JSON 解析（无外部依赖） */
static char *json_get_string_value(const char *json, const char *key) {
    /* 查找 "key": "value" 格式 */
    char search_pattern[128];
    snprintf(search_pattern, sizeof(search_pattern), "\"%s\"", key);

    const char *pos = strstr(json, search_pattern);
    if (!pos) return NULL;

    /* 跳过 key 和冒号 */
    pos += strlen(search_pattern);
    while (*pos && (*pos == ':' || *pos == ' ' || *pos == '\t')) pos++;

    if (*pos != '"') return NULL;
    pos++;  /* 跳过起始引号 */

    /* 找结束引号 */
    const char *end = pos;
    while (*end && *end != '"') end++;

    if (*end != '"') return NULL;

    size_t len = end - pos;
    char *value = malloc(len + 1);
    if (!value) return NULL;

    memcpy(value, pos, len);
    value[len] = '\0';
    return value;
}

static int json_get_int_value(const char *json, const char *key, int default_val) {
    char search_pattern[128];
    snprintf(search_pattern, sizeof(search_pattern), "\"%s\"", key);

    const char *pos = strstr(json, search_pattern);
    if (!pos) return default_val;

    pos += strlen(search_pattern);
    while (*pos && (*pos == ':' || *pos == ' ' || *pos == '\t')) pos++;

    /* 解析数字 */
    char *endptr;
    long value = strtol(pos, &endptr, 10);
    if (endptr == pos) return default_val;

    return (int)value;
}

static bool json_get_bool_value(const char *json, const char *key, bool default_val) {
    char search_pattern[128];
    snprintf(search_pattern, sizeof(search_pattern), "\"%s\"", key);

    const char *pos = strstr(json, search_pattern);
    if (!pos) return default_val;

    pos += strlen(search_pattern);
    while (*pos && (*pos == ':' || *pos == ' ' || *pos == '\t')) pos++;

    if (strncmp(pos, "true", 4) == 0) return true;
    if (strncmp(pos, "false", 5) == 0) return false;

    return default_val;
}

/* ========== HttpConfig 实现 ========== */

HttpConfig *http_config_create_default(void) {
    HttpConfig *config = malloc(sizeof(HttpConfig));
    if (!config) return NULL;

    /* 设置默认值 */
    config->port = DEFAULT_PORT;
    config->bind_address = NULL;
    config->max_connections = DEFAULT_MAX_CONNECTIONS;
    config->backlog = DEFAULT_BACKLOG;
    config->reuseport = false;

    config->read_buffer_capacity = 0;   /* 0 = 使用 server 默认 */
    config->write_buffer_capacity = 0;

    config->connection_timeout_ms = DEFAULT_CONNECTION_TIMEOUT;
    config->keepalive_timeout_ms = DEFAULT_KEEPALIVE_TIMEOUT;
    config->max_keepalive_requests = DEFAULT_MAX_KEEPALIVE_REQS;

    config->ssl_enabled = false;
    config->ssl_config.cert_file = NULL;
    config->ssl_config.key_file = NULL;
    config->ssl_config.ca_file = NULL;
    config->ssl_config.verify_peer = false;
    config->ssl_config.verify_depth = 1;
    config->ssl_config.session_timeout = 300;
    config->ssl_config.enable_tickets = true;

    config->vhost_manager = NULL;
    config->config_file_path = NULL;
    config->from_file = false;

    /* Phase 5: 热更新字段初始化 */
    config->version.version = 1;
    config->version.checksum = 0;
    config->version.timestamp = 0;
    config->version.source_file = NULL;
    config->update_policy = CONFIG_UPDATE_ATOMIC;
    config->hot_update_enabled = false;
    config->active_connections = 0;
    config->last_update_time = 0;

    return config;
}

HttpConfig *http_config_load_from_file(const char *file_path, const ConfigLoadOptions *options) {
    if (!file_path) return NULL;

    FILE *f = fopen(file_path, "r");
    if (!f) {
        fprintf(stderr, "[Config] Failed to open config file: %s (%s)\n",
                file_path, strerror(errno));
        return NULL;
    }

    /* 读取文件内容 */
    char *json_content = NULL;
    size_t file_size = 0;

    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    json_content = malloc(file_size + 1);
    if (!json_content) {
        fclose(f);
        return NULL;
    }

    size_t read_size = fread(json_content, 1, file_size, f);
    json_content[read_size] = '\0';
    fclose(f);

    /* 创建配置 */
    HttpConfig *config = http_config_create_default();
    if (!config) {
        free(json_content);
        return NULL;
    }

    config->config_file_path = strdup_safe(file_path);
    config->from_file = true;

    /* 解析 JSON */
    config->port = json_get_int_value(json_content, "port", config->port);

    char *bind_addr = json_get_string_value(json_content, "bind_address");
    if (bind_addr) {
        config->bind_address = bind_addr;
    }

    config->max_connections = json_get_int_value(json_content, "max_connections", config->max_connections);
    config->backlog = json_get_int_value(json_content, "backlog", config->backlog);
    config->reuseport = json_get_bool_value(json_content, "reuseport", config->reuseport);

    config->connection_timeout_ms = json_get_int_value(json_content, "connection_timeout_ms", config->connection_timeout_ms);
    config->keepalive_timeout_ms = json_get_int_value(json_content, "keepalive_timeout_ms", config->keepalive_timeout_ms);
    config->max_keepalive_requests = json_get_int_value(json_content, "max_keepalive_requests", config->max_keepalive_requests);

    /* SSL 配置 */
    char *cert_file = json_get_string_value(json_content, "cert_file");
    char *key_file = json_get_string_value(json_content, "key_file");
    char *ca_file = json_get_string_value(json_content, "ca_file");

    if (cert_file && key_file) {
        config->ssl_enabled = true;
        config->ssl_config.cert_file = cert_file;
        config->ssl_config.key_file = key_file;
        config->ssl_config.ca_file = ca_file;
        config->ssl_config.verify_peer = json_get_bool_value(json_content, "verify_peer", false);
        config->ssl_config.session_timeout = json_get_int_value(json_content, "session_timeout", 300);
        config->ssl_config.enable_tickets = json_get_bool_value(json_content, "enable_tickets", true);
    } else {
        /* 释放未使用的字符串 */
        free(cert_file);
        free(key_file);
        free(ca_file);
    }

    free(json_content);

    /* 验证 */
    if (options && options->validate_required) {
        int ret = http_config_validate(config);
        if (ret != CONFIG_OK) {
            fprintf(stderr, "[Config] Validation failed: %s\n",
                    http_config_get_error_string(ret));
            http_config_destroy(config);
            return NULL;
        }
    }

    return config;
}

void http_config_destroy(HttpConfig *config) {
    if (!config) return;

    free(config->bind_address);
    free(config->ssl_config.cert_file);
    free(config->ssl_config.key_file);
    free(config->ssl_config.ca_file);
    free(config->config_file_path);

    if (config->vhost_manager) {
        vhost_manager_destroy(config->vhost_manager);
    }

    free(config);
}

int http_config_validate(HttpConfig *config) {
    if (!config) return CONFIG_ERR_PARSE_FAILED;

    /* 验证端口 */
    if (config->port <= 0 || config->port > 65535) {
        return CONFIG_ERR_INVALID_PORT;
    }

    /* 验证超时 */
    if (config->connection_timeout_ms < 0 || config->keepalive_timeout_ms < 0) {
        return CONFIG_ERR_INVALID_TIMEOUT;
    }

    /* 如果启用 SSL，验证证书和私钥 */
    if (config->ssl_enabled) {
        if (!config->ssl_config.cert_file) {
            return CONFIG_ERR_MISSING_CERT;
        }
        if (!config->ssl_config.key_file) {
            return CONFIG_ERR_MISSING_KEY;
        }
    }

    return CONFIG_OK;
}

const char *http_config_get_error_string(int error_code) {
    switch (error_code) {
    case CONFIG_OK:
        return "OK";
    case CONFIG_ERR_INVALID_PORT:
        return "Invalid port number";
    case CONFIG_ERR_INVALID_ADDR:
        return "Invalid bind address";
    case CONFIG_ERR_INVALID_TIMEOUT:
        return "Invalid timeout value";
    case CONFIG_ERR_MISSING_CERT:
        return "Missing SSL certificate file";
    case CONFIG_ERR_MISSING_KEY:
        return "Missing SSL private key file";
    case CONFIG_ERR_CERT_KEY_MISMATCH:
        return "Certificate and key do not match";
    case CONFIG_ERR_FILE_NOT_FOUND:
        return "Configuration file not found";
    case CONFIG_ERR_PARSE_FAILED:
        return "Failed to parse configuration";
    case CONFIG_ERR_INVALID_JSON:
        return "Invalid JSON format";
    default:
        return "Unknown error";
    }
}

ServerConfig http_config_to_server_config(HttpConfig *http_config) {
    ServerConfig server_config = {0};

    if (!http_config) {
        /* 使用默认值 */
        server_config.port = DEFAULT_PORT;
        server_config.max_connections = DEFAULT_MAX_CONNECTIONS;
        server_config.backlog = DEFAULT_BACKLOG;
        server_config.connection_timeout_ms = DEFAULT_CONNECTION_TIMEOUT;
        server_config.keepalive_timeout_ms = DEFAULT_KEEPALIVE_TIMEOUT;
        server_config.max_keepalive_requests = DEFAULT_MAX_KEEPALIVE_REQS;
        return server_config;
    }

    server_config.port = http_config->port;
    server_config.max_connections = http_config->max_connections;
    server_config.backlog = http_config->backlog;
    server_config.bind_addr = http_config->bind_address;
    server_config.reuseport = http_config->reuseport;
    server_config.read_buf_cap = http_config->read_buffer_capacity;
    server_config.write_buf_cap = http_config->write_buffer_capacity;
    server_config.connection_timeout_ms = http_config->connection_timeout_ms;
    server_config.keepalive_timeout_ms = http_config->keepalive_timeout_ms;
    server_config.max_keepalive_requests = http_config->max_keepalive_requests;

    return server_config;
}

int http_config_merge(HttpConfig *dst, HttpConfig *src) {
    if (!dst || !src) return -1;

    /* src 非零值覆盖 dst */
    if (src->port > 0) dst->port = src->port;
    if (src->max_connections > 0) dst->max_connections = src->max_connections;
    if (src->backlog > 0) dst->backlog = src->backlog;
    if (src->connection_timeout_ms > 0) dst->connection_timeout_ms = src->connection_timeout_ms;
    if (src->keepalive_timeout_ms > 0) dst->keepalive_timeout_ms = src->keepalive_timeout_ms;
    if (src->max_keepalive_requests > 0) dst->max_keepalive_requests = src->max_keepalive_requests;

    /* 字符串覆盖 */
    if (src->bind_address) {
        free(dst->bind_address);
        dst->bind_address = strdup_safe(src->bind_address);
    }

    if (src->ssl_enabled) {
        dst->ssl_enabled = true;
        if (src->ssl_config.cert_file) {
            free(dst->ssl_config.cert_file);
            dst->ssl_config.cert_file = strdup_safe(src->ssl_config.cert_file);
        }
        if (src->ssl_config.key_file) {
            free(dst->ssl_config.key_file);
            dst->ssl_config.key_file = strdup_safe(src->ssl_config.key_file);
        }
    }

    return 0;
}

/* ========== Phase 5: 热更新实现 ========== */

/* 获取当前时间戳（毫秒） */
static uint64_t get_current_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 简化的 checksum 计算（基于配置字符串拼接） */
static uint64_t calculate_config_hash(HttpConfig *config) {
    if (!config) return 0;

    /* 拼接关键配置字段 */
    char buffer[1024];
    int len = snprintf(buffer, sizeof(buffer),
                       "port=%d,max_conn=%d,timeout=%d,keepalive=%d,ssl=%d",
                       config->port,
                       config->max_connections,
                       config->connection_timeout_ms,
                       config->keepalive_timeout_ms,
                       config->ssl_enabled ? 1 : 0);

    /* 简化的哈希计算 */
    uint64_t hash = 0;
    for (int i = 0; i < len; i++) {
        hash = hash * 31 + (unsigned char)buffer[i];
    }

    return hash;
}

int http_config_enable_hot_update(HttpConfig *config, ConfigUpdatePolicy policy) {
    if (!config) return -1;

    config->hot_update_enabled = true;
    config->update_policy = policy;
    config->version.version = 1;
    config->version.timestamp = get_current_ms();
    config->version.checksum = calculate_config_hash(config);

    return 0;
}

uint64_t http_config_get_version(HttpConfig *config) {
    if (!config) return 0;
    return config->version.version;
}

uint64_t http_config_calculate_checksum(HttpConfig *config) {
    if (!config) return 0;
    return calculate_config_hash(config);
}

bool http_config_has_changed(HttpConfig *config, const char *file_path) {
    if (!config || !file_path) return false;

    /* 加载新配置文件 */
    ConfigLoadOptions options = { .validate_required = false, .load_defaults = true };
    HttpConfig *new_config = http_config_load_from_file(file_path, &options);
    if (!new_config) return false;

    /* 计算新配置的 checksum */
    uint64_t new_checksum = calculate_config_hash(new_config);

    http_config_destroy(new_config);

    /* 比较 checksum */
    return (new_checksum != config->version.checksum);
}

int http_config_calculate_update_delay(HttpConfig *config, int active_connections) {
    if (!config) return 0;

    /* 原子更新：立即生效 */
    if (config->update_policy == CONFIG_UPDATE_ATOMIC) {
        return 0;
    }

    /* 渐进更新：根据活跃连接数计算延迟 */
    /* 基础延迟 + 每连接额外延迟 */
    int base_delay = 100;  /* 100ms 基础 */
    int per_conn_delay = 10;  /* 每连接 10ms */

    int delay = base_delay + (active_connections * per_conn_delay);

    /* 限制最大延迟为 keepalive timeout */
    if (delay > config->keepalive_timeout_ms) {
        delay = config->keepalive_timeout_ms;
    }

    return delay;
}

void http_config_register_connection(HttpConfig *config) {
    if (!config) return;
    config->active_connections++;
}

void http_config_unregister_connection(HttpConfig *config) {
    if (!config) return;
    if (config->active_connections > 0) {
        config->active_connections--;
    }
}

int http_config_wait_connections_close(HttpConfig *config, int max_wait_ms) {
    if (!config) return -1;

    int waited = 0;
    int check_interval = 100;  /* 100ms 检查间隔 */

    while (config->active_connections > 0 && waited < max_wait_ms) {
        usleep(check_interval * 1000);
        waited += check_interval;
    }

    return (config->active_connections == 0) ? 0 : -1;
}

int http_config_hot_update(HttpConfig *config, const char *file_path, ConfigUpdateResult *result) {
    if (!config || !file_path) return -1;

    /* 初始化结果 */
    if (result) {
        memset(result, 0, sizeof(ConfigUpdateResult));
        result->old_version = config->version.version;
        result->active_connections = config->active_connections;
    }

    /* 检查是否启用热更新 */
    if (!config->hot_update_enabled) {
        if (result) {
            result->success = false;
            result->error_message = strdup("Hot update not enabled");
        }
        return -1;
    }

    /* 检查是否有变更 */
    if (!http_config_has_changed(config, file_path)) {
        if (result) {
            result->success = true;
            result->new_version = config->version.version;
            result->delay_ms = 0;
        }
        return 0;  /* 无变更 */
    }

    /* 加载新配置 */
    ConfigLoadOptions options = { .validate_required = true, .load_defaults = true };
    HttpConfig *new_config = http_config_load_from_file(file_path, &options);
    if (!new_config) {
        if (result) {
            result->success = false;
            result->error_message = strdup("Failed to load new config");
        }
        return -1;
    }

    /* 验证新配置 */
    int ret = http_config_validate(new_config);
    if (ret != CONFIG_OK) {
        if (result) {
            result->success = false;
            result->error_message = strdup(http_config_get_error_string(ret));
        }
        http_config_destroy(new_config);
        return -1;
    }

    /* 根据更新策略执行更新 */
    int delay = http_config_calculate_update_delay(config, config->active_connections);
    if (result) {
        result->delay_ms = delay;
    }

    /* 渐进更新：等待连接关闭 */
    if (config->update_policy == CONFIG_UPDATE_GRADUAL && config->active_connections > 0) {
        int wait_result = http_config_wait_connections_close(config,
            config->keepalive_timeout_ms + delay);
        if (wait_result != 0 && config->active_connections > 0) {
            /* 超时，仍有活跃连接，可选继续或回退 */
            /* 这里选择继续更新 */
        }
    }

    /* 合并新配置 */
    http_config_merge(config, new_config);

    /* 更新版本信息 */
    config->version.version++;
    config->version.checksum = calculate_config_hash(config);
    config->version.timestamp = get_current_ms();
    if (config->version.source_file) {
        free(config->version.source_file);
    }
    config->version.source_file = strdup_safe(file_path);
    config->last_update_time = config->version.timestamp;

    if (result) {
        result->success = true;
        result->new_version = config->version.version;
    }

    http_config_destroy(new_config);
    return 0;
}

int http_config_partial_update(HttpConfig *config, const char *file_path,
                               const char **update_fields, int field_count) {
    if (!config || !file_path || !update_fields || field_count <= 0) return -1;

    /* 加载新配置 */
    ConfigLoadOptions options = { .validate_required = false, .load_defaults = false };
    HttpConfig *new_config = http_config_load_from_file(file_path, &options);
    if (!new_config) return -1;

    /* 只更新指定字段 */
    for (int i = 0; i < field_count; i++) {
        const char *field = update_fields[i];

        if (strcmp(field, "port") == 0 && new_config->port > 0) {
            config->port = new_config->port;
        } else if (strcmp(field, "max_connections") == 0 && new_config->max_connections > 0) {
            config->max_connections = new_config->max_connections;
        } else if (strcmp(field, "connection_timeout_ms") == 0 && new_config->connection_timeout_ms > 0) {
            config->connection_timeout_ms = new_config->connection_timeout_ms;
        } else if (strcmp(field, "keepalive_timeout_ms") == 0 && new_config->keepalive_timeout_ms > 0) {
            config->keepalive_timeout_ms = new_config->keepalive_timeout_ms;
        } else if (strcmp(field, "max_keepalive_requests") == 0 && new_config->max_keepalive_requests > 0) {
            config->max_keepalive_requests = new_config->max_keepalive_requests;
        } else if (strcmp(field, "backlog") == 0 && new_config->backlog > 0) {
            config->backlog = new_config->backlog;
        }
        /* SSL 字段需要单独处理 */
    }

    /* 更新版本信息 */
    config->version.version++;
    config->version.checksum = calculate_config_hash(config);
    config->version.timestamp = get_current_ms();

    http_config_destroy(new_config);
    return 0;
}

int http_config_rollback(HttpConfig *config) {
    if (!config) return -1;

    /* 简化实现：版本号回退 */
    /* 完整实现需要保存历史版本 */
    if (config->version.version <= 1) {
        return -1;  /* 无历史版本 */
    }

    /* 这里只是演示，实际需要保存完整的配置历史 */
    config->version.version--;

    return 0;
}

void http_config_update_result_free(ConfigUpdateResult *result) {
    if (!result) return;

    if (result->error_message) {
        free(result->error_message);
        result->error_message = NULL;
    }
}