#include "thread_pool_manager.h"
#include "eventloop.h"
#include "connection_pool.h"
#include "connection.h"
#include "router.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/uio.h>  /* SCM_RIGHTS */

#ifdef __linux__
#include <sys/eventfd.h>
#else
/* macOS 使用 pipe 替代 eventfd */
#include <sys/types.h>
#include <sys/socket.h>
#endif

/* ========== WorkerThread 结构体 ========== */
struct WorkerThread {
    int worker_id;                      /* Worker ID */
    EventLoop *loop;                    /* EventLoop */
    pthread_t thread;                   /* 线程 ID */
    int notify_fd;                      /* 通知 fd (eventfd/pipe) */
    int notify_read_fd;                 /* pipe 读端（macOS） */
    int notify_write_fd;                /* pipe 写端（macOS） */
    volatile bool running;              /* 运行标志 */
    volatile int connection_count;      /* 当前连接数 */
    pthread_mutex_t count_mutex;        /* 连接计数锁 */

    /* ConnectionPool */
    ConnectionPool *conn_pool;          /* 连接池 */
    int pool_capacity;                  /* 池容量 */

    /* 共享 Router */
    Router *router;                     /* 共享路由器 */

    /* 连接处理回调 */
    ConnectionHandler conn_handler;     /* 连接处理回调 */
    void *handler_user_data;            /* 回调用户数据 */
};

/* ========== ThreadPoolManager 结构体 ========== */
struct ThreadPoolManager {
    WorkerThread **workers;             /* Worker 数组 */
    int worker_count;                   /* Worker 数量 */
    DispatchStrategy strategy;          /* 分发策略 */
    int last_dispatch_index;            /* Round-Robin 上次分发索引 */
    int cached_least_loaded;            /* 缓存的最少负载 Worker */
    pthread_mutex_t dispatch_mutex;     /* 分发锁 */
    volatile bool running;              /* 运行标志 */
};

/* ========== WorkerThread 内部函数 ========== */

/* macOS fd passing: 发送文件描述符 */
static int send_fd(int socket, int fd_to_send) {
    struct msghdr msg = {0};
    char buf[1] = {0};
    struct iovec iov = { .iov_base = buf, .iov_len = 1 };

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg = (struct cmsghdr *)cmsg_buf;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    *((int *)CMSG_DATA(cmsg)) = fd_to_send;

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int));

    return sendmsg(socket, &msg, 0);
}

/* macOS fd passing: 接收文件描述符 */
static int recv_fd(int socket) {
    struct msghdr msg = {0};
    char buf[1];
    struct iovec iov = { .iov_base = buf, .iov_len = 1 };

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int));

    if (recvmsg(socket, &msg, 0) < 0) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        return *((int *)CMSG_DATA(cmsg));
    }
    return -1;
}

/* Worker 线程入口函数（阻塞式事件驱动） */
static void *worker_thread_entry(void *arg) {
    WorkerThread *worker = (WorkerThread *)arg;

    /* 使用阻塞式事件循环，高效处理事件 */
    eventloop_run(worker->loop);

    return NULL;
}

/* 通知回调：处理新连接分发 */
static void worker_notify_callback(int fd, uint32_t events, void *user_data) {
    WorkerThread *worker = (WorkerThread *)user_data;

    /* 检查停止标志 */
    if (!worker->running) {
        /* 停止信号，退出事件循环 */
        eventloop_stop(worker->loop);
        return;
    }

    if (events & EV_READ) {
        /* 循环接收所有待处理的 client_fd */
        while (worker->running) {
            int client_fd = -1;

    #ifdef __linux__
            /* Linux: 使用 eventfd 发送 fd 值 */
            uint64_t value = 0;
            ssize_t ret = read(fd, &value, sizeof(value));
            if (ret > 0) {
                client_fd = (int)value;
            }
    #else
            /* macOS: 使用 SCM_RIGHTS fd passing */
            client_fd = recv_fd(fd);
    #endif

            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* 缓冲区空，正常退出循环 */
                    break;
                }
                /* 其他错误，回退计数并退出 */
                worker_thread_decrement_connections(worker);
                break;
            }

            /* 调用外部连接处理回调 */
            if (worker->conn_handler) {
                worker->conn_handler(client_fd, worker->loop, worker->handler_user_data);
            } else {
                /* 无回调，直接关闭 */
                close(client_fd);
                worker_thread_decrement_connections(worker);
            }
        }
    }
}

/* ========== WorkerThread API 实现 ========== */

WorkerThread *worker_thread_create(int max_events, int worker_id) {
    WorkerThread *worker = malloc(sizeof(WorkerThread));
    if (!worker) return NULL;

    worker->worker_id = worker_id;
    worker->running = false;
    worker->connection_count = 0;
    pthread_mutex_init(&worker->count_mutex, NULL);

    /* 创建 EventLoop */
    worker->loop = eventloop_create(max_events);
    if (!worker->loop) {
        free(worker);
        return NULL;
    }

    /* 创建通知 fd */
#ifdef __linux__
    worker->notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    worker->notify_read_fd = worker->notify_fd;
    worker->notify_write_fd = worker->notify_fd;
#else
    /* macOS 使用 socketpair */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        eventloop_destroy(worker->loop);
        free(worker);
        return NULL;
    }
    /* 设置非阻塞 */
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    worker->notify_read_fd = sv[0];
    worker->notify_write_fd = sv[1];
    worker->notify_fd = sv[0];  /* 监听读端 */
#endif

    if (worker->notify_fd < 0) {
        eventloop_destroy(worker->loop);
        free(worker);
        return NULL;
    }

    /* 注册通知事件监听 */
    if (eventloop_add(worker->loop, worker->notify_fd, EV_READ,
                      worker_notify_callback, worker) < 0) {
#ifdef __linux__
        close(worker->notify_fd);
#else
        close(worker->notify_read_fd);
        close(worker->notify_write_fd);
#endif
        eventloop_destroy(worker->loop);
        free(worker);
        return NULL;
    }

    return worker;
}

void worker_thread_destroy(WorkerThread *worker) {
    if (!worker) return;

    worker_thread_stop(worker);

#ifdef __linux__
    if (worker->notify_fd >= 0) {
        close(worker->notify_fd);
    }
#else
    if (worker->notify_read_fd >= 0) {
        close(worker->notify_read_fd);
    }
    if (worker->notify_write_fd >= 0) {
        close(worker->notify_write_fd);
    }
#endif

    if (worker->conn_pool) {
        connection_pool_destroy(worker->conn_pool);
    }

    if (worker->loop) {
        eventloop_destroy(worker->loop);
    }

    pthread_mutex_destroy(&worker->count_mutex);
    free(worker);
}

/* 创建带 ConnectionPool 的 Worker */
WorkerThread *worker_thread_create_with_pool(int max_events, int worker_id, int pool_capacity) {
    WorkerThread *worker = worker_thread_create(max_events, worker_id);
    if (!worker) return NULL;

    /* 创建 ConnectionPool */
    worker->conn_pool = connection_pool_create(pool_capacity);
    if (!worker->conn_pool) {
        worker_thread_destroy(worker);
        return NULL;
    }
    worker->pool_capacity = pool_capacity;

    /* 初始化其他字段 */
    worker->router = NULL;
    worker->conn_handler = NULL;
    worker->handler_user_data = NULL;

    return worker;
}

ConnectionPool *worker_thread_get_connection_pool(WorkerThread *worker) {
    if (!worker) return NULL;
    return worker->conn_pool;
}

void worker_thread_set_router(WorkerThread *worker, Router *router) {
    if (!worker) return;
    worker->router = router;
}

Router *worker_thread_get_router(WorkerThread *worker) {
    if (!worker) return NULL;
    return worker->router;
}

void worker_thread_set_connection_handler(WorkerThread *worker, ConnectionHandler handler, void *user_data) {
    if (!worker) return;
    worker->conn_handler = handler;
    worker->handler_user_data = user_data;
}

int worker_thread_start(WorkerThread *worker) {
    if (!worker || worker->running) return -1;

    worker->running = true;

    if (pthread_create(&worker->thread, NULL, worker_thread_entry, worker) != 0) {
        worker->running = false;
        return -1;
    }

    return 0;
}

void worker_thread_stop(WorkerThread *worker) {
    if (!worker) return;

    worker->running = false;

    /* 唤醒阻塞的事件循环：写入 notify_fd */
#ifdef __linux__
    /* Linux: eventfd 发送停止信号 */
    uint64_t value = 1;
    write(worker->notify_write_fd, &value, sizeof(value));
#else
    /* macOS: 通过 socketpair 发送停止信号（发送无效 fd 即可唤醒） */
    char buf[1] = {0};
    write(worker->notify_write_fd, buf, 1);
#endif

    /* 等待线程结束 */
    pthread_join(worker->thread, NULL);
}

int worker_thread_get_notify_fd(WorkerThread *worker) {
    if (!worker) return -1;

#ifdef __linux__
    return worker->notify_fd;
#else
    return worker->notify_write_fd;  /* 返回写端用于分发 */
#endif
}

int worker_thread_get_connection_count(WorkerThread *worker) {
    if (!worker) return 0;

    int count;
    pthread_mutex_lock(&worker->count_mutex);
    count = worker->connection_count;
    pthread_mutex_unlock(&worker->count_mutex);

    return count;
}

void worker_thread_increment_connections(WorkerThread *worker) {
    if (!worker) return;

    pthread_mutex_lock(&worker->count_mutex);
    worker->connection_count++;
    pthread_mutex_unlock(&worker->count_mutex);
}

void worker_thread_decrement_connections(WorkerThread *worker) {
    if (!worker) return;

    pthread_mutex_lock(&worker->count_mutex);
    worker->connection_count--;
    pthread_mutex_unlock(&worker->count_mutex);
}

EventLoop *worker_thread_get_eventloop(WorkerThread *worker) {
    if (!worker) return NULL;
    return worker->loop;
}

int worker_thread_get_id(WorkerThread *worker) {
    if (!worker) return -1;
    return worker->worker_id;
}

/* ========== ThreadPoolManager API 实现 ========== */

ThreadPoolManager *thread_pool_manager_create(int worker_count, int max_events_per_worker) {
    if (worker_count <= 0 || max_events_per_worker <= 0) return NULL;

    ThreadPoolManager *manager = malloc(sizeof(ThreadPoolManager));
    if (!manager) return NULL;

    manager->worker_count = worker_count;
    manager->strategy = DISPATCH_LEAST_CONNECTIONS;
    manager->last_dispatch_index = 0;
    manager->cached_least_loaded = 0;
    manager->running = false;

    pthread_mutex_init(&manager->dispatch_mutex, NULL);

    /* 创建 Worker 数组 */
    manager->workers = malloc(worker_count * sizeof(WorkerThread *));
    if (!manager->workers) {
        pthread_mutex_destroy(&manager->dispatch_mutex);
        free(manager);
        return NULL;
    }

    /* 创建每个 Worker */
    for (int i = 0; i < worker_count; i++) {
        manager->workers[i] = worker_thread_create(max_events_per_worker, i);
        if (!manager->workers[i]) {
            /* 清理已创建的 Worker */
            for (int j = 0; j < i; j++) {
                worker_thread_destroy(manager->workers[j]);
            }
            free(manager->workers);
            pthread_mutex_destroy(&manager->dispatch_mutex);
            free(manager);
            return NULL;
        }
    }

    return manager;
}

void thread_pool_manager_destroy(ThreadPoolManager *manager) {
    if (!manager) return;

    thread_pool_manager_stop(manager);

    for (int i = 0; i < manager->worker_count; i++) {
        worker_thread_destroy(manager->workers[i]);
    }

    free(manager->workers);
    pthread_mutex_destroy(&manager->dispatch_mutex);
    free(manager);
}

int thread_pool_manager_start(ThreadPoolManager *manager) {
    if (!manager || manager->running) return -1;

    manager->running = true;

    /* 启动所有 Worker */
    for (int i = 0; i < manager->worker_count; i++) {
        if (worker_thread_start(manager->workers[i]) < 0) {
            /* 启动失败，停止已启动的 Worker */
            for (int j = 0; j < i; j++) {
                worker_thread_stop(manager->workers[j]);
            }
            manager->running = false;
            return -1;
        }
    }

    return 0;
}

void thread_pool_manager_stop(ThreadPoolManager *manager) {
    if (!manager) return;

    manager->running = false;

    /* 停止所有 Worker */
    for (int i = 0; i < manager->worker_count; i++) {
        worker_thread_stop(manager->workers[i]);
    }
}

/* Least-Connections 分发策略 */
static int dispatch_least_connections(ThreadPoolManager *manager) {
    int least_index = manager->cached_least_loaded;
    int least_count = worker_thread_get_connection_count(manager->workers[least_index]);

    /* 扫描所有 Worker 找最少连接 */
    for (int i = 0; i < manager->worker_count; i++) {
        int count = worker_thread_get_connection_count(manager->workers[i]);
        if (count < least_count) {
            least_count = count;
            least_index = i;
        }
    }

    /* 更新缓存 */
    manager->cached_least_loaded = least_index;

    return least_index;
}

/* Round-Robin 分发策略 */
static int dispatch_round_robin(ThreadPoolManager *manager) {
    int index = manager->last_dispatch_index;
    manager->last_dispatch_index = (index + 1) % manager->worker_count;
    return index;
}

int thread_pool_manager_dispatch(ThreadPoolManager *manager, int fd) {
    if (!manager || !manager->running || fd < 0) return -1;

    pthread_mutex_lock(&manager->dispatch_mutex);

    int worker_index;

    /* 根据策略选择 Worker */
    switch (manager->strategy) {
    case DISPATCH_LEAST_CONNECTIONS:
        worker_index = dispatch_least_connections(manager);
        break;
    case DISPATCH_ROUND_ROBIN:
        worker_index = dispatch_round_robin(manager);
        break;
    default:
        worker_index = 0;
    }

    WorkerThread *worker = manager->workers[worker_index];
    int notify_fd = worker_thread_get_notify_fd(worker);

    /* 增加连接计数 */
    worker_thread_increment_connections(worker);

    pthread_mutex_unlock(&manager->dispatch_mutex);

    /* 发送 client_fd 到 Worker */
    ssize_t ret;

#ifdef __linux__
    /* Linux: 使用 eventfd 发送 fd 值 */
    uint64_t value = (uint64_t)fd;
    ret = write(notify_fd, &value, sizeof(value));
#else
    /* macOS: 使用 SCM_RIGHTS 发送 fd */
    ret = send_fd(notify_fd, fd);
#endif

    if (ret < 0) {
        /* 通知失败，回退计数 */
        worker_thread_decrement_connections(worker);
        return -1;
    }

    return 0;
}

int thread_pool_manager_set_strategy(ThreadPoolManager *manager, DispatchStrategy strategy) {
    if (!manager) return -1;

    pthread_mutex_lock(&manager->dispatch_mutex);
    manager->strategy = strategy;
    pthread_mutex_unlock(&manager->dispatch_mutex);

    return 0;
}

DispatchStrategy thread_pool_manager_get_strategy(ThreadPoolManager *manager) {
    if (!manager) return DISPATCH_LEAST_CONNECTIONS;
    return manager->strategy;
}

int thread_pool_manager_get_worker_count(ThreadPoolManager *manager) {
    if (!manager) return 0;
    return manager->worker_count;
}

int thread_pool_manager_get_worker_connections(ThreadPoolManager *manager, int worker_index) {
    if (!manager || worker_index < 0 || worker_index >= manager->worker_count) return -1;
    return worker_thread_get_connection_count(manager->workers[worker_index]);
}

int thread_pool_manager_get_balance_stats(ThreadPoolManager *manager, int *connections, int count) {
    if (!manager || !connections || count != manager->worker_count) return -1;

    for (int i = 0; i < manager->worker_count; i++) {
        connections[i] = worker_thread_get_connection_count(manager->workers[i]);
    }

    return 0;
}

EventLoop *thread_pool_manager_get_worker_eventloop(ThreadPoolManager *manager, int worker_index) {
    if (!manager || worker_index < 0 || worker_index >= manager->worker_count) return NULL;
    return worker_thread_get_eventloop(manager->workers[worker_index]);
}

WorkerThread *thread_pool_manager_get_worker(ThreadPoolManager *manager, int index) {
    if (!manager || index < 0 || index >= manager->worker_count) return NULL;
    return manager->workers[index];
}