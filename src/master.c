/**
 * @file    master.c
 * @brief   Master 进程管理实现
 *
 * @details
 *          - 创建和管理 Worker 进程
 *          - 监控崩溃并自动重启
 *          - SO_REUSEPORT 多进程架构
 *
 * @layer   Process Layer
 *
 * @depends worker, socket
 * @usedby  examples
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "master.h"
#include "worker.h"
#include "socket.h"
#include "server.h"
#include "router.h"
#include "handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <poll.h>

/* Master 进程上下文 */
struct Master {
    MasterConfig config;
    WorkerInfo *workers;
    int worker_count;
    volatile sig_atomic_t running;
    int max_restarts;
    int restart_delay_ms;
    int signal_pipe[2];     /* 用于信号处理的自管道 */
    WorkerMainFunc worker_main_func;  /* Worker 入口函数钩子 */
};

/* 全局 Master 指针（信号处理需要） */
static Master *g_master = NULL;

/* 信号处理启用标志（用于安全关闭） */
static volatile sig_atomic_t g_master_signals_enabled = 0;

/* 信号处理函数 */
static void master_signal_handler(int sig) {
    /* 先检查信号处理是否已禁用 */
    if (g_master_signals_enabled == 0) return;
    if (g_master == NULL) return;

    switch (sig) {
        case SIGINT:
        case SIGTERM:
            /* 平滑关闭 */
            g_master->running = 0;
            /* 写入信号管道，唤醒主循环 */
            if (g_master->signal_pipe[1] >= 0) {
                char ch = sig;
                /* write 是 async-signal-safe，但可能失败（pipe 满） */
                /* 在非阻塞模式下失败时信号丢失，这是可接受的 */
                ssize_t ret = write(g_master->signal_pipe[1], &ch, 1);
                (void)ret;  /* 忽略返回值，信号处理函数中不能做更多操作 */
            }
            break;
        case SIGCHLD:
            /* Worker 状态变化 */
            if (g_master->signal_pipe[1] >= 0) {
                char ch = SIGCHLD;
                ssize_t ret = write(g_master->signal_pipe[1], &ch, 1);
                (void)ret;
            }
            break;
        default:
            break;
    }
}

/* 安装信号处理器 */
static int install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = master_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGINT, &sa, NULL) < 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0) return -1;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) return -1;

    /* 忽略 SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* 启用信号处理 */
    g_master_signals_enabled = 1;

    return 0;
}

/* 默认 Worker 入口函数（创建 Server 和 Router） */
static int default_worker_main(int worker_id, const MasterConfig *config) {
    /* 创建默认 Router */
    Router *router = router_create();
    if (router == NULL) {
        fprintf(stderr, "[Worker %d] Failed to create router\n", worker_id);
        return 1;
    }

    /* 创建 Server 配置 */
    ServerConfig server_cfg = {
        .port = config->port,
        .max_connections = config->max_connections,
        .backlog = config->backlog,
        .bind_addr = config->bind_addr,
        .reuseport = config->reuseport,
        .router = router,
        .read_buf_cap = 0,
        .write_buf_cap = 0
    };

    /* 创建 Server */
    Server *server = server_create(&server_cfg);
    if (server == NULL) {
        fprintf(stderr, "[Worker %d] Failed to create server\n", worker_id);
        router_destroy(router);
        return 1;
    }

    /* 创建 Worker 配置 */
    WorkerConfig wcfg = {
        .worker_id = worker_id,
        .server = server,
        .running = 1
    };

    Worker *worker = worker_create(&wcfg);
    if (worker == NULL) {
        fprintf(stderr, "[Worker %d] Failed to create worker\n", worker_id);
        server_destroy(server);
        router_destroy(router);
        return 1;
    }

    int ret = worker_run(worker);

    worker_destroy(worker);
    server_destroy(server);
    router_destroy(router);

    return ret;
}

/* 重置 Worker 进程的信号处理器 */
static void reset_worker_signal_handlers(void) {
    /* 重置 g_master 为 NULL，避免信号处理器访问无效内存 */
    g_master = NULL;

    /* 恢复默认信号处理器 */
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);

    /* 仍然忽略 SIGPIPE */
    signal(SIGPIPE, SIG_IGN);
}

/* 启动单个 Worker */
static pid_t spawn_worker(Master *master, int worker_id) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        /* 子进程（Worker） - 关闭继承的父进程 pipe */
        if (master->signal_pipe[0] >= 0) {
            close(master->signal_pipe[0]);
        }
        if (master->signal_pipe[1] >= 0) {
            close(master->signal_pipe[1]);
        }

        /* 重置信号处理器 */
        reset_worker_signal_handlers();

        int ret;
        if (master->worker_main_func) {
            ret = master->worker_main_func(worker_id, &master->config);
        } else {
            ret = default_worker_main(worker_id, &master->config);
        }
        exit(ret);
    }

    /* 父进程（Master） */
    master->workers[worker_id].pid = pid;
    master->workers[worker_id].state = WORKER_STATE_RUNNING;
    master->workers[worker_id].start_time = time(NULL);

    printf("[Master] Spawned Worker %d (pid=%d)\n", worker_id, pid);

    return pid;
}

/* 处理 Worker 状态变化 */
static void handle_worker_status(Master *master) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* 找到对应的 Worker */
        int worker_id = -1;
        for (int i = 0; i < master->worker_count; i++) {
            if (master->workers[i].pid == pid) {
                worker_id = i;
                break;
            }
        }

        if (worker_id < 0) {
            /* 未知进程，忽略 */
            continue;
        }

        WorkerInfo *wi = &master->workers[worker_id];

        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            printf("[Master] Worker %d (pid=%d) exited with code %d\n",
                   worker_id, pid, exit_code);

            if (exit_code == 0) {
                wi->state = WORKER_STATE_STOPPED;
            } else {
                wi->state = WORKER_STATE_CRASHED;
                wi->crash_count++;
                wi->last_crash_time = time(NULL);
            }
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            printf("[Master] Worker %d (pid=%d) killed by signal %d\n",
                   worker_id, pid, sig);

            wi->state = WORKER_STATE_CRASHED;
            wi->crash_count++;
            wi->last_crash_time = time(NULL);
        }
    }
}

/* 检查并重启崩溃的 Worker */
static void check_and_restart_workers(Master *master) {
    for (int i = 0; i < master->worker_count; i++) {
        WorkerInfo *wi = &master->workers[i];

        if (wi->state == WORKER_STATE_CRASHED) {
            /* 检查是否需要重启 */
            if (master->max_restarts > 0 &&
                (int)wi->restart_count >= master->max_restarts) {
                printf("[Master] Worker %d reached max restarts (%d), not restarting\n",
                       i, master->max_restarts);
                wi->state = WORKER_STATE_STOPPED;
                continue;
            }

            /* 检查重启延迟（使用秒级比较，但基于毫秒延迟值） */
            time_t now = time(NULL);
            time_t delay_seconds = master->restart_delay_ms / 1000;
            if (master->restart_delay_ms > 0 && delay_seconds == 0) {
                delay_seconds = 1;  /* 最小延迟 1 秒 */
            }
            if (wi->last_crash_time > 0 &&
                (now - wi->last_crash_time) < delay_seconds) {
                continue;  /* 延迟未到 */
            }

            printf("[Master] Restarting crashed Worker %d (restart #%llu)\n",
                   i, wi->restart_count + 1);

            pid_t new_pid = spawn_worker(master, i);
            if (new_pid > 0) {
                wi->restart_count++;
            }
        }
    }
}

Master *master_create(const MasterConfig *config) {
    Master *master = calloc(1, sizeof(Master));
    if (master == NULL) return NULL;

    memcpy(&master->config, config, sizeof(MasterConfig));
    master->worker_count = config->worker_count;
    master->running = 1;
    master->max_restarts = 0;      /* 默认无限重启 */
    master->restart_delay_ms = 1000; /* 默认 1 秒延迟 */

    /* 创建 Worker 信息数组 */
    master->workers = calloc(config->worker_count, sizeof(WorkerInfo));
    if (master->workers == NULL) {
        free(master);
        return NULL;
    }

    /* 初始化 Worker 信息 */
    for (int i = 0; i < config->worker_count; i++) {
        master->workers[i].id = i;
        master->workers[i].state = WORKER_STATE_IDLE;
    }

    /* 创建信号管道 */
    master->signal_pipe[0] = -1;
    master->signal_pipe[1] = -1;
    if (pipe(master->signal_pipe) < 0) {
        perror("pipe");
        free(master->workers);
        free(master);
        return NULL;
    }

    /* 设置管道非阻塞 */
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(master->signal_pipe[i], F_GETFL, 0);
        fcntl(master->signal_pipe[i], F_SETFL, flags | O_NONBLOCK);
    }

    /* 设置全局指针 */
    g_master = master;

    return master;
}

void master_destroy(Master *master) {
    if (master == NULL) return;

    /* 先禁用信号处理 */
    g_master_signals_enabled = 0;

    /* 阻塞相关信号，防止在销毁过程中收到信号 */
    sigset_t mask, old_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &old_mask);

    g_master = NULL;

    /* 关闭信号管道 */
    if (master->signal_pipe[0] >= 0) close(master->signal_pipe[0]);
    if (master->signal_pipe[1] >= 0) close(master->signal_pipe[1]);

    /* 停止所有 Worker */
    master_stop_workers(master, 5000);

    free(master->workers);
    free(master);

    /* 恢复信号屏蔽 */
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
}

int master_start_workers(Master *master) {
    if (master == NULL) return -1;

    /* 安装信号处理器 */
    if (install_signal_handlers() < 0) {
        fprintf(stderr, "[Master] Failed to install signal handlers\n");
        return -1;
    }

    int started = 0;
    for (int i = 0; i < master->worker_count; i++) {
        pid_t pid = spawn_worker(master, i);
        if (pid > 0) {
            started++;
        }
    }

    printf("[Master] Started %d workers\n", started);
    return started;
}

int master_stop_workers(Master *master, int timeout_ms) {
    if (master == NULL) return -1;

    /* 发送 SIGTERM 给所有运行中的 Worker */
    for (int i = 0; i < master->worker_count; i++) {
        WorkerInfo *wi = &master->workers[i];
        if (wi->state == WORKER_STATE_RUNNING && wi->pid > 0) {
            printf("[Master] Sending SIGTERM to Worker %d (pid=%d)\n", i, wi->pid);
            kill(wi->pid, SIGTERM);
            wi->state = WORKER_STATE_STOPPING;
        }
    }

    /* 等待 Worker 退出 */
    int deadline = timeout_ms;
    while (deadline > 0) {
        handle_worker_status(master);

        /* 检查是否所有 Worker 都已停止 */
        bool all_stopped = true;
        for (int i = 0; i < master->worker_count; i++) {
            if (master->workers[i].state == WORKER_STATE_RUNNING ||
                master->workers[i].state == WORKER_STATE_STOPPING) {
                all_stopped = false;
                break;
            }
        }

        if (all_stopped) break;

        usleep(100000);  /* 100ms */
        deadline -= 100;
    }

    /* 强制杀死未退出的 Worker */
    if (deadline <= 0) {
        for (int i = 0; i < master->worker_count; i++) {
            WorkerInfo *wi = &master->workers[i];
            if ((wi->state == WORKER_STATE_RUNNING ||
                 wi->state == WORKER_STATE_STOPPING) && wi->pid > 0) {
                printf("[Master] Force killing Worker %d (pid=%d)\n", i, wi->pid);
                kill(wi->pid, SIGKILL);
                /* 等待进程退出，避免僵尸进程 */
                int status;
                waitpid(wi->pid, &status, 0);
                wi->state = WORKER_STATE_STOPPED;
                wi->pid = 0;  /* 清除 pid，避免后续误操作 */
            }
        }
    }

    return 0;
}

int master_restart_worker(Master *master, int worker_id) {
    if (master == NULL || worker_id < 0 || worker_id >= master->worker_count) {
        return -1;
    }

    WorkerInfo *wi = &master->workers[worker_id];

    /* 先停止 */
    if (wi->state == WORKER_STATE_RUNNING && wi->pid > 0) {
        kill(wi->pid, SIGTERM);
        wi->state = WORKER_STATE_STOPPING;

        /* 等待退出（带超时） */
        int timeout_ms = 5000;  /* 5秒超时 */
        int elapsed = 0;
        int status;
        while (elapsed < timeout_ms) {
            pid_t ret = waitpid(wi->pid, &status, WNOHANG);
            if (ret > 0) {
                /* Worker 已退出 */
                wi->state = WORKER_STATE_STOPPED;
                wi->pid = 0;
                break;
            }
            usleep(100000);  /* 100ms */
            elapsed += 100;
        }

        /* 超时则强制杀死 */
        if (elapsed >= timeout_ms && wi->pid > 0) {
            printf("[Master] Worker %d timeout, force killing\n", worker_id);
            kill(wi->pid, SIGKILL);
            waitpid(wi->pid, &status, 0);
            wi->state = WORKER_STATE_STOPPED;
            wi->pid = 0;
        }
    }

    /* 重新启动 */
    pid_t new_pid = spawn_worker(master, worker_id);
    if (new_pid > 0) {
        wi->restart_count++;
        return 0;
    }

    return -1;
}

void master_run(Master *master) {
    if (master == NULL) return;

    printf("[Master] Running (pid=%d, workers=%d)\n",
           getpid(), master->worker_count);

    /* 启动 Worker */
    master_start_workers(master);

    /* 主循环 */
    while (master->running) {
        /* 使用 poll 等待信号管道 */
        struct pollfd pfd;
        pfd.fd = master->signal_pipe[0];
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 1000);  /* 1秒超时 */

        if (ret > 0 && (pfd.revents & POLLIN)) {
            /* 收到信号 */
            char sig;
            while (read(master->signal_pipe[0], &sig, 1) > 0) {
                if (sig == SIGCHLD) {
                    handle_worker_status(master);
                }
                /* SIGINT/SIGTERM 已设置 running = 0 */
            }
        }

        /* 定期检查崩溃的 Worker */
        check_and_restart_workers(master);
    }

    /* 平滑关闭 */
    printf("[Master] Shutting down...\n");
    master_stop_workers(master, 5000);
}

void master_stop(Master *master) {
    if (master == NULL) return;
    master->running = 0;

    if (master->signal_pipe[1] >= 0) {
        char ch = SIGTERM;
        write(master->signal_pipe[1], &ch, 1);
    }
}

const WorkerInfo *master_get_worker_info(Master *master, int worker_id) {
    if (master == NULL || worker_id < 0 || worker_id >= master->worker_count) {
        return NULL;
    }
    return &master->workers[worker_id];
}

int master_get_worker_count(Master *master) {
    if (master == NULL) return 0;
    return master->worker_count;
}

bool master_worker_needs_restart(Master *master, int worker_id) {
    if (master == NULL || worker_id < 0 || worker_id >= master->worker_count) {
        return false;
    }

    WorkerInfo *wi = &master->workers[worker_id];

    if (wi->state == WORKER_STATE_CRASHED) {
        if (master->max_restarts > 0 && (int)wi->restart_count >= master->max_restarts) {
            return false;
        }
        return true;
    }

    return false;
}

void master_set_restart_policy(Master *master, int max_restarts, int restart_delay_ms) {
    if (master == NULL) return;
    master->max_restarts = max_restarts;
    master->restart_delay_ms = restart_delay_ms;
}

void master_set_worker_main(Master *master, WorkerMainFunc func) {
    if (master == NULL) return;
    master->worker_main_func = func;
}

/* 兼容旧 API：create_reuseport_socket */
int create_reuseport_socket(int port, const char *bind_addr, int backlog) {
    return socket_create_server_default(port, bind_addr, backlog);
}