/**
 * @file    vhost.h
 * @brief   虚拟主机模块，支持精确域名和通配符匹配
 *
 * @details
 *          - VirtualHost 结构体定义（域名、路由器、SSL 配置）
 *          - VHostManager 管理多个虚拟主机
 *          - 精确域名匹配（example.com）
 *          - 通配符域名匹配（*.example.com，仅一级子域名）
 *          - 默认虚拟主机
 *
 * @layer   Core Layer
 *
 * @depends router, ssl_wrap
 * @usedby  server
 *
 * @author  minghui.liu
 * @date    2026-04-22
 */

#ifndef CHASE_VHOST_H
#define CHASE_VHOST_H

#include "router.h"
#include "ssl_wrap.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 虚拟主机结构体 */
typedef struct VirtualHost {
    char *hostname;           /* 域名（精确或通配符） */
    Router *router;           /* 路由器 */
    SslServerCtx *ssl_ctx;    /* SSL 上下文（可选，HTTPS） */
    bool is_wildcard;         /* 是否为通配符域名 */
    int priority;             /* 匹配优先级（精确优先） */
} VirtualHost;

/* 虚拟主机管理器 */
typedef struct VHostManager VHostManager;

/**
 * 创建虚拟主机管理器
 * @return VHostManager 指针，失败返回 NULL
 */
VHostManager *vhost_manager_create(void);

/**
 * 销毁虚拟主机管理器
 * @param manager VHostManager 指针
 */
void vhost_manager_destroy(VHostManager *manager);

/**
 * 创建虚拟主机
 * @param hostname 域名（精确或通配符如 "*.example.com"）
 * @param router 路由器
 * @param ssl_ctx SSL 上下文（可选）
 * @return VirtualHost 指针，失败返回 NULL
 */
VirtualHost *vhost_create(const char *hostname, Router *router, SslServerCtx *ssl_ctx);

/**
 * 销毁虚拟主机
 * @param vhost VirtualHost 指针
 */
void vhost_destroy(VirtualHost *vhost);

/**
 * 添加虚拟主机到管理器
 * @param manager VHostManager 指针
 * @param vhost VirtualHost 指针
 * @return 0 成功，负值错误
 */
int vhost_manager_add(VHostManager *manager, VirtualHost *vhost);

/**
 * 根据域名匹配虚拟主机
 * @param manager VHostManager 指针
 * @param hostname 域名
 * @return VirtualHost 指针，未找到返回默认虚拟主机或 NULL
 */
VirtualHost *vhost_manager_match(VHostManager *manager, const char *hostname);

/**
 * 设置默认虚拟主机
 * @param manager VHostManager 指针
 * @param vhost 默认虚拟主机
 * @return 0 成功，负值错误
 */
int vhost_manager_set_default(VHostManager *manager, VirtualHost *vhost);

/**
 * 获取默认虚拟主机
 * @param manager VHostManager 指针
 * @return VirtualHost 指针
 */
VirtualHost *vhost_manager_get_default(VHostManager *manager);

/**
 * 获取虚拟主机数量
 * @param manager VHostManager 指针
 * @return 虚拟主机数量
 */
int vhost_manager_count(VHostManager *manager);

/**
 * 检查域名是否为通配符格式
 * @param hostname 域名
 * @return true 通配符，false 精确域名
 */
bool vhost_is_wildcard(const char *hostname);

/**
 * 检查域名是否匹配通配符
 * @param wildcard 通配符域名（如 "*.example.com"）
 * @param hostname 待匹配域名
 * @return true 匹配，false 不匹配
 */
bool vhost_wildcard_match(const char *wildcard, const char *hostname);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_VHOST_H */