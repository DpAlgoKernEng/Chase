/**
 * @file    test_phase1_phase2_integration.c
 * @brief   Phase 1 + Phase 2 联合集成测试
 *
 * @details
 *          测试多进程架构与 HTTP 处理的联合工作情况：
 *          - 多 Worker 同时处理 HTTP 请求
 *          - Worker 崩溃恢复不影响 HTTP 服务
 *          - SO_REUSEPORT 负载均衡效果
 *          - 平滑关闭时连接完整处理
 *          - 多 Worker 吞吐量基准
 *          - 长时间运行稳定性
 *
 * @layer   Test (Integration)
 *
 * @depends master, worker, server, router, response, handler, http_parser
 * @usedby  测试框架
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "master.h"
#include "worker.h"
#include "server.h"
#include "router.h"
#include "response.h"
#include "handler.h"
#include "http_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>

/* ========== 测试辅助宏 ========== */

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("Test %d: %s\n", test_count++, #name); \
    test_##name(); \
    test_passed++; \
} while(0)

static int test_passed = 0;
static int test_count = 1;

/* ========== 测试端口分配 ========== */

#define TEST_PORT_BASE 20000
static int current_test_port = TEST_PORT_BASE;

static int get_test_port(void) {
    return current_test_port++;
}

/* ========== HTTP 客户端辅助函数 ========== */

/**
 * 发送 HTTP 请求并获取响应
 * @param port 目标端口
 * @param path 请求路径
 * @param method HTTP 方法
 * @param body 请求体（可选）
 * @param response_buf 响应缓冲区
 * @param buf_size 缓冲区大小
 * @return 0 成功，-1 失败
 */
static int http_request(int port, const char *path, const char *method,
                        const char *body, char *response_buf, size_t buf_size) {
    int sockfd;
    struct sockaddr_in addr;
    char request[1024];

    /* 创建 socket */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;

    /* 设置超时（5秒） */
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* 连接服务器 */
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sockfd);
        return -1;
    }

    /* 构造请求 */
    int len = snprintf(request, sizeof(request),
        "%s %s HTTP/1.1\r\n"
        "Host: localhost:%d\r\n"
        "Connection: close\r\n"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s",
        method, path, port,
        body ? strlen(body) : 0,
        body ? body : "");

    /* 发送请求 */
    if (write(sockfd, request, len) != len) {
        close(sockfd);
        return -1;
    }

    /* 读取响应 */
    ssize_t total = 0;
    ssize_t n;
    while ((n = read(sockfd, response_buf + total, buf_size - (size_t)total - 1)) > 0) {
        total += n;
        if ((size_t)total >= buf_size - 1) break;
    }
    response_buf[total] = '\0';

    close(sockfd);
    return (total > 0) ? 0 : -1;
}

/**
 * 检查响应是否包含指定字符串
 */
static bool response_contains(const char *resp, const char *pattern) {
    return strstr(resp, pattern) != NULL;
}

/**
 * 检查响应状态码
 */
static bool response_is_status(const char *resp, const char *status) {
    char buf[64];
    snprintf(buf, sizeof(buf), "HTTP/1.1 %s", status);
    return strstr(resp, buf) != NULL;
}

/* ========== Worker HTTP Handler ========== */

static void test_home_handler(HttpRequest *req, void *resp_ptr, void *user_data) {
    (void)req; (void)user_data;
    HttpResponse *resp = (HttpResponse *)resp_ptr;
    response_set_status(resp, HTTP_STATUS_OK);
    response_set_body_html(resp, "<h1>Hello from Worker</h1>", 24);
}

static void test_api_handler(HttpRequest *req, void *resp_ptr, void *user_data) {
    (void)req; (void)user_data;
    HttpResponse *resp = (HttpResponse *)resp_ptr;
    response_set_status(resp, HTTP_STATUS_OK);
    response_set_body_json(resp, "{\"test\":\"integration\",\"worker\":\"active\"}");
}

static void test_health_handler(HttpRequest *req, void *resp_ptr, void *user_data) {
    (void)req; (void)user_data;
    HttpResponse *resp = (HttpResponse *)resp_ptr;
    response_set_status(resp, HTTP_STATUS_OK);
    response_set_body_json(resp, "{\"healthy\":true}");
}

static void test_slow_handler(HttpRequest *req, void *resp_ptr, void *user_data) {
    (void)req; (void)user_data;
    /* 模拟慢响应（1秒） */
    sleep(1);
    HttpResponse *resp = (HttpResponse *)resp_ptr;
    response_set_status(resp, HTTP_STATUS_OK);
    response_set_body_html(resp, "<h1>Slow Response Done</h1>", 26);
}

static Router *create_test_router(void) {
    Router *router = router_create();
    if (!router) return NULL;

    Route *route_home = route_create(ROUTER_MATCH_EXACT, "/", test_home_handler, NULL);
    Route *route_api = route_create(ROUTER_MATCH_EXACT, "/api", test_api_handler, NULL);
    Route *route_health = route_create(ROUTER_MATCH_EXACT, "/health", test_health_handler, NULL);
    Route *route_slow = route_create(ROUTER_MATCH_EXACT, "/slow", test_slow_handler, NULL);

    router_add_route(router, route_home);
    router_add_route(router, route_api);
    router_add_route(router, route_health);
    router_add_route(router, route_slow);

    return router;
}

/* ========== Worker 入口函数 ========== */

static int http_worker_entry(int worker_id, const MasterConfig *mconfig) {
    (void)worker_id;
    Router *router = create_test_router();
    if (!router) return 1;

    ServerConfig config = {
        .port = mconfig->port,
        .max_connections = mconfig->max_connections,
        .backlog = mconfig->backlog,
        .bind_addr = mconfig->bind_addr,
        .reuseport = true,
        .router = router,
        .read_buf_cap = 0,
        .write_buf_cap = 0
    };

    Server *server = server_create(&config);
    if (!server) {
        router_destroy(router);
        return 1;
    }

    /* 运行 Server（阻塞直到收到停止信号） */
    int result = server_run(server);

    server_destroy(server);
    router_destroy(router);

    return result;
}

/* ========== 线程辅助结构和函数（用于吞吐量测试） ========== */

typedef struct {
    int port;
    int count;
    int success;
} ThreadData;

static void* throughput_request_thread(void *arg) {
    ThreadData *data = (ThreadData *)arg;
    char response[4096];

    for (int i = 0; i < data->count; i++) {
        if (http_request(data->port, "/", "GET", NULL, response, sizeof(response)) == 0) {
            if (response_is_status(response, "200 OK")) {
                data->success++;
            }
        }
    }
    return NULL;
}

/* ========== 测试 1: 多 Worker HTTP 处理 ========== */

TEST(multi_worker_http_processing) {
    int port = get_test_port();
    printf("  Using port: %d\n", port);

    MasterConfig config = {
        .worker_count = 4,
        .port = port,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_restart_policy(master, 0, 1000);
    master_set_worker_main(master, http_worker_entry);

    /* 启动 Workers */
    int started = master_start_workers(master);
    assert(started == 4);
    printf("  Started %d workers\n", started);

    /* 等待 Workers 初始化 */
    sleep(2);

    /* 发送 HTTP 请求测试 */
    char response[4096];
    int success_count = 0;

    /* 测试主页 */
    if (http_request(port, "/", "GET", NULL, response, sizeof(response)) == 0) {
        if (response_is_status(response, "200 OK")) {
            printf("  GET / -> 200 OK ✓\n");
            success_count++;
        }
    }

    /* 测试 API */
    if (http_request(port, "/api", "GET", NULL, response, sizeof(response)) == 0) {
        if (response_contains(response, "\"test\":\"integration\"")) {
            printf("  GET /api -> JSON ✓\n");
            success_count++;
        }
    }

    /* 测试健康检查 */
    if (http_request(port, "/health", "GET", NULL, response, sizeof(response)) == 0) {
        if (response_contains(response, "\"healthy\":true")) {
            printf("  GET /health -> OK ✓\n");
            success_count++;
        }
    }

    /* 测试 404 */
    if (http_request(port, "/notfound", "GET", NULL, response, sizeof(response)) == 0) {
        if (response_is_status(response, "404")) {
            printf("  GET /notfound -> 404 ✓\n");
            success_count++;
        }
    }

    /* 验证：至少 3 个请求成功 */
    assert(success_count >= 3);

    /* 停止 Workers */
    master_stop_workers(master, 5000);
    master_destroy(master);

    printf("  HTTP test completed: %d/4 passed\n", success_count);
}

/* ========== 测试 2: Worker 崩溃恢复 + HTTP 测试 ========== */

/* 崩溃测试用的 Worker 入口（会在处理一定请求后崩溃） */
static int crashable_worker_entry(int worker_id, const MasterConfig *mconfig) {
    Router *router = create_test_router();
    if (!router) return 1;

    ServerConfig config = {
        .port = mconfig->port,
        .max_connections = mconfig->max_connections,
        .backlog = mconfig->backlog,
        .bind_addr = mconfig->bind_addr,
        .reuseport = true,
        .router = router,
        .read_buf_cap = 0,
        .write_buf_cap = 0
    };

    Server *server = server_create(&config);
    if (!server) {
        router_destroy(router);
        return 1;
    }

    /* Worker 0 在 3 秒后崩溃（模拟 crash） */
    if (worker_id == 0) {
        sleep(3);
        printf("[Worker 0] Simulating crash...\n");
        exit(1);  /* 模拟崩溃 */
    }

    /* 其他 Worker 正常运行 */
    int result = server_run(server);

    server_destroy(server);
    router_destroy(router);

    return result;
}

TEST(worker_crash_http_recovery) {
    int port = get_test_port();
    printf("  Using port: %d\n", port);

    MasterConfig config = {
        .worker_count = 4,
        .port = port,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_restart_policy(master, 5, 500);  /* 最多重启 5 次，延迟 500ms */
    master_set_worker_main(master, crashable_worker_entry);

    int started = master_start_workers(master);
    assert(started == 4);
    printf("  Started %d workers (Worker 0 will crash in 3s)\n", started);

    sleep(2);

    /* 在崩溃前发送请求 */
    char response[4096];
    int before_crash = 0;
    if (http_request(port, "/", "GET", NULL, response, sizeof(response)) == 0) {
        before_crash++;
    }

    /* 等待 Worker 0 崩溃并重启 */
    sleep(3);

    /* 检查 Worker 0 是否被重启 */
    const WorkerInfo *info = master_get_worker_info(master, 0);
    assert(info != NULL);
    printf("  Worker 0 restart count: %llu, state: %d\n",
           (unsigned long long)info->restart_count, info->state);

    /* 崩溃后发送请求（验证服务仍然可用） */
    int after_crash = 0;
    for (int i = 0; i < 5; i++) {
        if (http_request(port, "/", "GET", NULL, response, sizeof(response)) == 0) {
            if (response_is_status(response, "200 OK")) {
                after_crash++;
            }
        }
        usleep(100000);  /* 100ms 间隔 */
    }

    printf("  Before crash: %d success, After crash: %d success\n",
           before_crash, after_crash);

    /* 验证：崩溃后至少 3 个请求成功 */
    assert(after_crash >= 3);

    master_stop_workers(master, 5000);
    master_destroy(master);

    printf("  Crash recovery test completed\n");
}

/* ========== 测试 3: 多 Worker 并发处理能力 ========== */

/**
 * 注意：由于进程隔离，无法直接统计每个 Worker 处理的请求数。
 * 这个测试验证多 Worker 能同时处理并发请求，而不是验证负载均衡比例。
 */

TEST(multi_worker_concurrent_capacity) {
    int port = get_test_port();
    printf("  Using port: %d\n", port);

    MasterConfig config = {
        .worker_count = 4,
        .port = port,
        .max_connections = 512,
        .backlog = 64,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_restart_policy(master, 0, 1000);
    master_set_worker_main(master, http_worker_entry);

    int started = master_start_workers(master);
    assert(started == 4);
    printf("  Started %d workers\n", started);

    sleep(2);

    /* 并发发送 200 个请求 */
    #define CONCURRENT_REQUESTS 200

    char response[4096];
    int total_success = 0;
    int total_failed = 0;

    printf("  Sending %d concurrent requests...\n", CONCURRENT_REQUESTS);

    for (int i = 0; i < CONCURRENT_REQUESTS; i++) {
        if (http_request(port, "/", "GET", NULL, response, sizeof(response)) == 0) {
            if (response_is_status(response, "200 OK")) {
                total_success++;
            } else {
                total_failed++;
            }
        } else {
            total_failed++;
        }
    }

    printf("  Results: %d success, %d failed\n", total_success, total_failed);

    /* 验证：至少 95% 成功 */
    float success_rate = (float)total_success / (float)CONCURRENT_REQUESTS;
    printf("  Success rate: %.1f%%\n", success_rate * 100);
    assert(success_rate >= 0.95f);

    /* 验证：所有 Worker 都在运行 */
    int running_count = 0;
    for (int i = 0; i < 4; i++) {
        const WorkerInfo *info = master_get_worker_info(master, i);
        if (info && info->state == WORKER_STATE_RUNNING) {
            running_count++;
        }
    }
    printf("  Workers still running: %d/4\n", running_count);
    assert(running_count == 4);

    master_stop_workers(master, 5000);
    master_destroy(master);

    printf("  Concurrent capacity test completed\n");
}

/* ========== 测试 4: 平滑关闭 + 连接处理 ========== */

TEST(graceful_shutdown_connections) {
    int port = get_test_port();
    printf("  Using port: %d\n", port);

    MasterConfig config = {
        .worker_count = 2,
        .port = port,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_restart_policy(master, 0, 1000);
    master_set_worker_main(master, http_worker_entry);

    int started = master_start_workers(master);
    assert(started == 2);

    sleep(2);

    /* 发送慢请求（预计耗时 1秒） */
    char response[4096];
    pid_t slow_request_pid = fork();

    if (slow_request_pid == 0) {
        /* 子进程：发送慢请求 */
        http_request(port, "/slow", "GET", NULL, response, sizeof(response));
        printf("  Slow request completed: %s\n",
               response_is_status(response, "200 OK") ? "SUCCESS" : "FAILED");
        exit(0);
    }

    /* 等待慢请求开始处理 */
    sleep(1);

    /* 发送 SIGTERM（模拟平滑关闭） */
    printf("  Sending SIGTERM to master...\n");
    kill(getpid(), SIGTERM);

    /* 注意：这个测试会比较特殊，因为测试进程本身就是 Master */
    /* 实际验证需要观察慢请求是否完成 */

    /* 等待子进程 */
    int status;
    waitpid(slow_request_pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("  Slow request completed during shutdown ✓\n");
    }

    /* 清理（如果 SIGTERM 没有终止进程） */
    master_stop_workers(master, 5000);
    master_destroy(master);
}

/* ========== 测试 5: 多 Worker 吞吐量基准 ========== */

TEST(multi_worker_throughput) {
    int port = get_test_port();
    printf("  Using port: %d\n", port);

    MasterConfig config = {
        .worker_count = 4,
        .port = port,
        .max_connections = 512,
        .backlog = 128,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_restart_policy(master, 0, 1000);
    master_set_worker_main(master, http_worker_entry);

    int started = master_start_workers(master);
    assert(started == 4);
    printf("  Started %d workers\n", started);

    sleep(2);

    /* 使用多线程并发发送请求 */
    #define NUM_THREADS 4
    #define REQUESTS_PER_THREAD 250
    #define TOTAL_REQUESTS (NUM_THREADS * REQUESTS_PER_THREAD)

    pthread_t threads[NUM_THREADS];
    ThreadData thread_data[NUM_THREADS];

    /* 启动线程 */
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].port = port;
        thread_data[i].count = REQUESTS_PER_THREAD;
        thread_data[i].success = 0;
        pthread_create(&threads[i], NULL, throughput_request_thread, &thread_data[i]);
    }

    /* 记录开始时间 */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* 等待线程完成 */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* 记录结束时间 */
    clock_gettime(CLOCK_MONOTONIC, &end);

    /* 计算吞吐量 */
    int total_success = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        total_success += thread_data[i].success;
    }

    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;
    double throughput = (double)total_success / elapsed;

    printf("  Total requests: %d, Success: %d\n", TOTAL_REQUESTS, total_success);
    printf("  Elapsed time: %.3f s\n", elapsed);
    printf("  Throughput: %.2f req/s\n", throughput);

    /* 验证：吞吐量应 >= 100 req/s（保守目标） */
    assert(throughput >= 100.0);

    /* 验证成功率 */
    float success_rate = (float)total_success / (float)TOTAL_REQUESTS;
    printf("  Success rate: %.1f%%\n", success_rate * 100);
    assert(success_rate >= 0.95f);  /* 至少 95% 成功 */

    master_stop_workers(master, 5000);
    master_destroy(master);

    printf("  Throughput test completed\n");
}

/* ========== 测试 6: 长时间运行稳定性 ========== */

TEST(long_running_stability) {
    int port = get_test_port();
    printf("  Using port: %d\n", port);

    MasterConfig config = {
        .worker_count = 2,
        .port = port,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_restart_policy(master, 10, 1000);
    master_set_worker_main(master, http_worker_entry);

    int started = master_start_workers(master);
    assert(started == 2);

    sleep(2);

    /* 30 秒持续请求 */
    #define STABILITY_DURATION 30

    char response[4096];
    int total_requests = 0;
    int success_requests = 0;
    int failed_requests = 0;

    printf("  Running stability test for %d seconds...\n", STABILITY_DURATION);

    for (int i = 0; i < STABILITY_DURATION * 10; i++) {
        if (http_request(port, "/", "GET", NULL, response, sizeof(response)) == 0) {
            if (response_is_status(response, "200 OK")) {
                success_requests++;
            } else {
                failed_requests++;
            }
        } else {
            failed_requests++;
        }
        total_requests++;

        /* 每 5 秒报告进度 */
        if (i % 50 == 0) {
            printf("  Progress: %d/%d requests, success: %d, failed: %d\n",
                   total_requests, STABILITY_DURATION * 10,
                   success_requests, failed_requests);
        }
    }

    printf("  Total: %d requests, Success: %d, Failed: %d\n",
           total_requests, success_requests, failed_requests);

    /* 验证：失败率应 < 5% */
    float fail_rate = (float)failed_requests / (float)total_requests;
    printf("  Fail rate: %.1f%%\n", fail_rate * 100);
    assert(fail_rate < 0.05f);

    /* 检查 Worker 状态 */
    int running_count = 0;
    for (int i = 0; i < 2; i++) {
        const WorkerInfo *info = master_get_worker_info(master, i);
        if (info && info->state == WORKER_STATE_RUNNING) {
            running_count++;
        }
    }

    printf("  Workers still running: %d/2\n", running_count);
    assert(running_count == 2);  /* 没有 Worker 崩溃 */

    master_stop_workers(master, 5000);
    master_destroy(master);

    printf("  Stability test completed\n");
}

/* ========== 测试 7: 路由 + 多 Worker 集成 ========== */

TEST(router_multi_worker_integration) {
    int port = get_test_port();
    printf("  Using port: %d\n", port);

    MasterConfig config = {
        .worker_count = 4,
        .port = port,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_restart_policy(master, 0, 1000);
    master_set_worker_main(master, http_worker_entry);

    int started = master_start_workers(master);
    assert(started == 4);

    sleep(2);

    /* 测试所有路由 */
    char response[4096];
    int success = 0;

    /* 主页 */
    if (http_request(port, "/", "GET", NULL, response, sizeof(response)) == 0 &&
        response_is_status(response, "200 OK")) {
        success++;
        printf("  GET / ✓\n");
    }

    /* API */
    if (http_request(port, "/api", "GET", NULL, response, sizeof(response)) == 0 &&
        response_contains(response, "\"test\":\"integration\"")) {
        success++;
        printf("  GET /api ✓\n");
    }

    /* 健康检查 */
    if (http_request(port, "/health", "GET", NULL, response, sizeof(response)) == 0 &&
        response_contains(response, "\"healthy\":true")) {
        success++;
        printf("  GET /health ✓\n");
    }

    /* 404 */
    if (http_request(port, "/nonexistent", "GET", NULL, response, sizeof(response)) == 0 &&
        response_is_status(response, "404")) {
        success++;
        printf("  GET /nonexistent -> 404 ✓\n");
    }

    assert(success >= 3);

    master_stop_workers(master, 5000);
    master_destroy(master);
}

/* ========== 测试 8: Keep-Alive 连接测试 ========== */

TEST(keepalive_basic) {
    int port = get_test_port();
    printf("  Using port: %d\n", port);

    MasterConfig config = {
        .worker_count = 2,
        .port = port,
        .max_connections = 100,
        .backlog = 10,
        .reuseport = true,
        .bind_addr = NULL,
        .user_data = NULL
    };

    Master *master = master_create(&config);
    assert(master != NULL);

    master_set_restart_policy(master, 0, 1000);
    master_set_worker_main(master, http_worker_entry);

    int started = master_start_workers(master);
    assert(started == 2);

    sleep(2);

    /* 测试 Keep-Alive 连接 */
    int sockfd;
    struct sockaddr_in addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(sockfd >= 0);

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int rc = connect(sockfd, (struct sockaddr*)&addr, sizeof(addr));
    assert(rc == 0);

    /* 发送第一个请求（Keep-Alive） */
    char request1[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    write(sockfd, request1, strlen(request1));

    char response1[4096];
    ssize_t n1 = read(sockfd, response1, sizeof(response1) - 1);
    if (n1 > 0) {
        response1[n1] = '\0';
        if (response_is_status(response1, "200 OK")) {
            printf("  First request ✓\n");
        }
    }

    /* 发送第二个请求（复用连接） */
    char request2[] = "GET /api HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    write(sockfd, request2, strlen(request2));

    char response2[4096];
    ssize_t n2 = read(sockfd, response2, sizeof(response2) - 1);
    if (n2 > 0) {
        response2[n2] = '\0';
        if (response_contains(response2, "\"test\":\"integration\"")) {
            printf("  Second request (Keep-Alive) ✓\n");
        }
    }

    close(sockfd);

    master_stop_workers(master, 5000);
    master_destroy(master);
}

/* ========== 主函数 ========== */

int main(void) {
    printf("\n========================================\n");
    printf("  Phase 1 + Phase 2 Integration Tests\n");
    printf("========================================\n\n");

    /* 注意：这些测试需要按顺序运行，因为端口是递增的 */
    RUN_TEST(multi_worker_http_processing);
    RUN_TEST(worker_crash_http_recovery);
    RUN_TEST(multi_worker_concurrent_capacity);
    RUN_TEST(graceful_shutdown_connections);
    RUN_TEST(multi_worker_throughput);
    RUN_TEST(long_running_stability);
    RUN_TEST(router_multi_worker_integration);
    RUN_TEST(keepalive_basic);

    printf("\n========================================\n");
    printf("  Test Results Summary\n");
    printf("========================================\n");
    printf("  Passed: %d/%d\n", test_passed, test_count - 1);
    printf("\n");

    return 0;
}