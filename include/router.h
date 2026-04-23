/**
 * @file    router.h
 * @brief   URL 路由器，支持精确匹配、前缀匹配和正则匹配
 *
 * @details
 *          - 精确匹配：完整路径匹配
 *          - 前缀匹配：路径前缀匹配
 *          - 正则匹配：POSIX 正则表达式匹配（Phase 5）
 *          - 捕获组：正则匹配支持捕获组提取（Phase 5）
 *          - 方法过滤：支持 GET/POST 等 HTTP 方法掩码
 *          - 优先级排序：高优先级路由优先匹配
 *          - 冲突检测：检测路由冲突（Phase 5）
 *
 * @layer   Core Layer
 *
 * @depends http_parser
 * @usedby  server, examples
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#ifndef CHASE_ROUTER_H
#define CHASE_ROUTER_H

#include <stdint.h>
#include <stdbool.h>
#include "http_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 路由匹配类型 */
typedef enum {
    ROUTER_MATCH_EXACT,   /* 精确匹配 */
    ROUTER_MATCH_PREFIX,  /* 前缀匹配 */
    ROUTER_MATCH_REGEX    /* 正则匹配 */
} RouteMatchType;

/* HTTP 方法掩码 */
#define METHOD_GET    (1 << HTTP_GET)
#define METHOD_POST   (1 << HTTP_POST)
#define METHOD_PUT    (1 << HTTP_PUT)
#define METHOD_DELETE (1 << HTTP_DELETE)
#define METHOD_HEAD   (1 << HTTP_HEAD)
#define METHOD_ALL    0xFF

/* 优先级定义 */
#define PRIORITY_HIGH    100
#define PRIORITY_NORMAL  50
#define PRIORITY_LOW     10

/* Phase 5: 冲突处理策略 */
typedef enum {
    ROUTER_CONFLICT_WARN,    /* 警告但允许添加 */
    ROUTER_CONFLICT_ERROR,   /* 拒绝添加冲突路由 */
    ROUTER_CONFLICT_OVERRIDE /* 覆盖已有路由 */
} RouterConflictPolicy;

/* Phase 5: 正则匹配结果（捕获组） */
typedef struct RegexMatchResult {
    char **groups;       /* 捕获组字符串数组 */
    int group_count;     /* 捕获组数量 */
    int *group_starts;   /* 每组起始位置 */
    int *group_ends;     /* 每组结束位置 */
} RegexMatchResult;

/* 路由处理器回调 */
typedef void (*RouteHandler)(HttpRequest *req, void *resp, void *user_data);

/* Route 结构体 */
typedef struct Route {
    RouteMatchType type;
    char *pattern;
    RouteHandler handler;
    void *user_data;
    int methods;        /* 方法掩码 */
    int priority;
    /* Phase 5: 正则相关字段 */
    bool regex_compiled; /* 正则是否已编译 */
    void *regex_cache;  /* 编译后的正则缓存（regex_t*） */
} Route;

/* Router 结构体（不透明指针） */
typedef struct Router Router;

/**
 * 创建路由器
 * @return Router 指针，失败返回 NULL
 */
Router *router_create(void);

/**
 * 销毁路由器
 * @param router Router 挀针
 */
void router_destroy(Router *router);

/**
 * 添加路由
 * @param router Router 指针
 * @param route Route 挀针
 * @return 0 成功，-1 失败
 */
int router_add_route(Router *router, Route *route);

/**
 * 添加路由（扩展参数）
 * @param router Router 挀针
 * @param route Route 挀针
 * @param priority 优先级
 * @return 0 成功，-1 失败
 */
int router_add_route_ex(Router *router, Route *route, int priority);

/**
 * 匹配路由
 * @param router Router 挀针
 * @param path 路径
 * @param method HTTP 方法
 * @return Route 挀针，未匹配返回 NULL
 */
Route *router_match(Router *router, const char *path, HttpMethod method);

/**
 * 创建 Route
 * @param type 匹配类型
 * @param pattern 模式
 * @param handler 处理器
 * @param user_data 用户数据
 * @return Route 挀针，失败返回 NULL
 */
Route *route_create(RouteMatchType type, const char *pattern,
                    RouteHandler handler, void *user_data);

/**
 * 销毁 Route
 * @param route Route 挀针
 */
void route_destroy(Route *route);

/* ========== Phase 5: 正则匹配 API ========== */

/**
 * 添加正则路由
 * @param router Router 指针
 * @param pattern 正则表达式模式
 * @param handler 处理器
 * @param user_data 用户数据
 * @param methods 方法掩码
 * @param priority 优先级
 * @return 0 成功，-1 失败
 */
int router_add_regex_route(Router *router, const char *pattern,
                           RouteHandler handler, void *user_data,
                           int methods, int priority);

/**
 * 匹配路由（带正则捕获组）
 * @param router Router 指针
 * @param path 路径
 * @param method HTTP 方法
 * @param result 输出：正则匹配结果（可选）
 * @return Route 指针，未匹配返回 NULL
 */
Route *router_match_ex(Router *router, const char *path, HttpMethod method,
                       RegexMatchResult *result);

/**
 * 检测路由冲突
 * @param router Router 指针
 * @param route 新路由
 * @return 冲突路由数量，0 无冲突
 */
int router_detect_conflicts(Router *router, Route *route);

/**
 * 设置冲突处理策略
 * @param router Router 指针
 * @param policy 冲突策略
 */
void router_set_conflict_policy(Router *router, RouterConflictPolicy policy);

/**
 * 释放正则匹配结果
 * @param result RegexMatchResult 指针
 */
void regex_match_result_free(RegexMatchResult *result);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_ROUTER_H */