/**
 * @file    router.c
 * @brief   URL 路由器实现
 *
 * @details
 *          - 精确匹配和前缀匹配
 *          - 正则匹配（POSIX regex）
 *          - 捕获组提取
 *          - 方法过滤
 *          - 优先级排序
 *          - 冲突检测
 *
 * @layer   Core Layer
 *
 * @depends http_parser
 * @usedby  server, examples
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "router.h"
#include <stdlib.h>
#include <string.h>
#include <regex.h>  /* Phase 5: POSIX regex */

/* Router 结构体 */
struct Router {
    Route **routes;
    int route_count;
    int route_capacity;
    RouterConflictPolicy conflict_policy;  /* Phase 5: 冲突策略 */
};

/* ========== 公共 API 实现 ========== */

Router *router_create(void) {
    Router *router = malloc(sizeof(Router));
    if (!router) return NULL;

    router->routes = malloc(16 * sizeof(Route *));
    router->route_count = 0;
    router->route_capacity = 16;
    router->conflict_policy = ROUTER_CONFLICT_WARN;  /* Phase 5: 默认警告 */

    if (!router->routes) {
        free(router);
        return NULL;
    }

    return router;
}

void router_destroy(Router *router) {
    if (!router) return;

    for (int i = 0; i < router->route_count; i++) {
        route_destroy(router->routes[i]);
    }

    free(router->routes);
    free(router);
}

int router_add_route(Router *router, Route *route) {
    return router_add_route_ex(router, route, PRIORITY_NORMAL);
}

int router_add_route_ex(Router *router, Route *route, int priority) {
    if (!router || !route) return -1;

    route->priority = priority;

    if (router->route_count >= router->route_capacity) {
        int new_cap = router->route_capacity * 2;
        Route **new_routes = realloc(router->routes, new_cap * sizeof(Route *));
        if (!new_routes) return -1;
        router->routes = new_routes;
        router->route_capacity = new_cap;
    }

    router->routes[router->route_count++] = route;
    return 0;
}

Route *router_match(Router *router, const char *path, HttpMethod method) {
    return router_match_ex(router, path, method, NULL);
}

Route *router_match_ex(Router *router, const char *path, HttpMethod method,
                       RegexMatchResult *result) {
    if (!router || !path) return NULL;

    int method_mask = 1 << method;

    /* 按优先级搜索 */
    Route *best_match = NULL;
    int best_priority = -1;
    regex_t *best_regex = NULL;
    regmatch_t *best_matches = NULL;

    for (int i = 0; i < router->route_count; i++) {
        Route *route = router->routes[i];

        /* 检查方法匹配 */
        if (!(route->methods & method_mask)) {
            continue;
        }

        /* 根据匹配类型检查 */
        bool matched = false;
        regmatch_t *matches = NULL;

        switch (route->type) {
        case ROUTER_MATCH_EXACT:
            matched = (strcmp(route->pattern, path) == 0);
            break;
        case ROUTER_MATCH_PREFIX:
            matched = (strncmp(route->pattern, path, strlen(route->pattern)) == 0);
            break;
        case ROUTER_MATCH_REGEX:
            /* Phase 5: 正则匹配 */
            if (!route->regex_compiled) {
                /* 编译正则表达式 */
                regex_t *regex = malloc(sizeof(regex_t));
                if (!regex) break;

                int ret = regcomp(regex, route->pattern, REG_EXTENDED | REG_ICASE);
                if (ret != 0) {
                    free(regex);
                    break;
                }

                route->regex_cache = regex;
                route->regex_compiled = true;
            }

            regex_t *regex = (regex_t *)route->regex_cache;
            size_t nmatch = regex->re_nsub + 1;  /* 捕获组数量 + 全匹配 */
            matches = malloc(nmatch * sizeof(regmatch_t));
            if (!matches) break;

            int ret = regexec(regex, path, nmatch, matches, 0);
            matched = (ret == 0);
            break;
        }

        if (matched && route->priority > best_priority) {
            /* 释放之前的最佳匹配 */
            if (best_matches) {
                free(best_matches);
            }
            best_match = route;
            best_priority = route->priority;
            best_matches = matches;
            best_regex = (regex_t *)route->regex_cache;
            matches = NULL;  /* 已转移给 best_matches */
        } else if (matches) {
            free(matches);  /* 释放未使用的匹配结果 */
        }
    }

    /* 如果有正则匹配结果且需要返回，提取捕获组 */
    if (best_match && best_match->type == ROUTER_MATCH_REGEX && result && best_matches) {
        regex_t *regex = best_regex;
        int ngroups = regex->re_nsub + 1;

        result->group_count = ngroups;
        result->groups = malloc(ngroups * sizeof(char *));
        result->group_starts = malloc(ngroups * sizeof(int));
        result->group_ends = malloc(ngroups * sizeof(int));

        if (result->groups && result->group_starts && result->group_ends) {
            for (int g = 0; g < ngroups; g++) {
                if (best_matches[g].rm_so >= 0 && best_matches[g].rm_eo >= 0) {
                    int len = best_matches[g].rm_eo - best_matches[g].rm_so;
                    result->groups[g] = malloc(len + 1);
                    if (result->groups[g]) {
                        memcpy(result->groups[g], path + best_matches[g].rm_so, len);
                        result->groups[g][len] = '\0';
                    }
                    result->group_starts[g] = best_matches[g].rm_so;
                    result->group_ends[g] = best_matches[g].rm_eo;
                } else {
                    result->groups[g] = NULL;
                    result->group_starts[g] = -1;
                    result->group_ends[g] = -1;
                }
            }
        }

        free(best_matches);
    } else if (best_matches) {
        free(best_matches);
    }

    return best_match;
}

Route *route_create(RouteMatchType type, const char *pattern,
                    RouteHandler handler, void *user_data) {
    Route *route = malloc(sizeof(Route));
    if (!route) return NULL;

    route->type = type;
    route->pattern = strdup(pattern);
    route->handler = handler;
    route->user_data = user_data;
    route->methods = METHOD_ALL;  /* 默认支持所有方法 */
    route->priority = PRIORITY_NORMAL;
    /* Phase 5: 正则字段初始化 */
    route->regex_compiled = false;
    route->regex_cache = NULL;

    if (!route->pattern) {
        free(route);
        return NULL;
    }

    return route;
}

void route_destroy(Route *route) {
    if (!route) return;

    free(route->pattern);

    /* Phase 5: 释放正则缓存 */
    if (route->regex_compiled && route->regex_cache) {
        regfree((regex_t *)route->regex_cache);
        free(route->regex_cache);
    }

    free(route);
}

/* ========== Phase 5: 正则匹配 API 实现 ========== */

int router_add_regex_route(Router *router, const char *pattern,
                           RouteHandler handler, void *user_data,
                           int methods, int priority) {
    if (!router || !pattern) return -1;

    Route *route = route_create(ROUTER_MATCH_REGEX, pattern, handler, user_data);
    if (!route) return -1;

    route->methods = methods;

    /* 检查冲突 */
    int conflicts = router_detect_conflicts(router, route);
    if (conflicts > 0) {
        switch (router->conflict_policy) {
        case ROUTER_CONFLICT_ERROR:
            route_destroy(route);
            return -1;
        case ROUTER_CONFLICT_OVERRIDE:
            /* 覆盖策略：移除冲突路由 */
            for (int i = 0; i < router->route_count; i++) {
                Route *existing = router->routes[i];
                if (existing->type == ROUTER_MATCH_REGEX &&
                    strcmp(existing->pattern, pattern) == 0 &&
                    (existing->methods & methods)) {
                    route_destroy(existing);
                    router->routes[i] = route;
                    route->priority = priority;
                    return 0;
                }
            }
            break;
        case ROUTER_CONFLICT_WARN:
            /* 警告但继续添加 */
            break;
        }
    }

    return router_add_route_ex(router, route, priority);
}

int router_detect_conflicts(Router *router, Route *route) {
    if (!router || !route) return 0;

    int conflicts = 0;

    for (int i = 0; i < router->route_count; i++) {
        Route *existing = router->routes[i];

        /* 检查是否可能冲突 */
        /* 1. 相同模式和类型 */
        if (existing->type == route->type &&
            strcmp(existing->pattern, route->pattern) == 0 &&
            (existing->methods & route->methods)) {
            conflicts++;
        }

        /* 2. 正则路由可能匹配相同路径（需要实际测试） */
        if (existing->type == ROUTER_MATCH_REGEX || route->type == ROUTER_MATCH_REGEX) {
            /* 对于正则路由，我们保守地认为可能冲突 */
            /* 完全准确的检测需要编译并测试多个路径 */
            /* 这里简化处理：相同优先级的正则路由视为潜在冲突 */
            if (existing->priority == route->priority &&
                (existing->methods & route->methods)) {
                /* 可能有冲突，但不增加计数（仅警告） */
            }
        }
    }

    return conflicts;
}

void router_set_conflict_policy(Router *router, RouterConflictPolicy policy) {
    if (!router) return;
    router->conflict_policy = policy;
}

void regex_match_result_free(RegexMatchResult *result) {
    if (!result) return;

    if (result->groups) {
        for (int i = 0; i < result->group_count; i++) {
            free(result->groups[i]);
        }
        free(result->groups);
    }

    free(result->group_starts);
    free(result->group_ends);

    result->groups = NULL;
    result->group_starts = NULL;
    result->group_ends = NULL;
    result->group_count = 0;
}