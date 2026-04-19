#ifndef CHASE_ROUTER_H
#define CHASE_ROUTER_H

#include <stdint.h>
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

#ifdef __cplusplus
}
#endif

#endif /* CHASE_ROUTER_H */