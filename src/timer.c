/**
 * @file    timer.c
 * @brief   定时器管理实现，基于最小堆
 *
 * @details
 *          - 最小堆结构实现高效调度
 *          - 支持单次触发和周期性定时器
 *          - O(log n) 插入/删除复杂度
 *          - 与 EventLoop 集成
 *
 * @layer   Core Layer
 *
 * @depends 无依赖
 * @usedby  eventloop, server
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "timer.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 获取当前时间（毫秒） */
static uint64_t get_current_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Timer 结构体 */
struct Timer {
    uint64_t id;              /* 定时器 ID（uint64_t，避免溢出） */
    uint64_t expire_time;     /* 过期时间（绝对时间，毫秒） */
    uint64_t interval;        /* 间隔（0=one-shot, >0=periodic） */
    TimerCallback callback;
    void *user_data;
    int heap_index;           /* 在堆中的索引（用于快速删除） */
};

/* TimerHeap 结构体 */
struct TimerHeap {
    Timer **timers;           /* 定时器数组（最小堆） */
    int size;                 /* 当前大小 */
    int capacity;             /* 容量 */
    uint64_t next_id;         /* 下一个定时器 ID */
};

/* ========== 最小堆辅助函数 ========== */

static void heap_swap(TimerHeap *heap, int i, int j) {
    Timer *tmp = heap->timers[i];
    heap->timers[i] = heap->timers[j];
    heap->timers[j] = tmp;

    /* 更新索引 */
    heap->timers[i]->heap_index = i;
    heap->timers[j]->heap_index = j;
}

static void heap_sift_up(TimerHeap *heap, int index) {
    while (index > 0) {
        int parent = (index - 1) / 2;
        if (heap->timers[index]->expire_time >= heap->timers[parent]->expire_time) {
            break;
        }
        heap_swap(heap, index, parent);
        index = parent;
    }
}

static void heap_sift_down(TimerHeap *heap, int index) {
    while (index < heap->size) {
        int left = 2 * index + 1;
        int right = 2 * index + 2;
        int smallest = index;

        if (left < heap->size &&
            heap->timers[left]->expire_time < heap->timers[smallest]->expire_time) {
            smallest = left;
        }
        if (right < heap->size &&
            heap->timers[right]->expire_time < heap->timers[smallest]->expire_time) {
            smallest = right;
        }

        if (smallest == index) {
            break;
        }

        heap_swap(heap, index, smallest);
        index = smallest;
    }
}

/* ========== 公共 API 实现 ========== */

TimerHeap *timer_heap_create(int capacity) {
    TimerHeap *heap = malloc(sizeof(TimerHeap));
    if (!heap) return NULL;

    heap->timers = malloc(capacity * sizeof(Timer *));
    if (!heap->timers) {
        free(heap);
        return NULL;
    }

    heap->size = 0;
    heap->capacity = capacity;
    heap->next_id = 1;  /* ID 从 1 开始 */

    return heap;
}

void timer_heap_destroy(TimerHeap *heap) {
    if (!heap) return;

    /* 释放所有定时器 */
    for (int i = 0; i < heap->size; i++) {
        free(heap->timers[i]);
    }

    free(heap->timers);
    free(heap);
}

Timer *timer_heap_add(TimerHeap *heap, uint64_t timeout_ms,
                      TimerCallback cb, void *user_data, bool periodic) {
    if (!heap || !cb) return NULL;

    /* 检查容量 */
    if (heap->size >= heap->capacity) {
        int new_cap = heap->capacity * 2;
        Timer **new_timers = realloc(heap->timers, new_cap * sizeof(Timer *));
        if (!new_timers) return NULL;
        heap->timers = new_timers;
        heap->capacity = new_cap;
    }

    /* 创建定时器 */
    Timer *timer = malloc(sizeof(Timer));
    if (!timer) return NULL;

    timer->id = heap->next_id++;
    timer->expire_time = get_current_ms() + timeout_ms;
    timer->interval = periodic ? timeout_ms : 0;
    timer->callback = cb;
    timer->user_data = user_data;
    timer->heap_index = heap->size;

    /* 插入堆 */
    heap->timers[heap->size] = timer;
    heap_sift_up(heap, heap->size);
    heap->size++;

    return timer;
}

int timer_heap_remove(TimerHeap *heap, Timer *timer) {
    if (!heap || !timer) return -1;
    if (timer->heap_index < 0 || timer->heap_index >= heap->size) return -1;

    int index = timer->heap_index;

    /* 将要删除的元素与最后一个元素交换 */
    heap_swap(heap, index, heap->size - 1);
    heap->size--;

    /* 调整堆 */
    if (index < heap->size) {
        heap_sift_down(heap, index);
        heap_sift_up(heap, index);
    }

    /* 释放定时器 */
    free(timer);

    return 0;
}

Timer *timer_heap_peek(TimerHeap *heap) {
    if (!heap || heap->size == 0) return NULL;
    return heap->timers[0];
}

Timer *timer_heap_pop(TimerHeap *heap) {
    if (!heap || heap->size == 0) return NULL;

    Timer *timer = heap->timers[0];

    /* 将堆顶与最后一个元素交换 */
    heap_swap(heap, 0, heap->size - 1);
    heap->size--;

    /* 调整堆 */
    if (heap->size > 0) {
        heap_sift_down(heap, 0);
    }

    /* 清除索引 */
    timer->heap_index = -1;

    return timer;
}

uint64_t timer_get_expire_time(Timer *timer) {
    if (!timer) return 0;
    return timer->expire_time;
}

uint64_t timer_get_id(Timer *timer) {
    if (!timer) return 0;
    return timer->id;
}

bool timer_heap_is_empty(TimerHeap *heap) {
    if (!heap) return true;
    return heap->size == 0;
}

int timer_heap_size(TimerHeap *heap) {
    if (!heap) return 0;
    return heap->size;
}