/**
 * @file    eventloop.h
 * @brief   I/O 事件循环，支持 epoll/kqueue/poll 多后端
 *
 * @details
 *          - 统一的 I/O 事件监听接口
 *          - 支持 Linux epoll、macOS kqueue、通用 poll
 *          - 非阻塞事件驱动架构
 *          - 支持定时器集成
 *
 * @layer   Core Layer
 *
 * @depends 无依赖
 * @usedby  server, worker, connection, timer
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#ifndef CHASE_EVENTLOOP_H
#define CHASE_EVENTLOOP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 事件类型 */
#define EV_READ    0x01
#define EV_WRITE   0x02
#define EV_ERROR   0x04
#define EV_CLOSE   0x08

/* 事件回调函数类型 */
typedef void (*EventCallback)(int fd, uint32_t events, void *user_data);

/* EventLoop 结构体（不透明指针） */
typedef struct EventLoop EventLoop;

/**
 * 创建事件循环
 * @param max_events 最大事件数量
 * @return EventLoop 指针，失败返回 NULL
 */
EventLoop *eventloop_create(int max_events);

/**
 * 销毁事件循环
 * @param loop EventLoop 指针
 */
void eventloop_destroy(EventLoop *loop);

/**
 * 添加 I/O 事件监听
 * @param loop EventLoop 指针
 * @param fd 文件描述符
 * @param events 事件类型（EV_READ | EV_WRITE 等）
 * @param cb 回调函数
 * @param user_data 用户数据
 * @return 0 成功，-1 失败
 */
int eventloop_add(EventLoop *loop, int fd, uint32_t events,
                  EventCallback cb, void *user_data);

/**
 * 修改 I/O 事件监听
 * @param loop EventLoop 指针
 * @param fd 文件描述符
 * @param events 新事件类型
 * @return 0 成功，-1 失败
 */
int eventloop_modify(EventLoop *loop, int fd, uint32_t events);

/**
 * 移除 I/O 事件监听
 * @param loop EventLoop 指针
 * @param fd 文件描述符
 * @return 0 成功，-1 失败
 */
int eventloop_remove(EventLoop *loop, int fd);

/**
 * 运行事件循环（阻塞）
 * @param loop EventLoop 指针
 */
void eventloop_run(EventLoop *loop);

/**
 * 停止事件循环
 * @param loop EventLoop 指针
 */
void eventloop_stop(EventLoop *loop);

/**
 * 单次事件轮询（非阻塞）
 * @param loop EventLoop 指针
 * @param timeout_ms 超时时间（毫秒），-1 表示无限等待
 * @return 处理的事件数量，-1 表示错误
 */
int eventloop_poll(EventLoop *loop, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_EVENTLOOP_H */