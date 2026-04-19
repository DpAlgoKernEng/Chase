#include "router.h"
#include <stdlib.h>
#include <string.h>

/* Router 结构体 */
struct Router {
    Route **routes;
    int route_count;
    int route_capacity;
};

/* ========== 公共 API 实现 ========== */

Router *router_create(void) {
    Router *router = malloc(sizeof(Router));
    if (!router) return NULL;

    router->routes = malloc(16 * sizeof(Route *));
    router->route_count = 0;
    router->route_capacity = 16;

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
    if (!router || !path) return NULL;

    int method_mask = 1 << method;

    /* 按优先级搜索 */
    Route *best_match = NULL;
    int best_priority = -1;

    for (int i = 0; i < router->route_count; i++) {
        Route *route = router->routes[i];

        /* 检查方法匹配 */
        if (!(route->methods & method_mask)) {
            continue;
        }

        /* 根据匹配类型检查 */
        bool matched = false;
        switch (route->type) {
        case ROUTER_MATCH_EXACT:
            matched = (strcmp(route->pattern, path) == 0);
            break;
        case ROUTER_MATCH_PREFIX:
            matched = (strncmp(route->pattern, path, strlen(route->pattern)) == 0);
            break;
        case ROUTER_MATCH_REGEX:
            /* 正则匹配暂不实现 */
            matched = false;
            break;
        }

        if (matched && route->priority > best_priority) {
            best_match = route;
            best_priority = route->priority;
        }
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

    if (!route->pattern) {
        free(route);
        return NULL;
    }

    return route;
}

void route_destroy(Route *route) {
    if (!route) return;
    free(route->pattern);
    free(route);
}