#include "timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

/* 测试回调 */
static int callback_count = 0;
static void test_callback(void *user_data) {
    callback_count++;
    printf("Timer callback called (count=%d)\n", callback_count);
}

/* 测试 1: 创建和销毁堆 */
static void test_heap_create(void) {
    printf("Test 1: TimerHeap create/destroy\n");

    TimerHeap *heap = timer_heap_create(16);
    assert(heap != NULL);
    assert(timer_heap_is_empty(heap));
    assert(timer_heap_size(heap) == 0);

    timer_heap_destroy(heap);
    printf("  PASS\n");
}

/* 测试 2: 添加定时器 */
static void test_timer_add(void) {
    printf("Test 2: Timer add\n");

    TimerHeap *heap = timer_heap_create(16);

    Timer *t1 = timer_heap_add(heap, 1000, test_callback, NULL, false);
    assert(t1 != NULL);
    assert(timer_get_id(t1) == 1);
    assert(!timer_heap_is_empty(heap));
    assert(timer_heap_size(heap) == 1);

    Timer *t2 = timer_heap_add(heap, 500, test_callback, NULL, false);
    assert(t2 != NULL);
    assert(timer_get_id(t2) == 2);
    assert(timer_heap_size(heap) == 2);

    /* t2 应该是堆顶（过期时间更短） */
    Timer *top = timer_heap_peek(heap);
    assert(top == t2);

    timer_heap_destroy(heap);
    printf("  PASS\n");
}

/* 测试 3: 弹出定时器 */
static void test_timer_pop(void) {
    printf("Test 3: Timer pop\n");

    TimerHeap *heap = timer_heap_create(16);

    Timer *t1 = timer_heap_add(heap, 1000, test_callback, NULL, false);
    Timer *t2 = timer_heap_add(heap, 500, test_callback, NULL, false);
    Timer *t3 = timer_heap_add(heap, 2000, test_callback, NULL, false);

    /* 按过期时间顺序弹出 */
    Timer *pop1 = timer_heap_pop(heap);
    assert(pop1 == t2);  /* 500ms */

    Timer *pop2 = timer_heap_pop(heap);
    assert(pop2 == t1);  /* 1000ms */

    Timer *pop3 = timer_heap_pop(heap);
    assert(pop3 == t3);  /* 2000ms */

    assert(timer_heap_is_empty(heap));

    timer_heap_destroy(heap);
    printf("  PASS\n");
}

/* 测试 4: 移除定时器 */
static void test_timer_remove(void) {
    printf("Test 4: Timer remove\n");

    TimerHeap *heap = timer_heap_create(16);

    Timer *t1 = timer_heap_add(heap, 1000, test_callback, NULL, false);
    Timer *t2 = timer_heap_add(heap, 500, test_callback, NULL, false);
    Timer *t3 = timer_heap_add(heap, 2000, test_callback, NULL, false);

    /* 移除中间的定时器 */
    int result = timer_heap_remove(heap, t1);
    assert(result == 0);
    assert(timer_heap_size(heap) == 2);

    /* 堆顶应该是 t2 */
    Timer *top = timer_heap_peek(heap);
    assert(top == t2);

    timer_heap_destroy(heap);
    printf("  PASS\n");
}

/* 测试 5: 空堆 pop */
static void test_timer_pop_empty(void) {
    printf("Test 5: Timer pop empty heap\n");

    TimerHeap *heap = timer_heap_create(16);

    Timer *t = timer_heap_pop(heap);
    assert(t == NULL);

    timer_heap_destroy(heap);
    printf("  PASS\n");
}

/* 测试 6: 空堆 peek */
static void test_timer_peek_empty(void) {
    printf("Test 6: Timer peek empty heap\n");

    TimerHeap *heap = timer_heap_create(16);

    Timer *t = timer_heap_peek(heap);
    assert(t == NULL);

    timer_heap_destroy(heap);
    printf("  PASS\n");
}

/* 测试 7: 移除已弹出的定时器 */
static void test_timer_remove_popped(void) {
    printf("Test 7: Timer remove popped timer\n");

    TimerHeap *heap = timer_heap_create(16);
    Timer *t1 = timer_heap_add(heap, 1000, test_callback, NULL, false);

    /* 弹出后 heap_index 变为 -1 */
    Timer *popped = timer_heap_pop(heap);
    assert(popped == t1);

    /* 尝试移除已弹出的定时器 */
    int result = timer_heap_remove(heap, t1);
    assert(result == -1);  /* 应失败，因为 heap_index < 0 */

    timer_heap_destroy(heap);
    printf("  PASS\n");
}

/* 测试 8: 定时器 ID 递增 */
static void test_timer_id_increment(void) {
    printf("Test 8: Timer ID increment\n");

    TimerHeap *heap = timer_heap_create(16);

    Timer *t1 = timer_heap_add(heap, 1000, test_callback, NULL, false);
    Timer *t2 = timer_heap_add(heap, 500, test_callback, NULL, false);
    Timer *t3 = timer_heap_add(heap, 2000, test_callback, NULL, false);

    assert(timer_get_id(t1) == 1);
    assert(timer_get_id(t2) == 2);
    assert(timer_get_id(t3) == 3);

    timer_heap_destroy(heap);
    printf("  PASS\n");
}

/* 测试 9: 过期时间 */
static void test_timer_expire_time(void) {
    printf("Test 9: Timer expire time\n");

    TimerHeap *heap = timer_heap_create(16);

    uint64_t now = 0;  /* 简化测试 */
    Timer *t = timer_heap_add(heap, 1000, test_callback, NULL, false);

    uint64_t expire = timer_get_expire_time(t);
    assert(expire > 0);  /* 应有过期时间 */

    timer_heap_destroy(heap);
    printf("  PASS\n");
}

/* 测试 10: 大量定时器 */
static void test_timer_many_timers(void) {
    printf("Test 10: Timer many timers\n");

    TimerHeap *heap = timer_heap_create(256);

    /* 添加 100 个定时器 */
    for (int i = 0; i < 100; i++) {
        Timer *t = timer_heap_add(heap, i * 100, test_callback, NULL, false);
        assert(t != NULL);
    }

    assert(timer_heap_size(heap) == 100);

    /* 弹出应按过期时间排序 */
    uint64_t last_expire = 0;
    while (!timer_heap_is_empty(heap)) {
        Timer *t = timer_heap_pop(heap);
        uint64_t expire = timer_get_expire_time(t);
        assert(expire >= last_expire);
        last_expire = expire;
    }

    timer_heap_destroy(heap);
    printf("  PASS\n");
}

int main(void) {
    printf("=== Timer Module Tests ===\n\n");

    test_heap_create();
    test_timer_add();
    test_timer_pop();
    test_timer_remove();
    test_timer_pop_empty();
    test_timer_peek_empty();
    test_timer_remove_popped();
    test_timer_id_increment();
    test_timer_expire_time();
    test_timer_many_timers();

    printf("\n=== All tests passed ===\n");
    return 0;
}