/**
 * @file    security.c
 * @brief   安全模块实现
 *
 * @details
 *          - 分片哈希表（减少锁竞争）
 *          - IP 连接计数和速率追踪
 *          - Slowloris 检测（字节速率）
 *          - 封禁机制（过期自动解封）
 *
 * @layer   Core Layer
 *
 * @depends timer, error
 * @usedby  server, worker
 *
 * @author  minghui.liu
 * @date    2026-04-23
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <arpa/inet.h>
#include <time.h>

#include "security.h"
#include "error.h"

/* IP 条目 */
typedef struct IpEntry {
    IpAddress ip;
    int connection_count;
    int request_count;
    uint64_t last_request_time;
    uint64_t bytes_received;
    uint64_t block_expire_time;
    bool is_blocked;
    struct IpEntry *next;
} IpEntry;

/* 分片哈希表 */
typedef struct IpShard {
    IpEntry **buckets;
    int bucket_count;
    pthread_mutex_t lock;
    int entry_count;
} IpShard;

/* Security 结构 */
struct Security {
    SecurityConfig config;
    IpShard **shards;              /* 分片数组（指针数组） */
    uint64_t current_time_ms;
};

/* ============== 哈希函数 ============== */

static uint32_t ip_hash(const IpAddress *ip) {
    uint32_t hash = 0;
    if (ip->is_ipv6) {
        /* IPv6: 使用所有 16 字节 */
        for (int i = 0; i < 16; i++) {
            hash = hash * 31 + ip->data[i];
        }
    } else {
        /* IPv4: 使用前 4 字节 */
        hash = ip->data[0] | (ip->data[1] << 8) | (ip->data[2] << 16) | (ip->data[3] << 24);
    }
    return hash;
}

static int get_shard_index(Security *security, const IpAddress *ip) {
    uint32_t hash = ip_hash(ip);
    return hash % security->config.shard_count;
}

static int get_bucket_index(IpShard *shard, const IpAddress *ip) {
    uint32_t hash = ip_hash(ip);
    return hash % shard->bucket_count;
}

/* ============== 分片哈希表操作 ============== */

static IpShard *shard_create(int bucket_count) {
    IpShard *shard = malloc(sizeof(IpShard));
    if (!shard) return NULL;

    shard->buckets = malloc(bucket_count * sizeof(IpEntry *));
    if (!shard->buckets) {
        free(shard);
        return NULL;
    }

    for (int i = 0; i < bucket_count; i++) {
        shard->buckets[i] = NULL;
    }

    shard->bucket_count = bucket_count;
    shard->entry_count = 0;
    pthread_mutex_init(&shard->lock, NULL);

    return shard;
}

static void shard_destroy(IpShard *shard) {
    if (!shard) return;

    pthread_mutex_lock(&shard->lock);

    for (int i = 0; i < shard->bucket_count; i++) {
        IpEntry *entry = shard->buckets[i];
        while (entry) {
            IpEntry *next = entry->next;
            free(entry);
            entry = next;
        }
    }

    free(shard->buckets);

    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_destroy(&shard->lock);
    free(shard);
}

static bool ip_equal(const IpAddress *a, const IpAddress *b) {
    if (a->is_ipv6 != b->is_ipv6) return false;
    if (a->is_ipv6) {
        return memcmp(a->data, b->data, 16) == 0;
    } else {
        return memcmp(a->data, b->data, 4) == 0;
    }
}

static IpEntry *shard_find_entry(IpShard *shard, const IpAddress *ip) {
    int bucket = get_bucket_index(shard, ip);
    IpEntry *entry = shard->buckets[bucket];

    while (entry) {
        if (ip_equal(&entry->ip, ip)) {
            return entry;
        }
        entry = entry->next;
    }

    return NULL;
}

static IpEntry *shard_create_entry(IpShard *shard, const IpAddress *ip) {
    IpEntry *entry = malloc(sizeof(IpEntry));
    if (!entry) return NULL;

    entry->ip = *ip;
    entry->connection_count = 0;
    entry->request_count = 0;
    entry->last_request_time = 0;
    entry->bytes_received = 0;
    entry->block_expire_time = 0;
    entry->is_blocked = false;
    entry->next = NULL;

    int bucket = get_bucket_index(shard, ip);
    entry->next = shard->buckets[bucket];
    shard->buckets[bucket] = entry;
    shard->entry_count++;

    return entry;
}

static void shard_remove_entry(IpShard *shard, const IpAddress *ip) {
    int bucket = get_bucket_index(shard, ip);
    IpEntry **pp = &shard->buckets[bucket];

    while (*pp) {
        if (ip_equal(&(*pp)->ip, ip)) {
            IpEntry *entry = *pp;
            *pp = entry->next;
            free(entry);
            shard->entry_count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ============== 时间获取 ============== */

static uint64_t get_current_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ============== Security API 实现 ============== */

Security *security_create(const SecurityConfig *config) {
    if (!config) return NULL;

    Security *security = malloc(sizeof(Security));
    if (!security) return NULL;

    /* 复制配置 */
    security->config = *config;

    if (security->config.shard_count <= 0) {
        security->config.shard_count = SECURITY_DEFAULT_SHARD_COUNT;
    }
    if (security->config.max_connections_per_ip <= 0) {
        security->config.max_connections_per_ip = SECURITY_DEFAULT_MAX_CONN_PER_IP;
    }
    if (security->config.max_requests_per_second <= 0) {
        security->config.max_requests_per_second = SECURITY_DEFAULT_MAX_REQ_PER_SEC;
    }
    if (security->config.min_request_rate <= 0) {
        security->config.min_request_rate = SECURITY_DEFAULT_MIN_REQ_RATE;
    }
    if (security->config.slowloris_timeout_ms <= 0) {
        security->config.slowloris_timeout_ms = SECURITY_DEFAULT_SLOWLORIS_TIMEOUT;
    }
    if (security->config.block_duration_ms <= 0) {
        security->config.block_duration_ms = SECURITY_DEFAULT_BLOCK_DURATION;
    }

    /* 创建分片 */
    security->shards = malloc(security->config.shard_count * sizeof(IpShard *));
    if (!security->shards) {
        free(security);
        return NULL;
    }

    int bucket_per_shard = 1024;  /* 每分片 1024 个桶 */
    for (int i = 0; i < security->config.shard_count; i++) {
        security->shards[i] = shard_create(bucket_per_shard);
        if (!security->shards[i]) {
            /* 清理已创建的分片 */
            for (int j = 0; j < i; j++) {
                shard_destroy(security->shards[j]);
            }
            free(security->shards);
            free(security);
            return NULL;
        }
    }

    security->current_time_ms = get_current_ms();

    return security;
}

void security_destroy(Security *security) {
    if (!security) return;

    for (int i = 0; i < security->config.shard_count; i++) {
        shard_destroy(security->shards[i]);
    }

    free(security->shards);
    free(security);
}

SecurityResult security_check_connection(Security *security, const IpAddress *ip) {
    if (!security || !ip) return SECURITY_INTERNAL_ERROR;

    security->current_time_ms = get_current_ms();

    int shard_idx = get_shard_index(security, ip);
    IpShard *shard = security->shards[shard_idx];

    pthread_mutex_lock(&shard->lock);

    IpEntry *entry = shard_find_entry(shard, ip);

    /* 检查是否被封禁 */
    if (entry && entry->is_blocked) {
        if (security->current_time_ms < entry->block_expire_time) {
            pthread_mutex_unlock(&shard->lock);
            return SECURITY_BLOCKED_IP;
        }
        /* 封禁已过期 */
        entry->is_blocked = false;
    }

    /* 检查连接数 */
    int conn_count = entry ? entry->connection_count : 0;
    if (conn_count >= security->config.max_connections_per_ip) {
        pthread_mutex_unlock(&shard->lock);
        return SECURITY_TOO_MANY_CONN;
    }

    pthread_mutex_unlock(&shard->lock);
    return SECURITY_OK;
}

SecurityResult security_add_connection(Security *security, const IpAddress *ip) {
    if (!security || !ip) return SECURITY_INTERNAL_ERROR;

    int shard_idx = get_shard_index(security, ip);
    IpShard *shard = security->shards[shard_idx];

    pthread_mutex_lock(&shard->lock);

    IpEntry *entry = shard_find_entry(shard, ip);
    if (!entry) {
        entry = shard_create_entry(shard, ip);
        if (!entry) {
            pthread_mutex_unlock(&shard->lock);
            return SECURITY_INTERNAL_ERROR;
        }
    }

    entry->connection_count++;

    pthread_mutex_unlock(&shard->lock);
    return SECURITY_OK;
}

void security_remove_connection(Security *security, const IpAddress *ip) {
    if (!security || !ip) return;

    int shard_idx = get_shard_index(security, ip);
    IpShard *shard = security->shards[shard_idx];

    pthread_mutex_lock(&shard->lock);

    IpEntry *entry = shard_find_entry(shard, ip);
    if (entry) {
        entry->connection_count--;
        if (entry->connection_count <= 0 && entry->request_count <= 0 && !entry->is_blocked) {
            /* 无活动且未封禁，可以移除 */
            shard_remove_entry(shard, ip);
        }
    }

    pthread_mutex_unlock(&shard->lock);
}

SecurityResult security_check_request_rate(Security *security, const IpAddress *ip,
                                           size_t bytes_received) {
    if (!security || !ip) return SECURITY_INTERNAL_ERROR;

    security->current_time_ms = get_current_ms();

    int shard_idx = get_shard_index(security, ip);
    IpShard *shard = security->shards[shard_idx];

    pthread_mutex_lock(&shard->lock);

    IpEntry *entry = shard_find_entry(shard, ip);
    if (!entry) {
        entry = shard_create_entry(shard, ip);
        if (!entry) {
            pthread_mutex_unlock(&shard->lock);
            return SECURITY_INTERNAL_ERROR;
        }
    }

    /* 检查封禁状态 */
    if (entry->is_blocked) {
        if (security->current_time_ms < entry->block_expire_time) {
            pthread_mutex_unlock(&shard->lock);
            return SECURITY_BLOCKED_IP;
        }
        /* 封禁已过期，重置状态 */
        entry->is_blocked = false;
        entry->request_count = 0;
        entry->bytes_received = 0;
        entry->last_request_time = 0;
    }

    /* 更新统计 */
    uint64_t elapsed_ms = security->current_time_ms - entry->last_request_time;
    entry->last_request_time = security->current_time_ms;
    entry->request_count++;
    entry->bytes_received += bytes_received;

    /* 速率限制检查 */
    if (elapsed_ms < 1000) {
        /* 在同一秒内，检查请求计数 */
        if (entry->request_count > security->config.max_requests_per_second) {
            /* 自动封禁 */
            entry->is_blocked = true;
            entry->block_expire_time = security->current_time_ms + security->config.block_duration_ms;
            pthread_mutex_unlock(&shard->lock);
            return SECURITY_RATE_LIMITED;
        }
    } else {
        /* 超过1秒，重置计数 */
        entry->request_count = 1;
    }

    /* Slowloris 检测 - 仅在已有历史数据时检查 */
    if (bytes_received > 0 && elapsed_ms > 0 && elapsed_ms < 60000) {
        /* 只检查合理时间范围内的请求（1分钟内），避免新条目触发误报 */
        double bytes_per_sec = (double)bytes_received * 1000.0 / elapsed_ms;
        if (bytes_per_sec < security->config.min_request_rate &&
            elapsed_ms > (uint64_t)security->config.slowloris_timeout_ms) {
            /* Slowloris 检测 */
            entry->is_blocked = true;
            entry->block_expire_time = security->current_time_ms + security->config.block_duration_ms;
            pthread_mutex_unlock(&shard->lock);
            return SECURITY_SLOWLORIS;
        }
    }

    pthread_mutex_unlock(&shard->lock);
    return SECURITY_OK;
}

int security_block_ip(Security *security, const IpAddress *ip, int duration_ms) {
    if (!security || !ip) return -1;

    security->current_time_ms = get_current_ms();

    int duration = duration_ms > 0 ? duration_ms : security->config.block_duration_ms;

    int shard_idx = get_shard_index(security, ip);
    IpShard *shard = security->shards[shard_idx];

    pthread_mutex_lock(&shard->lock);

    IpEntry *entry = shard_find_entry(shard, ip);
    if (!entry) {
        entry = shard_create_entry(shard, ip);
        if (!entry) {
            pthread_mutex_unlock(&shard->lock);
            return -1;
        }
    }

    entry->is_blocked = true;
    entry->block_expire_time = security->current_time_ms + duration;

    pthread_mutex_unlock(&shard->lock);
    return 0;
}

void security_unblock_ip(Security *security, const IpAddress *ip) {
    if (!security || !ip) return;

    int shard_idx = get_shard_index(security, ip);
    IpShard *shard = security->shards[shard_idx];

    pthread_mutex_lock(&shard->lock);

    IpEntry *entry = shard_find_entry(shard, ip);
    if (entry) {
        entry->is_blocked = false;
        entry->block_expire_time = 0;
    }

    pthread_mutex_unlock(&shard->lock);
}

bool security_is_blocked(Security *security, const IpAddress *ip) {
    if (!security || !ip) return false;

    security->current_time_ms = get_current_ms();

    int shard_idx = get_shard_index(security, ip);
    IpShard *shard = security->shards[shard_idx];

    pthread_mutex_lock(&shard->lock);

    IpEntry *entry = shard_find_entry(shard, ip);
    bool blocked = false;

    if (entry && entry->is_blocked) {
        blocked = (security->current_time_ms < entry->block_expire_time);
    }

    pthread_mutex_unlock(&shard->lock);
    return blocked;
}

int security_get_ip_stats(Security *security, const IpAddress *ip, IpStats *stats) {
    if (!security || !ip || !stats) return -1;

    security->current_time_ms = get_current_ms();

    int shard_idx = get_shard_index(security, ip);
    IpShard *shard = security->shards[shard_idx];

    pthread_mutex_lock(&shard->lock);

    IpEntry *entry = shard_find_entry(shard, ip);
    if (!entry) {
        pthread_mutex_unlock(&shard->lock);
        return -1;
    }

    stats->connection_count = entry->connection_count;
    stats->request_count = entry->request_count;
    stats->bytes_received = entry->bytes_received;
    stats->last_request_time = entry->last_request_time;
    stats->is_blocked = entry->is_blocked &&
                        (security->current_time_ms < entry->block_expire_time);
    stats->block_expire_time = entry->block_expire_time;

    pthread_mutex_unlock(&shard->lock);
    return 0;
}

int security_parse_ip(const struct sockaddr *addr, IpAddress *ip) {
    if (!addr || !ip) return -1;

    memset(ip, 0, sizeof(IpAddress));

    if (addr->sa_family == AF_INET) {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)addr;
        memcpy(ip->data, &addr_in->sin_addr.s_addr, 4);
        ip->is_ipv6 = false;
        return 0;
    } else if (addr->sa_family == AF_INET6) {
        struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)addr;
        memcpy(ip->data, addr_in6->sin6_addr.s6_addr, 16);
        ip->is_ipv6 = true;
        return 0;
    }

    return -1;
}

int security_ip_to_string(const IpAddress *ip, char *buffer, size_t size) {
    if (!ip || !buffer || size < 16) return -1;

    if (ip->is_ipv6) {
        struct in6_addr addr;
        memcpy(&addr, ip->data, 16);
        inet_ntop(AF_INET6, &addr, buffer, size);
    } else {
        struct in_addr addr;
        memcpy(&addr, ip->data, 4);
        inet_ntop(AF_INET, &addr, buffer, size);
    }

    return strlen(buffer);
}

int security_string_to_ip(const char *str, IpAddress *ip) {
    if (!str || !ip) return -1;

    memset(ip, 0, sizeof(IpAddress));

    /* 先尝试 IPv4 */
    struct in_addr addr4;
    if (inet_pton(AF_INET, str, &addr4) == 1) {
        memcpy(ip->data, &addr4.s_addr, 4);
        ip->is_ipv6 = false;
        return 0;
    }

    /* 尝试 IPv6 */
    struct in6_addr addr6;
    if (inet_pton(AF_INET6, str, &addr6) == 1) {
        memcpy(ip->data, addr6.s6_addr, 16);
        ip->is_ipv6 = true;
        return 0;
    }

    return -1;
}

void security_cleanup(Security *security) {
    if (!security) return;

    security->current_time_ms = get_current_ms();

    for (int i = 0; i < security->config.shard_count; i++) {
        IpShard *shard = security->shards[i];
        pthread_mutex_lock(&shard->lock);

        for (int b = 0; b < shard->bucket_count; b++) {
            IpEntry **pp = &shard->buckets[b];
            while (*pp) {
                IpEntry *entry = *pp;

                /* 清理过期封禁 */
                if (entry->is_blocked &&
                    security->current_time_ms >= entry->block_expire_time) {
                    entry->is_blocked = false;
                    entry->block_expire_time = 0;
                }

                /* 清理无活动条目 */
                if (entry->connection_count <= 0 &&
                    entry->request_count <= 0 &&
                    !entry->is_blocked) {
                    *pp = entry->next;
                    free(entry);
                    shard->entry_count--;
                } else {
                    pp = &entry->next;
                }
            }
        }

        pthread_mutex_unlock(&shard->lock);
    }
}

void security_get_summary(Security *security, int *total_tracked, int *total_blocked) {
    if (!security) return;

    int tracked = 0;
    int blocked = 0;

    security->current_time_ms = get_current_ms();

    for (int i = 0; i < security->config.shard_count; i++) {
        IpShard *shard = security->shards[i];
        pthread_mutex_lock(&shard->lock);

        tracked += shard->entry_count;

        for (int b = 0; b < shard->bucket_count; b++) {
            IpEntry *entry = shard->buckets[b];
            while (entry) {
                if (entry->is_blocked &&
                    security->current_time_ms < entry->block_expire_time) {
                    blocked++;
                }
                entry = entry->next;
            }
        }

        pthread_mutex_unlock(&shard->lock);
    }

    if (total_tracked) *total_tracked = tracked;
    if (total_blocked) *total_blocked = blocked;
}