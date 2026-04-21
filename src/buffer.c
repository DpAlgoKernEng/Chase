/**
 * @file    buffer.c
 * @brief   环形缓冲区实现
 *
 * @details
 *          - 固定容量：超出则丢弃
 *          - 自动扩容：按需扩展，有上限
 *          - 环形结构：高效读写
 *          - 用于 Connection 读写缓冲
 *
 * @layer   Core Layer
 *
 * @depends 无依赖
 * @usedby  connection
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "buffer.h"
#include <stdlib.h>
#include <string.h>

/* 默认最大容量 */
#define DEFAULT_MAX_CAPACITY   (1024 * 1024) /* 1MB */

/* Buffer 结构体 */
struct Buffer {
    char *data;
    size_t capacity;
    size_t size;
    size_t head;
    size_t tail;
    BufferMode mode;
    size_t max_capacity;
};

/* ========== 公共 API 实现 ========== */

Buffer *buffer_create(size_t capacity) {
    return buffer_create_ex(capacity, BUFFER_MODE_FIXED, DEFAULT_MAX_CAPACITY);
}

Buffer *buffer_create_ex(size_t capacity, BufferMode mode, size_t max_cap) {
    Buffer *buf = malloc(sizeof(Buffer));
    if (!buf) return NULL;

    buf->data = malloc(capacity);
    if (!buf->data) {
        free(buf);
        return NULL;
    }

    buf->capacity = capacity;
    buf->size = 0;
    buf->head = 0;
    buf->tail = 0;
    buf->mode = mode;
    buf->max_capacity = max_cap;

    return buf;
}

void buffer_destroy(Buffer *buf) {
    if (!buf) return;
    free(buf->data);
    free(buf);
}

int buffer_write(Buffer *buf, const char *data, size_t len) {
    if (!buf || !data) return -1;
    if (len == 0) return 0;

    /* 检查容量 */
    if (buf->size + len > buf->capacity) {
        if (buf->mode == BUFFER_MODE_FIXED) {
            return -1;  /* 固定模式，溢出 */
        }
        /* AUTO 模式扩容 */
        size_t new_cap = buf->capacity * 2;
        while (new_cap < buf->size + len) {
            new_cap *= 2;
        }
        if (new_cap > buf->max_capacity) {
            new_cap = buf->max_capacity;
        }
        if (buf->size + len > new_cap) {
            return -1;  /* 达到最大容量 */
        }

        /* 线性化并扩容 */
        char *new_data = malloc(new_cap);
        if (!new_data) return -1;

        if (buf->size > 0) {
            if (buf->tail > buf->head) {
                /* 连续数据 */
                memcpy(new_data, buf->data + buf->head, buf->size);
            } else {
                /* 环绕数据 */
                size_t first_part = buf->capacity - buf->head;
                memcpy(new_data, buf->data + buf->head, first_part);
                memcpy(new_data + first_part, buf->data, buf->tail);
            }
        }

        free(buf->data);
        buf->data = new_data;
        buf->capacity = new_cap;
        buf->head = 0;
        buf->tail = buf->size;
    }

    /* 写入数据（考虑环绕） */
    size_t first_write = len;
    if (buf->tail + len > buf->capacity) {
        first_write = buf->capacity - buf->tail;
    }

    memcpy(buf->data + buf->tail, data, first_write);

    if (first_write < len) {
        /* 环绕写入 */
        memcpy(buf->data, data + first_write, len - first_write);
        buf->tail = len - first_write;
    } else {
        buf->tail = (buf->tail + first_write) % buf->capacity;
    }

    buf->size += len;
    return 0;
}

int buffer_read(Buffer *buf, char *data, size_t len) {
    if (!buf || !data) return -1;
    if (buf->size == 0) return 0;

    size_t to_read = len > buf->size ? buf->size : len;

    size_t first_read = to_read;
    if (buf->head + to_read > buf->capacity) {
        first_read = buf->capacity - buf->head;
    }

    memcpy(data, buf->data + buf->head, first_read);

    if (first_read < to_read) {
        /* 环绕读取 */
        memcpy(data + first_read, buf->data, to_read - first_read);
        buf->head = to_read - first_read;
    } else {
        buf->head = (buf->head + first_read) % buf->capacity;
    }

    buf->size -= to_read;
    return (int)to_read;
}

size_t buffer_available(Buffer *buf) {
    if (!buf) return 0;
    return buf->size;
}

size_t buffer_capacity(Buffer *buf) {
    if (!buf) return 0;
    return buf->capacity;
}

size_t buffer_remaining(Buffer *buf) {
    if (!buf) return 0;
    return buf->capacity - buf->size;
}

bool buffer_is_empty(Buffer *buf) {
    if (!buf) return true;
    return buf->size == 0;
}

bool buffer_is_full(Buffer *buf) {
    if (!buf) return false;
    return buf->size >= buf->capacity;
}

void buffer_clear(Buffer *buf) {
    if (!buf) return;
    buf->size = 0;
    buf->head = 0;
    buf->tail = 0;
}

const char *buffer_peek(Buffer *buf, size_t *len) {
    if (!buf || !len) {
        if (len) *len = 0;
        return NULL;
    }
    if (buf->size == 0) {
        *len = 0;
        return NULL;
    }

    /* 返回连续可读的数据长度 */
    /* 特殊情况：满缓冲区时 tail == head，但 size == capacity */
    if (buf->size == buf->capacity) {
        /* 满缓冲区，数据可能环绕，返回从 head 到缓冲区末尾的长度 */
        *len = buf->capacity - buf->head;
    } else if (buf->tail > buf->head) {
        /* 连续数据（未环绕） */
        *len = buf->size;
    } else {
        /* 环绕数据，返回第一部分长度 */
        *len = buf->capacity - buf->head;
    }

    return buf->data + buf->head;
}

size_t buffer_skip(Buffer *buf, size_t len) {
    if (!buf || buf->size == 0) return 0;

    size_t to_skip = len > buf->size ? buf->size : len;

    if (buf->head + to_skip > buf->capacity) {
        buf->head = to_skip - (buf->capacity - buf->head);
    } else {
        buf->head = (buf->head + to_skip) % buf->capacity;
    }

    buf->size -= to_skip;
    return to_skip;
}