/**
 * @file    mime.c
 * @brief   MIME 类型注册表实现
 *
 * @details
 *          - 内置常用 MIME 类型映射
 *          - 支持自定义扩展名映射
 *          - 从文件路径推断类型
 *
 * @layer   Core Layer
 *
 * @depends 无依赖
 * @usedby  fileserve, handler
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "mime.h"
#include <stdlib.h>
#include <string.h>

/* MIME 类型映射 */
typedef struct {
    const char *extension;
    const char *mime_type;
} MimeMapping;

/* 默认 MIME 类型映射表 */
static const MimeMapping default_mime_types[] = {
    {".html",  MIME_TEXT_HTML},
    {".htm",   MIME_TEXT_HTML},
    {".css",   MIME_TEXT_CSS},
    {".js",    MIME_TEXT_JS},
    {".json",  MIME_TEXT_JSON},
    {".txt",   MIME_TEXT_PLAIN},
    {".xml",   "application/xml"},
    {".jpg",   MIME_IMAGE_JPEG},
    {".jpeg",  MIME_IMAGE_JPEG},
    {".png",   MIME_IMAGE_PNG},
    {".gif",   MIME_IMAGE_GIF},
    {".svg",   MIME_IMAGE_SVG},
    {".ico",   "image/x-icon"},
    {".mp4",   MIME_VIDEO_MP4},
    {".webm",  "video/webm"},
    {".mp3",   MIME_AUDIO_MP3},
    {".wav",   "audio/wav"},
    {".pdf",   MIME_APP_PDF},
    {".zip",   MIME_APP_ZIP},
    {".gz",    "application/gzip"},
    {".tar",   "application/x-tar"},
    {".doc",   "application/msword"},
    {".docx",  "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".xls",   "application/vnd.ms-excel"},
    {".xlsx",  "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".ppt",   "application/vnd.ms-powerpoint"},
    {".pptx",  "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".woff",  "font/woff"},
    {".woff2", "font/woff2"},
    {".ttf",   "font/ttf"},
    {".eot",   "application/vnd.ms-fontobject"},
    {NULL, NULL}  /* 终止符 */
};

/* 自定义 MIME 类型映射链表节点 */
typedef struct CustomMimeNode {
    char *extension;
    char *mime_type;
    struct CustomMimeNode *next;
} CustomMimeNode;

/* MimeRegistry 结构体 */
struct MimeRegistry {
    CustomMimeNode *custom_head;   /* 自定义 MIME 类型链表 */
};

/* ========== 内部辅助函数 ========== */

/* 在默认映射表中查找 */
static const char *find_in_default(const char *extension) {
    for (int i = 0; default_mime_types[i].extension != NULL; i++) {
        if (strcmp(default_mime_types[i].extension, extension) == 0) {
            return default_mime_types[i].mime_type;
        }
    }
    return MIME_APP_OCTET;
}

/* ========== 公共 API 实现 ========== */

MimeRegistry *mime_registry_create(void) {
    MimeRegistry *registry = malloc(sizeof(MimeRegistry));
    if (!registry) return NULL;

    registry->custom_head = NULL;

    return registry;
}

void mime_registry_destroy(MimeRegistry *registry) {
    if (!registry) return;

    /* 释放自定义 MIME 类型链表 */
    CustomMimeNode *node = registry->custom_head;
    while (node) {
        CustomMimeNode *next = node->next;
        free(node->extension);
        free(node->mime_type);
        free(node);
        node = next;
    }

    free(registry);
}

const char *mime_registry_get_type(MimeRegistry *registry, const char *extension) {
    if (!registry || !extension) return MIME_APP_OCTET;

    /* 先查找自定义映射 */
    CustomMimeNode *node = registry->custom_head;
    while (node) {
        if (strcmp(node->extension, extension) == 0) {
            return node->mime_type;
        }
        node = node->next;
    }

    /* 查找默认映射 */
    return find_in_default(extension);
}

MimeType mime_registry_get_type_from_path(MimeRegistry *registry, const char *path) {
    MimeType result = {MIME_APP_OCTET, NULL};

    if (!path) return result;

    /* 查找扩展名 */
    const char *ext = strrchr(path, '.');
    if (!ext) return result;

    result.type = mime_registry_get_type(registry, ext);
    return result;
}

int mime_registry_add_type(MimeRegistry *registry, const char *extension, const char *mime_type) {
    if (!registry || !extension || !mime_type) return -1;

    CustomMimeNode *node = malloc(sizeof(CustomMimeNode));
    if (!node) return -1;

    node->extension = strdup(extension);
    node->mime_type = strdup(mime_type);
    if (!node->extension || !node->mime_type) {
        free(node->extension);
        free(node->mime_type);
        free(node);
        return -1;
    }

    node->next = registry->custom_head;
    registry->custom_head = node;

    return 0;
}

const char *mime_get_type_by_path(const char *path) {
    if (!path) return MIME_APP_OCTET;

    /* 查找扩展名 */
    const char *ext = strrchr(path, '.');
    if (!ext) return MIME_APP_OCTET;

    return find_in_default(ext);
}