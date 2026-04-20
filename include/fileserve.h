#ifndef CHASE_FILESERVE_H
#define CHASE_FILESERVE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 文件服务结果 */
typedef enum {
    FILESERVE_OK,               /* 成功 */
    FILESERVE_NOT_FOUND,        /* 文件不存在 */
    FILESERVE_FORBIDDEN,        /* 权限拒绝（路径穿越等） */
    FILESERVE_INTERNAL_ERROR,   /* 内部错误 */
    FILESERVE_RANGE_INVALID     /* Range 请求无效 */
} FileServeResult;

/* Range 请求信息 */
typedef struct {
    bool has_range;             /* 是否有 Range 请求 */
    uint64_t start;             /* 起始位置 */
    uint64_t end;               /* 结束位置 */
    uint64_t total_size;        /* 文件总大小 */
} RangeInfo;

/* MIME 类型推断结果 */
typedef struct {
    const char *type;           /* MIME 类型字符串 */
    const char *charset;        /* 字符集（可选） */
} MimeType;

/* 文件信息 */
typedef struct {
    char *path;                 /* 文件路径（已验证） */
    uint64_t size;              /* 文件大小 */
    MimeType mime;              /* MIME 类型 */
    bool is_readable;           /* 是否可读 */
} FileInfo;

/* FileServe 结构体（不透明指针） */
typedef struct FileServe FileServe;

/* 文件读取回调 */
typedef void (*FileReadCallback)(const char *data, size_t len, void *user_data);

/**
 * 创建文件服务模块
 * @param root_dir 静态文件根目录
 * @return FileServe 指针，失败返回 NULL
 */
FileServe *fileserve_create(const char *root_dir);

/**
 * 销毁文件服务模块
 * @param fs FileServe 指针
 */
void fileserve_destroy(FileServe *fs);

/**
 * 设置根目录
 * @param fs FileServe 指针
 * @param root_dir 根目录路径
 * @return 0 成功，-1 失败
 */
int fileserve_set_root_dir(FileServe *fs, const char *root_dir);

/**
 * 获取根目录
 * @param fs FileServe 指针
 * @return 根目录路径
 */
const char *fileserve_get_root_dir(FileServe *fs);

/**
 * 解析并验证请求路径（防止路径穿越）
 * @param fs FileServe 挝针
 * @param request_path HTTP 请求路径
 * @param resolved_path 输出：解析后的真实路径（需预分配）
 * @param max_len 最大长度
 * @return FileServeResult 结果
 */
FileServeResult fileserve_resolve_path(FileServe *fs, const char *request_path,
                                        char *resolved_path, size_t max_len);

/**
 * 获取文件信息
 * @param fs FileServe 指针
 * @param path 文件路径（已验证）
 * @param info 输出：文件信息
 * @return FileServeResult 结果
 */
FileServeResult fileserve_get_file_info(FileServe *fs, const char *path, FileInfo *info);

/**
 * 推断 MIME 类型
 * @param path 文件路径
 * @return MIME 类型结构
 */
MimeType fileserve_get_mime_type(const char *path);

/**
 * 解析 Range 请求头
 * @param range_header Range 头值（如 "bytes=0-1023"）
 * @param file_size 文件总大小
 * @param range_info 输出：Range 信息
 * @return 0 成功，-1 失败
 */
int fileserve_parse_range(const char *range_header, uint64_t file_size, RangeInfo *range_info);

/**
 * 发送文件（使用 sendfile 零拷贝）
 * @param fs FileServe 指针
 * @param fd 目标文件描述符（连接）
 * @param path 文件路径（已验证）
 * @param range Range 信息（可选，NULL 表示发送整个文件）
 * @param bytes_sent 输出：发送的字节数
 * @return FileServeResult 结果
 */
FileServeResult fileserve_send_file(FileServe *fs, int fd, const char *path,
                                     RangeInfo *range, uint64_t *bytes_sent);

/**
 * 读取文件内容（用于小文件或不支持 sendfile 的场景）
 * @param fs FileServe 挝针
 * @param path 文件路径（已验证）
 * @param offset 起始偏移
 * @param length 读取长度
 * @param callback 数据回调
 * @param user_data 用户数据
 * @return FileServeResult 结果
 */
FileServeResult fileserve_read_file(FileServe *fs, const char *path,
                                     uint64_t offset, size_t length,
                                     FileReadCallback callback, void *user_data);

/**
 * 检查路径是否安全（防止路径穿越）
 * @param resolved_path 已解析的真实路径
 * @param root_dir 根目录
 * @return true 安全，false 不安全
 */
bool fileserve_is_path_safe(const char *resolved_path, const char *root_dir);

/**
 * 添加自定义 MIME 类型映射
 * @param fs FileServe 指针
 * @param extension 文件扩展名（如 ".json"）
 * @param mime_type MIME 类型（如 "application/json"）
 * @return 0 成功，-1 失败
 */
int fileserve_add_mime_type(FileServe *fs, const char *extension, const char *mime_type);

/* 常用 MIME 类型常量 */
#define MIME_TEXT_HTML       "text/html"
#define MIME_TEXT_CSS        "text/css"
#define MIME_TEXT_JS         "application/javascript"
#define MIME_TEXT_PLAIN      "text/plain"
#define MIME_TEXT_JSON       "application/json"
#define MIME_IMAGE_JPEG      "image/jpeg"
#define MIME_IMAGE_PNG       "image/png"
#define MIME_IMAGE_GIF       "image/gif"
#define MIME_IMAGE_SVG       "image/svg+xml"
#define MIME_VIDEO_MP4       "video/mp4"
#define MIME_AUDIO_MP3       "audio/mpeg"
#define MIME_APP_PDF         "application/pdf"
#define MIME_APP_ZIP         "application/zip"
#define MIME_APP_OCTET       "application/octet-stream"

#ifdef __cplusplus
}
#endif

#endif /* CHASE_FILESERVE_H */