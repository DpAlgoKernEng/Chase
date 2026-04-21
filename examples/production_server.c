/**
 * @file    production_server.c
 * @brief   生产级 HTTP 服务器示例（SO_REUSEPORT + Master/Worker）
 *
 * @details
 *          - 使用 Server 模块封装 HTTP 服务
 *          - 使用 Master 进程管理多个 Worker
 *          - SO_REUSEPORT 实现内核级负载均衡
 *          - 简化的请求处理流程
 *
 * @layer   Application Layer
 *
 * @depends master, server, router, response, handler
 * @usedby  示例程序
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "master.h"
#include "server.h"
#include "router.h"
#include "response.h"
#include "handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define DEFAULT_WORKERS 4
#define DEFAULT_MAX_CONNECTIONS 1024

/* ========== 自定义 Handler ========== */

/* 主页 Handler（使用 RouteHandler 签名） */
static void home_handler(HttpRequest *req, void *resp_ptr, void *user_data) {
    (void)req;
    (void)user_data;

    HttpResponse *resp = (HttpResponse *)resp_ptr;
    response_set_status(resp, HTTP_STATUS_OK);
    response_set_body_html(resp,
        "<!DOCTYPE html>"
        "<html><head><title>Chase Server</title></head>"
        "<body><h1>Hello from Chase Production Server!</h1>"
        "<p>SO_REUSEPORT + Master/Worker Architecture</p></body></html>",
        97);
}

/* API Handler（使用 RouteHandler 签名） */
static void api_handler(HttpRequest *req, void *resp_ptr, void *user_data) {
    (void)req;
    (void)user_data;

    HttpResponse *resp = (HttpResponse *)resp_ptr;
    response_set_status(resp, HTTP_STATUS_OK);
    response_set_body_json(resp, "{\"status\":\"ok\",\"version\":\"2.0\",\"arch\":\"master/worker\"}");
}

/* 健康检查 Handler（包装 handler_json_api） */
static void health_handler(HttpRequest *req, void *resp_ptr, void *user_data) {
    (void)req;
    HttpResponse *resp = (HttpResponse *)resp_ptr;
    response_set_status(resp, HTTP_STATUS_OK);
    response_set_body_json(resp, "{\"status\":\"healthy\"}");
}

/* ========== Router 创建 ========== */

static Router *create_router(void) {
    Router *router = router_create();
    if (!router) return NULL;

    /* 主页路由 */
    Route *route_home = route_create(ROUTER_MATCH_EXACT, "/", home_handler, NULL);
    if (route_home) {
        router_add_route(router, route_home);
    }

    /* API 路由 */
    Route *route_api = route_create(ROUTER_MATCH_EXACT, "/api", api_handler, NULL);
    if (route_api) {
        router_add_route(router, route_api);
    }

    /* 健康检查路由 */
    Route *route_health = route_create(ROUTER_MATCH_EXACT, "/health", health_handler, NULL);
    if (route_health) {
        router_add_route(router, route_health);
    }

    return router;
}

/* ========== Worker 入口函数 ========== */

static int worker_entry(int worker_id, const MasterConfig *mconfig) {
    printf("[Worker %d] Starting (pid=%d)\n", worker_id, getpid());

    /* 创建 Router（每个 Worker 独立） */
    Router *router = create_router();
    if (!router) {
        fprintf(stderr, "[Worker %d] Failed to create router\n", worker_id);
        return 1;
    }

    /* 创建 Server 配置 */
    ServerConfig config = {
        .port = mconfig->port,
        .max_connections = mconfig->max_connections,
        .backlog = mconfig->backlog,
        .bind_addr = mconfig->bind_addr,
        .reuseport = true,
        .router = router,
        .read_buf_cap = 0,      /* 使用默认 */
        .write_buf_cap = 0      /* 使用默认 */
    };

    /* 创建 Server */
    Server *server = server_create(&config);
    if (!server) {
        fprintf(stderr, "[Worker %d] Failed to create server\n", worker_id);
        router_destroy(router);
        return 1;
    }

    /* 运行 Server */
    int result = server_run(server);

    /* 清理 */
    server_destroy(server);
    router_destroy(router);

    printf("[Worker %d] Exited (result=%d)\n", worker_id, result);

    return result;
}

/* ========== 主入口 ========== */

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    int workers = DEFAULT_WORKERS;

    if (argc > 1) {
        port = atoi(argv[1]);
    }
    if (argc > 2) {
        workers = atoi(argv[2]);
    }

    printf("=== Chase HTTP Server - Production ===\n");
    printf("Port: %d, Workers: %d\n", port, workers);
    printf("Architecture: Master/Worker + SO_REUSEPORT\n\n");

    /* 创建 Master 配置 */
    MasterConfig config = {
        .worker_count = workers,
        .port = port,
        .max_connections = DEFAULT_MAX_CONNECTIONS,
        .backlog = 1024,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    /* 创建 Master */
    Master *master = master_create(&config);
    if (!master) {
        fprintf(stderr, "Failed to create master\n");
        return 1;
    }

    /* 设置重启策略 */
    master_set_restart_policy(master, 10, 1000);  /* 最多重启 10 次，延迟 1 秒 */

    /* 设置 Worker 入口函数 */
    master_set_worker_main(master, worker_entry);

    /* 运行 Master */
    master_run(master);

    /* 清理 */
    master_destroy(master);

    printf("\nServer stopped.\n");

    return 0;
}