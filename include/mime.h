/**
 * @file    mime.h
 * @brief   MIME 类型注册表，根据文件扩展名推断 MIME 类型
 *
 * @details
 *          - 内置常用 MIME 类型映射
 *          - 支持自定义扩展名映射
 *          - 从文件路径推断类型
 *          - 零依赖模块
 *
 * @layer   Core Layer
 *
 * @depends 无依赖
 * @usedby  fileserve, handler
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#ifndef CHASE_MIME_H
#define CHASE_MIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 常用 MIME 类型常量 */
#define MIME_TEXT_HTML       "text/html"
#define MIME_TEXT_CSS        "text/css"
#define MIME_TEXT_JS         "text/javascript"
#define MIME_TEXT_JSON       "application/json"
#define MIME_TEXT_PLAIN      "text/plain"
#define MIME_IMAGE_JPEG      "image/jpeg"
#define MIME_IMAGE_PNG       "image/png"
#define MIME_IMAGE_GIF       "image/gif"
#define MIME_IMAGE_SVG       "image/svg+xml"
#define MIME_VIDEO_MP4       "video/mp4"
#define MIME_AUDIO_MP3       "audio/mpeg"
#define MIME_APP_PDF         "application/pdf"
#define MIME_APP_ZIP         "application/zip"
#define MIME_APP_OCTET       "application/octet-stream"

/* MIME 类型结果 */
typedef struct {
    const char *type;       /* MIME 类型字符串 */
    const char *charset;    /* 字符集（可选） */
} MimeType;

/* MimeRegistry 结构体（不透明指针） */
typedef struct MimeRegistry MimeRegistry;

/**
 * 创建 MIME 注册表（使用默认映射）
 * @return MimeRegistry 指针，失败返回 NULL
 */
MimeRegistry *mime_registry_create(void);

/**
 * 销毁 MIME 注册表
 * @param registry MimeRegistry 指针
 */
void mime_registry_destroy(MimeRegistry *registry);

/**
 * 根据文件扩展名获取 MIME 类型
 * @param registry MimeRegistry 指针
 * @param extension 文件扩展名（如 ".html"）
 * @return MIME 类型字符串
 */
const char *mime_registry_get_type(MimeRegistry *registry, const char *extension);

/**
 * 根据文件路径获取 MIME 类型
 * @param registry MimeRegistry 指针
 * @param path 文件路径
 * @return MimeType 结构
 */
MimeType mime_registry_get_type_from_path(MimeRegistry *registry, const char *path);

/**
 * 添加自定义 MIME 类型映射
 * @param registry MimeRegistry 指针
 * @param extension 文件扩展名（如 ".xyz"）
 * @param mime_type MIME 类型字符串
 * @return 0 成功，-1 失败
 */
int mime_registry_add_type(MimeRegistry *registry, const char *extension, const char *mime_type);

/**
 * 根据文件路径获取 MIME 类型（使用默认映射，无需 registry）
 * @param path 文件路径
 * @return MIME 类型字符串
 */
const char *mime_get_type_by_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_MIME_H */