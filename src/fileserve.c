#include "fileserve.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#ifdef __linux__
#include <sys/sendfile.h>
#endif

/* ========== MIME 类型映射 ========== */

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

/* ========== FileServe 结构体 ========== */

struct FileServe {
    char *root_dir;                     /* 根目录 */
    size_t root_dir_len;                /* 根目录长度 */
    CustomMimeNode *custom_mime_head;   /* 自定义 MIME 类型链表 */
};

/* ========== 辅助函数 ========== */

/* 查找扩展名对应的 MIME 类型 */
static const char *find_mime_type(FileServe *fs, const char *extension) {
    /* 先查找自定义映射 */
    CustomMimeNode *node = fs->custom_mime_head;
    while (node) {
        if (strcmp(node->extension, extension) == 0) {
            return node->mime_type;
        }
        node = node->next;
    }

    /* 查找默认映射 */
    for (int i = 0; default_mime_types[i].extension != NULL; i++) {
        if (strcmp(default_mime_types[i].extension, extension) == 0) {
            return default_mime_types[i].mime_type;
        }
    }

    /* 默认类型 */
    return MIME_APP_OCTET;
}

/* ========== API 实现 ========== */

FileServe *fileserve_create(const char *root_dir) {
    FileServe *fs = malloc(sizeof(FileServe));
    if (!fs) return NULL;

    /* 使用 realpath 解析根目录，确保路径一致性 */
    char *real_root = realpath(root_dir, NULL);
    if (!real_root) {
        free(fs);
        return NULL;
    }

    fs->root_dir = real_root;
    fs->root_dir_len = strlen(real_root);
    fs->custom_mime_head = NULL;

    return fs;
}

void fileserve_destroy(FileServe *fs) {
    if (!fs) return;

    free(fs->root_dir);

    /* 释放自定义 MIME 类型链表 */
    CustomMimeNode *node = fs->custom_mime_head;
    while (node) {
        CustomMimeNode *next = node->next;
        free(node->extension);
        free(node->mime_type);
        free(node);
        node = next;
    }

    free(fs);
}

int fileserve_set_root_dir(FileServe *fs, const char *root_dir) {
    if (!fs || !root_dir) return -1;

    char *new_dir = strdup(root_dir);
    if (!new_dir) return -1;

    free(fs->root_dir);
    fs->root_dir = new_dir;
    fs->root_dir_len = strlen(root_dir);

    return 0;
}

const char *fileserve_get_root_dir(FileServe *fs) {
    if (!fs) return NULL;
    return fs->root_dir;
}

FileServeResult fileserve_resolve_path(FileServe *fs, const char *request_path,
                                        char *resolved_path, size_t max_len) {
    if (!fs || !request_path || !resolved_path || max_len == 0) {
        return FILESERVE_INTERNAL_ERROR;
    }

    /* 构建完整路径 */
    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s%s", fs->root_dir, request_path);

    /* 使用 realpath 解析真实路径 */
    char *real = realpath(full_path, NULL);
    if (!real) {
        if (errno == ENOENT) {
            return FILESERVE_NOT_FOUND;
        }
        if (errno == EACCES) {
            return FILESERVE_FORBIDDEN;
        }
        return FILESERVE_INTERNAL_ERROR;
    }

    /* 检查路径是否在根目录内（防止路径穿越） */
    if (!fileserve_is_path_safe(real, fs->root_dir)) {
        free(real);
        return FILESERVE_FORBIDDEN;
    }

    /* 复制结果 */
    size_t real_len = strlen(real);
    if (real_len >= max_len) {
        free(real);
        return FILESERVE_INTERNAL_ERROR;
    }

    strncpy(resolved_path, real, max_len - 1);
    resolved_path[max_len - 1] = '\0';

    free(real);
    return FILESERVE_OK;
}

bool fileserve_is_path_safe(const char *resolved_path, const char *root_dir) {
    if (!resolved_path || !root_dir) return false;

    size_t root_len = strlen(root_dir);

    /* 检查解析后的路径是否以根目录开头 */
    if (strncmp(resolved_path, root_dir, root_len) != 0) {
        return false;
    }

    /* 检查根目录后是否是路径分隔符或结束 */
    if (resolved_path[root_len] != '/' && resolved_path[root_len] != '\0') {
        return false;
    }

    return true;
}

FileServeResult fileserve_get_file_info(FileServe *fs, const char *path, FileInfo *info) {
    if (!fs || !path || !info) {
        return FILESERVE_INTERNAL_ERROR;
    }

    struct stat st;
    if (stat(path, &st) < 0) {
        if (errno == ENOENT) {
            return FILESERVE_NOT_FOUND;
        }
        if (errno == EACCES) {
            return FILESERVE_FORBIDDEN;
        }
        return FILESERVE_INTERNAL_ERROR;
    }

    /* 检查是否是普通文件 */
    if (!S_ISREG(st.st_mode)) {
        return FILESERVE_FORBIDDEN;
    }

    info->path = strdup(path);
    if (!info->path) {
        return FILESERVE_INTERNAL_ERROR;
    }

    info->size = st.st_size;
    info->is_readable = (st.st_mode & S_IRUSR) != 0;

    /* 推断 MIME 类型 */
    const char *ext = strrchr(path, '.');
    if (ext) {
        info->mime.type = find_mime_type(fs, ext);
        info->mime.charset = NULL;
    } else {
        info->mime.type = MIME_APP_OCTET;
        info->mime.charset = NULL;
    }

    return FILESERVE_OK;
}

MimeType fileserve_get_mime_type(const char *path) {
    MimeType result = {MIME_APP_OCTET, NULL};

    if (!path) return result;

    /* 查找扩展名 */
    const char *ext = strrchr(path, '.');
    if (!ext) return result;

    /* 返回默认 MIME 类型结果，实际查找需要 FileServe 对象 */
    result.type = MIME_APP_OCTET;
    return result;
}

int fileserve_parse_range(const char *range_header, uint64_t file_size, RangeInfo *range_info) {
    if (!range_header || !range_info) return -1;

    /* Range 格式: "bytes=start-end" 或 "bytes=start-" 或 "bytes=-suffix" */
    if (strncmp(range_header, "bytes=", 6) != 0) {
        return -1;
    }

    const char *range_spec = range_header + 6;
    range_info->has_range = true;
    range_info->total_size = file_size;

    /* 解析范围 */
    const char *dash = strchr(range_spec, '-');
    if (!dash) {
        range_info->has_range = false;
        return -1;
    }

    const char *start_str = range_spec;
    const char *end_str = dash + 1;

    /* 后缀范围: "-suffix" 表示最后 suffix 字节（- 在开头）*/
    if (dash == range_spec) {
        if (*end_str == '\0') {
            range_info->has_range = false;
            return -1;
        }
        uint64_t suffix = strtoull(end_str, NULL, 10);
        range_info->start = (suffix > file_size) ? 0 : (file_size - suffix);
        range_info->end = file_size - 1;
    } else if (*end_str == '\0') {
        /* 开放范围: "start-" 表示从 start 到文件末尾 */
        range_info->start = strtoull(start_str, NULL, 10);
        range_info->end = file_size - 1;
    } else {
        /* 指定范围: "start-end" */
        range_info->start = strtoull(start_str, NULL, 10);
        range_info->end = strtoull(end_str, NULL, 10);
    }

    /* 验证范围有效性 */
    if (range_info->start > range_info->end ||
        range_info->start >= file_size ||
        range_info->end >= file_size) {
        range_info->has_range = false;
        return -1;
    }

    return 0;
}

FileServeResult fileserve_send_file(FileServe *fs, int fd, const char *path,
                                     RangeInfo *range, uint64_t *bytes_sent) {
    if (!fs || fd < 0 || !path) {
        return FILESERVE_INTERNAL_ERROR;
    }

    /* 打开文件 */
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        if (errno == ENOENT) {
            return FILESERVE_NOT_FOUND;
        }
        if (errno == EACCES) {
            return FILESERVE_FORBIDDEN;
        }
        return FILESERVE_INTERNAL_ERROR;
    }

    /* 获取文件大小 */
    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        return FILESERVE_INTERNAL_ERROR;
    }

    uint64_t file_size = st.st_size;
    uint64_t offset = 0;
    uint64_t length = file_size;

    /* 处理 Range 请求 */
    if (range && range->has_range) {
        offset = range->start;
        length = range->end - range->start + 1;
    }

    *bytes_sent = 0;

#ifdef __linux__
    /* Linux 使用 sendfile */
    while (*bytes_sent < length) {
        ssize_t sent = sendfile(fd, file_fd, (off_t *)&offset, length - *bytes_sent);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            close(file_fd);
            return FILESERVE_INTERNAL_ERROR;
        }
        if (sent == 0) {
            break;  /* 发送完成 */
        }
        *bytes_sent += sent;
    }
#else
    /* macOS/其他系统使用 read/write */
    char buf[8192];
    while (*bytes_sent < length) {
        size_t to_read = (length - *bytes_sent > sizeof(buf)) ?
                         sizeof(buf) : (length - *bytes_sent);
        ssize_t read_ret = pread(file_fd, buf, to_read, offset + *bytes_sent);
        if (read_ret < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            close(file_fd);
            return FILESERVE_INTERNAL_ERROR;
        }
        if (read_ret == 0) {
            break;
        }

        ssize_t write_ret = write(fd, buf, read_ret);
        if (write_ret < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            close(file_fd);
            return FILESERVE_INTERNAL_ERROR;
        }
        *bytes_sent += write_ret;
    }
#endif

    close(file_fd);
    return FILESERVE_OK;
}

FileServeResult fileserve_read_file(FileServe *fs, const char *path,
                                     uint64_t offset, size_t length,
                                     FileReadCallback callback, void *user_data) {
    if (!fs || !path || !callback) {
        return FILESERVE_INTERNAL_ERROR;
    }

    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        if (errno == ENOENT) {
            return FILESERVE_NOT_FOUND;
        }
        if (errno == EACCES) {
            return FILESERVE_FORBIDDEN;
        }
        return FILESERVE_INTERNAL_ERROR;
    }

    char buf[8192];
    size_t bytes_read = 0;

    while (bytes_read < length) {
        size_t to_read = (length - bytes_read > sizeof(buf)) ?
                         sizeof(buf) : (length - bytes_read);
        ssize_t ret = pread(file_fd, buf, to_read, offset + bytes_read);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            close(file_fd);
            return FILESERVE_INTERNAL_ERROR;
        }
        if (ret == 0) {
            break;
        }

        callback(buf, ret, user_data);
        bytes_read += ret;
    }

    close(file_fd);
    return FILESERVE_OK;
}

int fileserve_add_mime_type(FileServe *fs, const char *extension, const char *mime_type) {
    if (!fs || !extension || !mime_type) return -1;

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

    node->next = fs->custom_mime_head;
    fs->custom_mime_head = node;

    return 0;
}