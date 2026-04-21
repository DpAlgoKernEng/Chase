/**
 * @file    buffer.h
 * @brief   环形缓冲区，支持固定容量和自动扩容模式
 *
 * @details
 *          - 固定容量模式：超出则丢弃数据
 *          - 自动扩容模式：按需扩展，有上限
 *          - 环形结构，零拷贝读写
 *          - 用于 Connection 的读写缓冲
 *
 * @layer   Core Layer
 *
 * @depends 无依赖
 * @usedby  connection
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#ifndef CHASE_BUFFER_H
#define CHASE_BUFFER_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 缓冲区模式 */
typedef enum {
    BUFFER_MODE_FIXED,     /* 固定容量（安全优先） */
    BUFFER_MODE_AUTO       /* 自动扩容（灵活性优先） */
} BufferMode;

/* Buffer 结构体（不透明指针） */
typedef struct Buffer Buffer;

/* 默认配置 */
#define BUFFER_DEFAULT_READ_CAP   (16 * 1024)   /* 16KB */
#define BUFFER_DEFAULT_WRITE_CAP  (64 * 1024)   /* 64KB */
#define BUFFER_DEFAULT_MAX_CAP    (1024 * 1024) /* 1MB */

/**
 * 创建缓冲区
 * @param capacity 初始容量
 * @return Buffer 指针，失败返回 NULL
 */
Buffer *buffer_create(size_t capacity);

/**
 * 创建缓冲区（扩展参数）
 * @param capacity 初始容量
 * @param mode 扩容模式
 * @param max_cap 最大容量
 * @return Buffer 指针，失败返回 NULL
 */
Buffer *buffer_create_ex(size_t capacity, BufferMode mode, size_t max_cap);

/**
 * 销毁缓冲区
 * @param buf Buffer 指针
 */
void buffer_destroy(Buffer *buf);

/**
 * 写入数据到缓冲区
 * @param buf Buffer 指针
 * @param data 数据
 * @param len 长度
 * @return 0 成功，-1 失败（溢出）
 */
int buffer_write(Buffer *buf, const char *data, size_t len);

/**
 * 从缓冲区读取数据
 * @param buf Buffer 指针
 * @param data 目标缓冲区
 * @param len 要读取的长度
 * @return 实际读取的字节数
 */
int buffer_read(Buffer *buf, char *data, size_t len);

/**
 * 获取可读数据量
 * @param buf Buffer 指针
 * @return 可读字节数
 */
size_t buffer_available(Buffer *buf);

/**
 * 获取缓冲区容量
 * @param buf Buffer 指针
 * @return 容量
 */
size_t buffer_capacity(Buffer *buf);

/**
 * 获取剩余可写空间
 * @param buf Buffer 指针
 * @return 可写字节数
 */
size_t buffer_remaining(Buffer *buf);

/**
 * 检查缓冲区是否为空
 * @param buf Buffer 指针
 * @return true 空，false 非空
 */
bool buffer_is_empty(Buffer *buf);

/**
 * 检查缓冲区是否已满
 * @param buf Buffer 指针
 * @return true 满，false 未满
 */
bool buffer_is_full(Buffer *buf);

/**
 * 清空缓冲区（重置索引，不释放内存）
 * @param buf Buffer 指针
 */
void buffer_clear(Buffer *buf);

/**
 * 获取缓冲区数据指针（用于直接读取，不移动索引）
 * @param buf Buffer 指针
 * @param len 输出：可连续读取的长度
 * @return 数据起始指针
 */
const char *buffer_peek(Buffer *buf, size_t *len);

/**
 * 跳过指定字节数（移动读取索引）
 * @param buf Buffer 指针
 * @param len 要跳过的字节数
 * @return 实际跳过的字节数
 */
size_t buffer_skip(Buffer *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_BUFFER_H */