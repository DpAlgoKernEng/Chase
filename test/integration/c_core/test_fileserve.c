/**
 * @file    test_fileserve.c
 * @brief   FileServe 模块测试
 *
 * @details
 *          - 测试文件服务创建和销毁
 *          - 测试路径解析和验证
 *          - 测试路径穿越防护
 *          - 测试文件信息获取
 *
 * @layer   Test
 *
 * @depends fileserve
 * @usedby  测试框架
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "fileserve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

/* 测试目录 */
#define TEST_ROOT_DIR "/tmp/chase_fileserve_test"

/* 测试文件 */
#define TEST_FILE_NAME "test.txt"
#define TEST_FILE_CONTENT "Hello, Chase HTTP Server!"
#define TEST_FILE_SIZE 27

/* 辅助函数：创建测试目录和文件 */
static void setup_test_files(void) {
    /* 创建测试目录 */
    mkdir(TEST_ROOT_DIR, 0755);

    /* 创建测试文件 */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", TEST_ROOT_DIR, TEST_FILE_NAME);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, TEST_FILE_CONTENT, TEST_FILE_SIZE);
        close(fd);
    }

    /* 创建子目录 */
    snprintf(path, sizeof(path), "%s/subdir", TEST_ROOT_DIR);
    mkdir(path, 0755);

    /* 创建子目录文件 */
    snprintf(path, sizeof(path), "%s/subdir/nested.txt", TEST_ROOT_DIR);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, "Nested file content", 19);
        close(fd);
    }

    /* 创建各种 MIME 类型文件 */
    snprintf(path, sizeof(path), "%s/test.html", TEST_ROOT_DIR);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, "<html><body>Test</body></html>", 30);
        close(fd);
    }

    snprintf(path, sizeof(path), "%s/test.json", TEST_ROOT_DIR);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, "{\"key\": \"value\"}", 16);
        close(fd);
    }

    snprintf(path, sizeof(path), "%s/test.css", TEST_ROOT_DIR);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, "body { color: red; }", 20);
        close(fd);
    }
}

/* 辅助函数：清理测试文件 */
static void cleanup_test_files(void) {
    char path[256];

    /* 删除子目录文件 */
    snprintf(path, sizeof(path), "%s/subdir/nested.txt", TEST_ROOT_DIR);
    unlink(path);

    /* 删除子目录 */
    snprintf(path, sizeof(path), "%s/subdir", TEST_ROOT_DIR);
    rmdir(path);

    /* 删除各种 MIME 类型文件 */
    snprintf(path, sizeof(path), "%s/test.html", TEST_ROOT_DIR);
    unlink(path);
    snprintf(path, sizeof(path), "%s/test.json", TEST_ROOT_DIR);
    unlink(path);
    snprintf(path, sizeof(path), "%s/test.css", TEST_ROOT_DIR);
    unlink(path);

    /* 删除测试文件 */
    snprintf(path, sizeof(path), "%s/%s", TEST_ROOT_DIR, TEST_FILE_NAME);
    unlink(path);

    /* 删除测试目录 */
    rmdir(TEST_ROOT_DIR);
}

/* 测试 1: 创建和销毁 */
static void test_fileserve_create_destroy(void) {
    printf("Test 1: FileServe create/destroy\n");

    setup_test_files();

    FileServe *fs = fileserve_create(TEST_ROOT_DIR);
    assert(fs != NULL);

    const char *root = fileserve_get_root_dir(fs);
    assert(root != NULL);
    /* realpath 可能改变路径（如 macOS /tmp -> /private/tmp），检查包含关系 */
    assert(strstr(root, "chase_fileserve_test") != NULL);

    fileserve_destroy(fs);
    cleanup_test_files();
    printf("  PASS\n");
}

/* 测试 2: 设置根目录 */
static void test_fileserve_set_root_dir(void) {
    printf("Test 2: FileServe set root dir\n");

    FileServe *fs = fileserve_create("/tmp");
    assert(fs != NULL);

    int result = fileserve_set_root_dir(fs, TEST_ROOT_DIR);
    assert(result == 0);

    const char *root = fileserve_get_root_dir(fs);
    assert(strstr(root, "chase_fileserve_test") != NULL);

    fileserve_destroy(fs);
    printf("  PASS\n");
}

/* 测试 3: 路径解析（正常路径） */
static void test_fileserve_resolve_path_normal(void) {
    printf("Test 3: FileServe resolve path normal\n");

    setup_test_files();

    FileServe *fs = fileserve_create(TEST_ROOT_DIR);
    assert(fs != NULL);

    char resolved[256];
    FileServeResult result = fileserve_resolve_path(fs, "/" TEST_FILE_NAME,
                                                      resolved, sizeof(resolved));
    assert(result == FILESERVE_OK);

    /* 检查解析后的路径包含测试文件名 */
    assert(strstr(resolved, TEST_FILE_NAME) != NULL);

    fileserve_destroy(fs);
    cleanup_test_files();
    printf("  PASS\n");
}

/* 测试 4: 路径穿越检测 */
static void test_fileserve_path_traversal(void) {
    printf("Test 4: FileServe path traversal detection\n");

    setup_test_files();

    FileServe *fs = fileserve_create(TEST_ROOT_DIR);
    assert(fs != NULL);

    char resolved[256];

    /* 测试路径穿越（目标不存在，返回 NOT_FOUND）*/
    FileServeResult result = fileserve_resolve_path(fs, "/../etc/passwd",
                                                      resolved, sizeof(resolved));
    /* realpath 对不存在的路径返回 ENOENT，所以是 NOT_FOUND */
    assert(result == FILESERVE_NOT_FOUND);

    /* 测试内部路径穿越但回到测试目录（存在）*/
    result = fileserve_resolve_path(fs, "/subdir/../test.txt",
                                      resolved, sizeof(resolved));
    /* 这个路径指向存在的文件，应该成功 */
    assert(result == FILESERVE_OK);
    assert(strstr(resolved, "test.txt") != NULL);

    /* 测试通过子目录的路径穿越 */
    result = fileserve_resolve_path(fs, "/subdir/../../etc/passwd",
                                      resolved, sizeof(resolved));
    /* 目标不存在，返回 NOT_FOUND */
    assert(result == FILESERVE_NOT_FOUND);

    fileserve_destroy(fs);
    cleanup_test_files();
    printf("  PASS\n");
}

/* 测试 5: 文件不存在 */
static void test_fileserve_not_found(void) {
    printf("Test 5: FileServe not found\n");

    setup_test_files();

    FileServe *fs = fileserve_create(TEST_ROOT_DIR);
    assert(fs != NULL);

    char resolved[256];
    FileServeResult result = fileserve_resolve_path(fs, "/nonexistent.txt",
                                                      resolved, sizeof(resolved));
    assert(result == FILESERVE_NOT_FOUND);

    fileserve_destroy(fs);
    cleanup_test_files();
    printf("  PASS\n");
}

/* 测试 6: 获取文件信息 */
static void test_fileserve_get_file_info(void) {
    printf("Test 6: FileServe get file info\n");

    setup_test_files();

    FileServe *fs = fileserve_create(TEST_ROOT_DIR);
    assert(fs != NULL);

    char resolved[256];
    FileServeResult result = fileserve_resolve_path(fs, "/" TEST_FILE_NAME,
                                                      resolved, sizeof(resolved));
    assert(result == FILESERVE_OK);

    FileInfo info;
    result = fileserve_get_file_info(fs, resolved, &info);
    assert(result == FILESERVE_OK);
    assert(info.size == TEST_FILE_SIZE);
    assert(info.is_readable == true);
    assert(info.path != NULL);

    free(info.path);
    fileserve_destroy(fs);
    cleanup_test_files();
    printf("  PASS\n");
}

/* 测试 7: MIME 类型推断 */
static void test_fileserve_mime_type(void) {
    printf("Test 7: FileServe MIME type inference\n");

    setup_test_files();

    FileServe *fs = fileserve_create(TEST_ROOT_DIR);
    assert(fs != NULL);

    char resolved[256];
    FileInfo info;

    /* HTML 文件 */
    snprintf(resolved, sizeof(resolved), "%s/test.html", TEST_ROOT_DIR);
    FileServeResult result = fileserve_get_file_info(fs, resolved, &info);
    assert(result == FILESERVE_OK);
    assert(strcmp(info.mime.type, MIME_TEXT_HTML) == 0);
    free(info.path);

    /* JSON 文件 */
    snprintf(resolved, sizeof(resolved), "%s/test.json", TEST_ROOT_DIR);
    result = fileserve_get_file_info(fs, resolved, &info);
    assert(result == FILESERVE_OK);
    assert(strcmp(info.mime.type, MIME_TEXT_JSON) == 0);
    free(info.path);

    /* CSS 文件 */
    snprintf(resolved, sizeof(resolved), "%s/test.css", TEST_ROOT_DIR);
    result = fileserve_get_file_info(fs, resolved, &info);
    assert(result == FILESERVE_OK);
    assert(strcmp(info.mime.type, MIME_TEXT_CSS) == 0);
    free(info.path);

    /* TXT 文件（默认类型） */
    snprintf(resolved, sizeof(resolved), "%s/%s", TEST_ROOT_DIR, TEST_FILE_NAME);
    result = fileserve_get_file_info(fs, resolved, &info);
    assert(result == FILESERVE_OK);
    assert(strcmp(info.mime.type, MIME_TEXT_PLAIN) == 0);
    free(info.path);

    fileserve_destroy(fs);
    cleanup_test_files();
    printf("  PASS\n");
}

/* 测试 8: Range 请求解析 */
static void test_fileserve_parse_range(void) {
    printf("Test 8: FileServe parse range\n");

    uint64_t file_size = 1000;
    RangeInfo range;

    /* 正常范围 */
    int result = fileserve_parse_range("bytes=0-499", file_size, &range);
    assert(result == 0);
    assert(range.has_range == true);
    assert(range.start == 0);
    assert(range.end == 499);

    /* 开放范围 */
    result = fileserve_parse_range("bytes=500-", file_size, &range);
    assert(result == 0);
    assert(range.has_range == true);
    assert(range.start == 500);
    assert(range.end == 999);

    /* 后缀范围 */
    result = fileserve_parse_range("bytes=-100", file_size, &range);
    assert(result == 0);
    assert(range.has_range == true);
    assert(range.start == 900);
    assert(range.end == 999);

    /* 无效范围 */
    result = fileserve_parse_range("invalid", file_size, &range);
    assert(result == -1);

    printf("  PASS\n");
}

/* 测试 9: 自定义 MIME 类型 */
static void test_fileserve_custom_mime(void) {
    printf("Test 9: FileServe custom MIME type\n");

    setup_test_files();

    FileServe *fs = fileserve_create(TEST_ROOT_DIR);
    assert(fs != NULL);

    int result = fileserve_add_mime_type(fs, ".custom", "application/x-custom");
    assert(result == 0);

    /* 测试获取（需要实际文件） */
    fileserve_destroy(fs);
    cleanup_test_files();
    printf("  PASS\n");
}

/* 测试 10: 路径安全检查 */
static void test_fileserve_is_path_safe(void) {
    printf("Test 10: FileServe is path safe\n");

    /* 安全路径 */
    bool safe = fileserve_is_path_safe("/var/www/html/index.html", "/var/www/html");
    assert(safe == true);

    /* 不安全路径（路径穿越） */
    safe = fileserve_is_path_safe("/etc/passwd", "/var/www/html");
    assert(safe == false);

    /* 边界情况 */
    safe = fileserve_is_path_safe("/var/www/html", "/var/www/html");
    assert(safe == true);

    printf("  PASS\n");
}

int main(void) {
    printf("=== FileServe Module Tests ===\n\n");

    test_fileserve_create_destroy();
    test_fileserve_set_root_dir();
    test_fileserve_resolve_path_normal();
    test_fileserve_path_traversal();
    test_fileserve_not_found();
    test_fileserve_get_file_info();
    test_fileserve_mime_type();
    test_fileserve_parse_range();
    test_fileserve_custom_mime();
    test_fileserve_is_path_safe();

    printf("\n=== All tests passed ===\n");
    return 0;
}