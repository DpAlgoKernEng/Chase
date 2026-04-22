/**
 * @file    vhost.c
 * @brief   虚拟主机模块实现
 *
 * @details
 *          - 精确域名匹配
 *          - 通配符域名匹配（仅一级子域名）
 *          - 默认虚拟主机
 *
 * @author  minghui.liu
 * @date    2026-04-22
 */

#include "vhost.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========== 内部结构体定义 ========== */

struct VHostManager {
    VirtualHost **vhosts;
    int count;
    int capacity;
    VirtualHost *default_vhost;
};

/* ========== 辅助函数 ========== */

static char *strdup_safe(const char *s) {
    if (!s) return NULL;
    return strdup(s);
}

/* ========== VHostManager 实现 ========== */

VHostManager *vhost_manager_create(void) {
    VHostManager *manager = malloc(sizeof(VHostManager));
    if (!manager) return NULL;

    manager->capacity = 16;
    manager->vhosts = malloc(manager->capacity * sizeof(VirtualHost *));
    if (!manager->vhosts) {
        free(manager);
        return NULL;
    }

    manager->count = 0;
    manager->default_vhost = NULL;

    return manager;
}

void vhost_manager_destroy(VHostManager *manager) {
    if (!manager) return;

    /* 销毁所有虚拟主机 */
    for (int i = 0; i < manager->count; i++) {
        vhost_destroy(manager->vhosts[i]);
    }
    free(manager->vhosts);

    /* 默认虚拟主机不单独销毁（已在列表中或需要单独处理） */
    free(manager);
}

int vhost_manager_add(VHostManager *manager, VirtualHost *vhost) {
    if (!manager || !vhost) return -1;

    /* 扩容 */
    if (manager->count >= manager->capacity) {
        int new_cap = manager->capacity * 2;
        VirtualHost **new_vhosts = realloc(manager->vhosts, new_cap * sizeof(VirtualHost *));
        if (!new_vhosts) return -1;
        manager->vhosts = new_vhosts;
        manager->capacity = new_cap;
    }

    manager->vhosts[manager->count++] = vhost;

    /* 设置优先级：精确匹配优先级更高 */
    vhost->priority = vhost->is_wildcard ? 10 : 0;

    /* 第一个添加的虚拟主机作为默认 */
    if (manager->count == 1 && !manager->default_vhost) {
        manager->default_vhost = vhost;
    }

    return 0;
}

/* 通配符匹配：仅一级子域名 */
bool vhost_wildcard_match(const char *wildcard, const char *hostname) {
    if (!wildcard || !hostname) return false;

    /* 通配符格式：*.example.com */
    if (wildcard[0] != '*' || wildcard[1] != '.') return false;

    const char *wildcard_suffix = wildcard + 2;  /* 跳过 "*." */
    size_t suffix_len = strlen(wildcard_suffix);
    size_t hostname_len = strlen(hostname);

    /* hostname 必须比 suffix 长 */
    if (hostname_len <= suffix_len) return false;

    /* 检查 hostname 是否以 suffix 结尾 */
    const char *hostname_suffix = hostname + hostname_len - suffix_len;
    if (strcmp(hostname_suffix, wildcard_suffix) != 0) return false;

    /* 检查 hostname 中只有一个 '.' 在 suffix 前面（一级子域名） */
    /* 例如：*.example.com 匹配 sub.example.com，不匹配 sub.sub.example.com */
    const char *dot_pos = hostname;
    while (dot_pos < hostname_suffix) {
        if (*dot_pos == '.') {
            /* 检查是否是最后一个 '.'（即一级子域名） */
            /* hostname_suffix 前面的最后一个 '.' 位置 */
            const char *last_dot = hostname_suffix - 1;
            while (last_dot > hostname && *last_dot != '.') {
                last_dot--;
            }
            /* 如果只有一个 '.' 在 suffix 前面，则是一级子域名 */
            if (last_dot < hostname_suffix && last_dot >= hostname) {
                return true;  /* 一级子域名匹配 */
            }
            break;
        }
        dot_pos++;
    }

    /* 如果没有 '.' 在 suffix 前面，也匹配（例如 example.com 匹配 *.com 不应该，但这是边界情况） */
    /* 实际上我们要求至少有一个子域名 */
    return false;
}

VirtualHost *vhost_manager_match(VHostManager *manager, const char *hostname) {
    if (!manager || !hostname) {
        return manager ? manager->default_vhost : NULL;
    }

    VirtualHost *best_match = NULL;
    int best_priority = -1;

    /* 遍历所有虚拟主机 */
    for (int i = 0; i < manager->count; i++) {
        VirtualHost *vhost = manager->vhosts[i];

        if (vhost->is_wildcard) {
            /* 通配符匹配 */
            if (vhost_wildcard_match(vhost->hostname, hostname)) {
                if (vhost->priority > best_priority) {
                    best_match = vhost;
                    best_priority = vhost->priority;
                }
            }
        } else {
            /* 精确匹配 */
            if (strcmp(vhost->hostname, hostname) == 0) {
                /* 精确匹配优先级最高 */
                return vhost;
            }
        }
    }

    /* 返回最佳匹配或默认 */
    return best_match ? best_match : manager->default_vhost;
}

int vhost_manager_set_default(VHostManager *manager, VirtualHost *vhost) {
    if (!manager || !vhost) return -1;
    manager->default_vhost = vhost;
    return 0;
}

VirtualHost *vhost_manager_get_default(VHostManager *manager) {
    if (!manager) return NULL;
    return manager->default_vhost;
}

int vhost_manager_count(VHostManager *manager) {
    if (!manager) return 0;
    return manager->count;
}

/* ========== VirtualHost 实现 ========== */

VirtualHost *vhost_create(const char *hostname, Router *router, SslServerCtx *ssl_ctx) {
    if (!hostname || !router) return NULL;

    VirtualHost *vhost = malloc(sizeof(VirtualHost));
    if (!vhost) return NULL;

    vhost->hostname = strdup_safe(hostname);
    if (!vhost->hostname) {
        free(vhost);
        return NULL;
    }

    vhost->router = router;
    vhost->ssl_ctx = ssl_ctx;
    vhost->is_wildcard = vhost_is_wildcard(hostname);
    vhost->priority = vhost->is_wildcard ? 10 : 0;

    return vhost;
}

void vhost_destroy(VirtualHost *vhost) {
    if (!vhost) return;

    free(vhost->hostname);
    /* router 和 ssl_ctx 由调用者管理 */
    free(vhost);
}

bool vhost_is_wildcard(const char *hostname) {
    if (!hostname) return false;
    return (hostname[0] == '*' && hostname[1] == '.');
}