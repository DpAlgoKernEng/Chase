/**
 * @file    timer.h
 * @brief   定时器管理，基于最小堆实现高效超时调度
 *
 * @details
 *          - 最小堆结构，O(log n) 添加/删除
 *          - 支持单次触发和周期性定时器
 *          - 与 EventLoop 集成
 *          - 按过期时间排序
 *
 * @layer   Core Layer
 *
 * @depends 无依赖
 * @usedby  eventloop, server
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#ifndef CHASE_TIMER_H
#define CHASE_TIMER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 定时器回调函数类型 */
typedef void (*TimerCallback)(void *user_data);

/* Timer 结构体（不透明指针） */
typedef struct Timer Timer;

/* TimerHeap 结构体（不透明指针） */
typedef struct TimerHeap TimerHeap;

/**
 * 创建定时器堆
 * @param capacity 初始容量
 * @return TimerHeap 指针，失败返回 NULL
 */
TimerHeap *timer_heap_create(int capacity);

/**
 * 销毁定时器堆
 * @param heap TimerHeap 指针
 */
void timer_heap_destroy(TimerHeap *heap);

/**
 * 创建并添加定时器
 * @param heap TimerHeap 指针
 * @param timeout_ms 超时时间（毫秒）
 * @param cb 回调函数
 * @param user_data 用户数据
 * @param periodic 是否周期性（true=periodic, false=one-shot）
 * @return Timer 指针，失败返回 NULL
 */
Timer *timer_heap_add(TimerHeap *heap, uint64_t timeout_ms,
                      TimerCallback cb, void *user_data, bool periodic);

/**
 * 移除定时器
 * @param heap TimerHeap 指针
 * @param timer Timer 指针
 * @return 0 成功，-1 失败
 * @note 此函数会释放定时器，调用后 timer 指针无效
 */
int timer_heap_remove(TimerHeap *heap, Timer *timer);

/**
 * 获取堆顶定时器（最小过期时间）
 * @param heap TimerHeap 指针
 * @return Timer 指针，空堆返回 NULL
 */
Timer *timer_heap_peek(TimerHeap *heap);

/**
 * 弹出堆顶定时器
 * @param heap TimerHeap 指针
 * @return Timer 指针，空堆返回 NULL
 * @note 调用者必须使用 timer_free() 释放返回的定时器
 */
Timer *timer_heap_pop(TimerHeap *heap);

/**
 * 释放定时器
 * @param timer Timer 指针
 * @note 用于释放 timer_heap_pop() 返回的定时器
 *       不要用于 heap 中的定时器（使用 timer_heap_remove）
 */
void timer_free(Timer *timer);

/**
 * 获取定时器过期时间
 * @param timer Timer 指针
 * @return 过期时间（毫秒）
 */
uint64_t timer_get_expire_time(Timer *timer);

/**
 * 获取定时器 ID
 * @param timer Timer 指针
 * @return 定时器 ID（uint64_t）
 */
uint64_t timer_get_id(Timer *timer);

/**
 * 检查堆是否为空
 * @param heap TimerHeap 指针
 * @return true 空，false 非空
 */
bool timer_heap_is_empty(TimerHeap *heap);

/**
 * 获取堆大小
 * @param heap TimerHeap 指针
 * @return 定时器数量
 */
int timer_heap_size(TimerHeap *heap);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_TIMER_H */