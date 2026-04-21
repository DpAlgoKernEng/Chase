/**
 * @file    master.h
 * @brief   Master 进程管理，监控和重启 Worker 进程
 *
 * @details
 *          - 创建和管理多个 Worker 进程
 *          - 监控 Worker 崩溃并自动重启
 *          - 支持平滑关闭和重启策略
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

#ifndef CHASE_MASTER_H
#define CHASE_MASTER_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>  /* pid_t */
#include <time.h>       /* time_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Master 进程配置
 */
typedef struct MasterConfig MasterConfig;

/**
 * Worker 进程状态
 */
typedef enum {
    WORKER_STATE_IDLE,          /**< 空闲（未启动） */
    WORKER_STATE_RUNNING,       /**< 运行中 */
    WORKER_STATE_STOPPING,      /**< 正在停止 */
    WORKER_STATE_CRASHED,       /**< 已崩溃 */
    WORKER_STATE_STOPPED        /**< 已停止 */
} WorkerState;

/**
 * Worker 进程信息
 */
typedef struct WorkerInfo WorkerInfo;

/**
 * Worker 进程入口函数类型
 */
typedef int (*WorkerMainFunc)(int worker_id, const MasterConfig *config);

/* MasterConfig 详细定义 */
struct MasterConfig {
    int worker_count;           /**< Worker 进程数量 */
    int port;                   /**< 监听端口 */
    int max_connections;        /**< 最大连接数 */
    int backlog;                /**< listen backlog */
    bool reuseport;             /**< 是否启用 SO_REUSEPORT */
    const char *bind_addr;      /**< 绑定地址（NULL = 0.0.0.0） */
    void *user_data;            /**< 用户自定义数据 */
};

/* WorkerInfo 详细定义 */
struct WorkerInfo {
    pid_t pid;                  /**< 进程 ID */
    int id;                     /**< Worker 序号（0 ~ N-1） */
    WorkerState state;          /**< 当前状态 */
    uint64_t restart_count;     /**< 重启次数 */
    uint64_t crash_count;       /**< 崩溃次数 */
    time_t start_time;          /**< 启动时间 */
    time_t last_crash_time;     /**< 上次崩溃时间 */
};

/**
 * Master 进程上下文（不透明指针）
 */
typedef struct Master Master;

/**
 * 创建 Master 进程上下文
 * @param config Master 配置
 * @return Master 指针，失败返回 NULL
 */
Master *master_create(const MasterConfig *config);

/**
 * 销毁 Master 进程上下文
 * @param master Master 指针
 */
void master_destroy(Master *master);

/**
 * 启动所有 Worker 进程
 * @param master Master 指针
 * @return 成功启动的 Worker 数量
 */
int master_start_workers(Master *master);

/**
 * 停止所有 Worker 进程（平滑关闭）
 * @param master Master 指针
 * @param timeout_ms 最大等待时间（毫秒）
 * @return 0 成功，-1 失败
 */
int master_stop_workers(Master *master, int timeout_ms);

/**
 * 重启指定 Worker 进程
 * @param master Master 指针
 * @param worker_id Worker 序号
 * @return 0 成功，-1 失败
 */
int master_restart_worker(Master *master, int worker_id);

/**
 * 运行 Master 主循环（监控 Worker）
 * @param master Master 指针
 */
void master_run(Master *master);

/**
 * 停止 Master 主循环
 * @param master Master 指针
 */
void master_stop(Master *master);

/**
 * 获取 Worker 信息
 * @param master Master 指针
 * @param worker_id Worker 序号
 * @return WorkerInfo 指针，失败返回 NULL
 */
const WorkerInfo *master_get_worker_info(Master *master, int worker_id);

/**
 * 获取 Worker 数量
 * @param master Master 指针
 * @return Worker 数量
 */
int master_get_worker_count(Master *master);

/**
 * 检查 Worker 是否需要重启
 * @param master Master 指针
 * @param worker_id Worker 序号
 * @return true 需要重启，false 不需要
 */
bool master_worker_needs_restart(Master *master, int worker_id);

/**
 * 设置 Worker 重启策略
 * @param master Master 指针
 * @param max_restarts 最大重启次数（0 = 无限）
 * @param restart_delay_ms 重启延迟（毫秒）
 */
void master_set_restart_policy(Master *master, int max_restarts, int restart_delay_ms);

/**
 * 设置 Worker 入口函数（可选）
 * @param master Master 指针
 * @param func Worker 入口函数
 * @note 如果不设置，使用内置的默认 Worker 实现
 */
void master_set_worker_main(Master *master, WorkerMainFunc func);

/**
 * 创建 SO_REUSEPORT socket（兼容旧 API）
 * @param port 端口
 * @param bind_addr 绑定地址（NULL = 0.0.0.0）
 * @param backlog listen backlog
 * @return socket fd，失败返回 -1
 * @note 建议使用 socket.h 中的 socket_create_server_default()
 */
int create_reuseport_socket(int port, const char *bind_addr, int backlog);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_MASTER_H */