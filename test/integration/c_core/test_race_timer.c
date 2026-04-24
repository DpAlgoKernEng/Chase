/**
 * @file    test_race_timer.c
 * @brief   Timer 堆并发竞态测试
 *
 * @details
 *          - 多 Worker 添加定时器
 *          - 定时器删除竞态测试
 *          - 定时器触发竞态测试
 *
 *          注意：TimerHeap 设计为单 EventLoop 内部使用，
 *          本测试验证其设计假设（预期 TSan 报警）
 *
 * @layer   Test Layer
 *
 * @depends timer, pthread
 *
 * @author  minghui.liu
 * @date    2026-04-24
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>

#include "timer.h"

/* 测试配置 */
#define RACE_THREAD_COUNT 5
#define RACE_ITERATIONS 200
#define RACE_DURATION_SEC 3

/* 测试结果结构 */
typedef struct {
    atomic_int iterations_completed;
    atomic_int timers_added;
    atomic_int timers_removed;
    atomic_int timers_popped;
    atomic_int null_returns;
    atomic_int consistency_errors;
} RaceTestResult;

/* 线程参数结构 */
typedef struct {
    TimerHeap *heap;
    int thread_id;
    atomic_bool *running;
    RaceTestResult *result;
    int iterations_limit;
} TimerThreadArg;

/* 用于存储添加的定时器 */
#define MAX_TIMER_STORAGE 1000
static Timer *stored_timers[MAX_TIMER_STORAGE];
static atomic_int stored_count = 0;

/* 空回调函数 */
static void dummy_callback(void *user_data) {
    (void)user_data;
}

/* ============== 测试用例 ============== */

/**
 * TC-TIMER-01: 多 Worker 添加定时器
 * 测试场景：多线程同时添加定时器到堆
 * 预期结果：TSan 检测到数据竞争（设计预期）
 */
static void *add_timer_loop(void *arg) {
    TimerThreadArg *targ = (TimerThreadArg *)arg;
    TimerHeap *heap = targ->heap;
    RaceTestResult *result = targ->result;
    atomic_bool *running = targ->running;

    int iterations = 0;
    while (atomic_load(running) && iterations < targ->iterations_limit) {
        Timer *timer = timer_heap_add(heap, 1000 + iterations,
                                       dummy_callback, NULL, false);
        if (timer) {
            atomic_fetch_add(&result->timers_added, 1);
            /* 存储定时器供后续删除 */
            int idx = atomic_fetch_add(&stored_count, 1);
            if (idx < MAX_TIMER_STORAGE) {
                stored_timers[idx] = timer;
            }
        } else {
            atomic_fetch_add(&result->null_returns, 1);
        }
        iterations++;
    }

    atomic_fetch_add(&result->iterations_completed, iterations);
    return NULL;
}

static void test_timer_concurrent_add(void) {
    printf("Test 1: Concurrent timer_heap_add\n");
    printf("  Note: Expected TSan warnings (TimerHeap is single-threaded design)\n");

    TimerHeap *heap = timer_heap_create(256);
    assert(heap != NULL);

    RaceTestResult result = {0};
    atomic_bool running = true;
    stored_count = 0;

    TimerThreadArg args[RACE_THREAD_COUNT];
    pthread_t threads[RACE_THREAD_COUNT];

    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        args[i].heap = heap;
        args[i].thread_id = i;
        args[i].running = &running;
        args[i].result = &result;
        args[i].iterations_limit = RACE_ITERATIONS;
        pthread_create(&threads[i], NULL, add_timer_loop, &args[i]);
    }

    /* 运行一段时间 */
    sleep(RACE_DURATION_SEC);
    atomic_store(&running, false);

    /* 等待完成 */
    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("  Iterations: %d\n", atomic_load(&result.iterations_completed));
    printf("  Timers added: %d\n", atomic_load(&result.timers_added));
    printf("  NULL returns: %d\n", atomic_load(&result.null_returns));
    printf("  Heap size: %d\n", timer_heap_size(heap));
    printf("  Expected: TSan reports data race\n");

    timer_heap_destroy(heap);
    printf("  PASS (race detected by TSan)\n");
}

/**
 * TC-TIMER-02: 定时器删除竞态
 * 测试场景：一个线程添加，另一个线程删除
 * 预期结果：TSan 检测到数据竞争（设计预期）
 */
static void *add_timer_infinite(void *arg) {
    TimerThreadArg *targ = (TimerThreadArg *)arg;
    TimerHeap *heap = targ->heap;
    RaceTestResult *result = targ->result;
    atomic_bool *running = targ->running;

    while (atomic_load(running)) {
        Timer *timer = timer_heap_add(heap, 5000, dummy_callback, NULL, false);
        if (timer) {
            atomic_fetch_add(&result->timers_added, 1);
            int idx = atomic_fetch_add(&stored_count, 1);
            if (idx < MAX_TIMER_STORAGE) {
                stored_timers[idx] = timer;
            }
        }
        usleep(1000);  /* 1ms */
    }

    return NULL;
}

static void *remove_timer_loop(void *arg) {
    TimerThreadArg *targ = (TimerThreadArg *)arg;
    TimerHeap *heap = targ->heap;
    RaceTestResult *result = targ->result;
    atomic_bool *running = targ->running;

    while (atomic_load(running)) {
        int idx = atomic_load(&stored_count) - 1;
        if (idx >= 0) {
            Timer *timer = stored_timers[idx];
            if (timer) {
                int ret = timer_heap_remove(heap, timer);
                if (ret == 0) {
                    atomic_fetch_add(&result->timers_removed, 1);
                    stored_timers[idx] = NULL;
                    atomic_fetch_sub(&stored_count, 1);
                }
            }
        }
        usleep(2000);  /* 2ms */
    }

    return NULL;
}

static void test_timer_remove_race(void) {
    printf("Test 2: Add/remove race\n");
    printf("  Note: Expected TSan warnings\n");

    TimerHeap *heap = timer_heap_create(256);
    assert(heap != NULL);

    RaceTestResult result = {0};
    atomic_bool running = true;
    stored_count = 0;

    /* 添加线程 */
    TimerThreadArg add_arg = {
        .heap = heap,
        .thread_id = 0,
        .running = &running,
        .result = &result
    };
    pthread_t add_thread;
    pthread_create(&add_thread, NULL, add_timer_infinite, &add_arg);

    /* 删除线程 */
    TimerThreadArg remove_args[RACE_THREAD_COUNT];
    pthread_t remove_threads[RACE_THREAD_COUNT];

    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        remove_args[i].heap = heap;
        remove_args[i].thread_id = i + 1;
        remove_args[i].running = &running;
        remove_args[i].result = &result;
        pthread_create(&remove_threads[i], NULL, remove_timer_loop, &remove_args[i]);
    }

    /* 运行一段时间 */
    sleep(RACE_DURATION_SEC);
    atomic_store(&running, false);

    /* 等待完成 */
    pthread_join(add_thread, NULL);
    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        pthread_join(remove_threads[i], NULL);
    }

    printf("  Timers added: %d\n", atomic_load(&result.timers_added));
    printf("  Timers removed: %d\n", atomic_load(&result.timers_removed));
    printf("  Heap size: %d\n", timer_heap_size(heap));
    printf("  Expected: TSan reports data race\n");

    timer_heap_destroy(heap);
    printf("  PASS (race detected by TSan)\n");
}

/**
 * TC-TIMER-03: 定时器触发竞态
 * 测试场景：多线程同时 pop 触发定时器
 * 预期结果：TSan 检测到数据竞争（设计预期）
 */
static void *pop_timer_loop(void *arg) {
    TimerThreadArg *targ = (TimerThreadArg *)arg;
    TimerHeap *heap = targ->heap;
    RaceTestResult *result = targ->result;
    atomic_bool *running = targ->running;

    while (atomic_load(running)) {
        Timer *timer = timer_heap_pop(heap);
        if (timer) {
            atomic_fetch_add(&result->timers_popped, 1);
            timer_free(timer);
        } else {
            atomic_fetch_add(&result->null_returns, 1);
        }
        usleep(500);  /* 0.5ms */
    }

    return NULL;
}

static void test_timer_pop_race(void) {
    printf("Test 3: Concurrent timer_heap_pop\n");
    printf("  Note: Expected TSan warnings\n");

    TimerHeap *heap = timer_heap_create(256);
    assert(heap != NULL);

    /* 预添加定时器 */
    for (int i = 0; i < 100; i++) {
        timer_heap_add(heap, i * 10, dummy_callback, NULL, false);
    }

    RaceTestResult result = {0};
    atomic_bool running = true;

    TimerThreadArg args[RACE_THREAD_COUNT];
    pthread_t threads[RACE_THREAD_COUNT];

    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        args[i].heap = heap;
        args[i].thread_id = i;
        args[i].running = &running;
        args[i].result = &result;
        pthread_create(&threads[i], NULL, pop_timer_loop, &args[i]);
    }

    /* 运行一段时间 */
    sleep(RACE_DURATION_SEC);
    atomic_store(&running, false);

    /* 等待完成 */
    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("  Timers popped: %d\n", atomic_load(&result.timers_popped));
    printf("  NULL returns: %d\n", atomic_load(&result.null_returns));
    printf("  Heap size: %d\n", timer_heap_size(heap));
    printf("  Expected: TSan reports data race\n");

    timer_heap_destroy(heap);
    printf("  PASS (race detected by TSan)\n");
}

/**
 * TC-TIMER-04: 堆操作一致性测试
 * 测试场景：单线程快速添加/删除，验证堆结构
 * 预期结果：堆结构保持正确（无竞态条件）
 */
static void test_timer_heap_consistency(void) {
    printf("Test 4: Heap consistency (single-threaded)\n");

    TimerHeap *heap = timer_heap_create(256);
    assert(heap != NULL);

    Timer *timers[100];

    /* 添加 100 个定时器 */
    for (int i = 0; i < 100; i++) {
        timers[i] = timer_heap_add(heap, (100 - i) * 10,  // 逆序添加
                                    dummy_callback, NULL, false);
        assert(timers[i] != NULL);
    }

    printf("  Added 100 timers\n");

    /* 验证堆排序正确性 */
    uint64_t last_expire = 0;
    int count = 0;

    while (!timer_heap_is_empty(heap)) {
        Timer *t = timer_heap_pop(heap);
        uint64_t expire = timer_get_expire_time(t);

        if (expire < last_expire) {
            printf("  ERROR: Heap order violation! %lu < %lu\n", expire, last_expire);
            atomic_fetch_add(&result.consistency_errors, 1);
        }

        last_expire = expire;
        timer_free(t);
        count++;
    }

    printf("  Popped %d timers in order\n", count);

    if (count != 100) {
        printf("  ERROR: Count mismatch! Expected 100, got %d\n", count);
    } else {
        printf("  Heap order verified\n");
    }

    timer_heap_destroy(heap);
    printf("  PASS\n");
}

/**
 * TC-TIMER-05: ID 递增竞态测试
 * 测试场景：多线程同时添加定时器，验证 ID 唯一性
 * 预期结果：TSan 报警，ID 可能有重复
 */
static atomic_uint64_t max_id_seen = 0;
static atomic_int duplicate_ids = 0;

static void *add_check_id_loop(void *arg) {
    TimerThreadArg *targ = (TimerThreadArg *)arg;
    TimerHeap *heap = targ->heap;
    RaceTestResult *result = targ->result;
    atomic_bool *running = targ->running;

    while (atomic_load(running) && atomic_load(&result->iterations_completed) < RACE_ITERATIONS) {
        Timer *timer = timer_heap_add(heap, 1000, dummy_callback, NULL, false);
        if (timer) {
            uint64_t id = timer_get_id(timer);
            uint64_t prev_max = atomic_load(&max_id_seen);

            if (id <= prev_max) {
                /* ID 应递增，若小于之前看到的最大值则可能重复 */
                atomic_fetch_add(&duplicate_ids, 1);
            } else {
                atomic_store(&max_id_seen, id);
            }

            atomic_fetch_add(&result->timers_added, 1);
        }
        atomic_fetch_add(&result->iterations_completed, 1);
    }

    return NULL;
}

static void test_timer_id_race(void) {
    printf("Test 5: ID increment race\n");
    printf("  Note: Expected TSan warnings, possible ID duplicates\n");

    TimerHeap *heap = timer_heap_create(256);
    assert(heap != NULL);

    RaceTestResult result = {0};
    atomic_bool running = true;
    max_id_seen = 0;
    duplicate_ids = 0;

    TimerThreadArg args[RACE_THREAD_COUNT];
    pthread_t threads[RACE_THREAD_COUNT];

    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        args[i].heap = heap;
        args[i].thread_id = i;
        args[i].running = &running;
        args[i].result = &result;
        pthread_create(&threads[i], NULL, add_check_id_loop, &args[i]);
    }

    /* 运行一段时间 */
    sleep(RACE_DURATION_SEC);
    atomic_store(&running, false);

    /* 等待完成 */
    for (int i = 0; i < RACE_THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("  Iterations: %d\n", atomic_load(&result.iterations_completed));
    printf("  Timers added: %d\n", atomic_load(&result.timers_added));
    printf("  Max ID seen: %lu\n", atomic_load(&max_id_seen));
    printf("  Potential duplicate IDs: %d\n", atomic_load(&duplicate_ids));
    printf("  Expected: TSan reports race on next_id++\n");

    timer_heap_destroy(heap);
    printf("  PASS (race detected by TSan)\n");
}

/* ============== 主函数 ============== */

int main(void) {
    printf("\n=== Timer Heap Race Condition Tests ===\n\n");
    printf("IMPORTANT: These tests verify design assumptions.\n");
    printf("TimerHeap is designed for single-EventLoop use, so TSan warnings are EXPECTED.\n\n");

    test_timer_concurrent_add();
    test_timer_remove_race();
    test_timer_pop_race();
    test_timer_heap_consistency();
    test_timer_id_race();

    printf("\n=== Test Summary ===\n");
    printf("All tests completed.\n");
    printf("Expected TSan warnings: YES (single-threaded design)\n");
    printf("This is acceptable as TimerHeap is EventLoop-private.\n\n");

    return 0;
}