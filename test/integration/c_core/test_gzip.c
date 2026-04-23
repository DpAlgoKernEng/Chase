/**
 * @file    test_gzip.c
 * @brief   HTTP Parser gzip/deflate 解压测试
 *
 * @details
 *          - gzip 解压测试
 *          - deflate 解压测试
 *          - Zip Bomb 检测测试
 *          - 大小限制测试
 *          - 错误处理测试
 *
 * @layer   Test Layer
 *
 * @depends http_parser, zlib
 *
 * @author  minghui.liu
 * @date    2026-04-23
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <zlib.h>

#include "http_parser.h"

static int test_count = 0;
static int test_passed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("Test %d: %s\n", ++test_count, #name); \
    fflush(stdout); \
    test_##name(); \
    test_passed++; \
} while(0)

/* 辅助函数：gzip 压缩数据 */
static size_t gzip_compress(const char *input, size_t input_len,
                            char **output, size_t *output_capacity) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    /* 初始化 gzip 压缩 */
    int ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                           15 + 16, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) return 0;

    /* 分配输出缓冲区 */
    size_t buf_size = input_len + 256;
    *output = malloc(buf_size);
    if (!*output) {
        deflateEnd(&strm);
        return 0;
    }
    *output_capacity = buf_size;

    strm.next_in = (Bytef *)input;
    strm.avail_in = input_len;

    strm.next_out = (Bytef *)*output;
    strm.avail_out = buf_size;

    ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&strm);
        free(*output);
        *output = NULL;
        return 0;
    }

    size_t compressed_len = strm.total_out;
    deflateEnd(&strm);

    return compressed_len;
}

/* 辅助函数：deflate 压缩数据（raw deflate） */
static size_t deflate_compress(const char *input, size_t input_len,
                               char **output, size_t *output_capacity) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    /* 初始化 raw deflate 压缩 */
    int ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                           -15, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) return 0;

    size_t buf_size = input_len + 256;
    *output = malloc(buf_size);
    if (!*output) {
        deflateEnd(&strm);
        return 0;
    }
    *output_capacity = buf_size;

    strm.next_in = (Bytef *)input;
    strm.avail_in = input_len;

    strm.next_out = (Bytef *)*output;
    strm.avail_out = buf_size;

    ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&strm);
        free(*output);
        *output = NULL;
        return 0;
    }

    size_t compressed_len = strm.total_out;
    deflateEnd(&strm);

    return compressed_len;
}

/* ============== 测试用例 ============== */

TEST(gzip_simple) {
    HttpParser *parser = http_parser_create();
    assert(parser != NULL);

    HttpRequest *req = http_request_create();
    assert(req != NULL);

    /* 原始数据 */
    const char *original = "Hello, World! This is a test message for gzip compression.";
    size_t original_len = strlen(original);

    /* 压缩数据 */
    char *compressed = NULL;
    size_t compressed_cap = 0;
    size_t compressed_len = gzip_compress(original, original_len, &compressed, &compressed_cap);
    assert(compressed_len > 0);
    assert(compressed != NULL);

    /* 设置请求 body */
    req->body = compressed;
    req->body_length = compressed_len;

    /* 手动添加头部 */
    req->headers[0].name = strdup("Content-Encoding");
    req->headers[0].value = strdup("gzip");
    req->header_count = 1;

    /* 解压 */
    DecompressResult result = http_request_decompress_body(req, parser);
    assert(result == DECOMPRESS_OK);
    assert(req->decompress_result == DECOMPRESS_OK);
    assert(req->body_length == original_len);
    assert(strcmp(req->body, original) == 0);
    assert(req->original_body_size == compressed_len);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

TEST(deflate_simple) {
    HttpParser *parser = http_parser_create();
    assert(parser != NULL);

    HttpRequest *req = http_request_create();
    assert(req != NULL);

    /* 原始数据 */
    const char *original = "Hello deflate! Testing raw deflate compression.";
    size_t original_len = strlen(original);

    /* 压缩数据 */
    char *compressed = NULL;
    size_t compressed_cap = 0;
    size_t compressed_len = deflate_compress(original, original_len, &compressed, &compressed_cap);
    assert(compressed_len > 0);
    assert(compressed != NULL);

    /* 设置请求 body */
    req->body = compressed;
    req->body_length = compressed_len;

    /* 添加 Content-Encoding 头部 */
    req->headers[0].name = strdup("Content-Encoding");
    req->headers[0].value = strdup("deflate");
    req->header_count = 1;

    /* 解压 */
    DecompressResult result = http_request_decompress_body(req, parser);
    assert(result == DECOMPRESS_OK);
    assert(req->body_length == original_len);
    assert(strcmp(req->body, original) == 0);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

TEST(no_compression) {
    HttpParser *parser = http_parser_create();
    assert(parser != NULL);

    HttpRequest *req = http_request_create();
    assert(req != NULL);

    /* 无 Content-Encoding 头部 */
    const char *data = "Plain text without compression";
    req->body = strdup(data);
    req->body_length = strlen(data);

    /* 解压应返回 NOT_NEEDED */
    DecompressResult result = http_request_decompress_body(req, parser);
    assert(result == DECOMPRESS_NOT_NEEDED);
    assert(strcmp(req->body, data) == 0);  /* body 未被修改 */

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

TEST(zip_bomb_detection) {
    HttpParser *parser = http_parser_create();
    assert(parser != NULL);

    /* 设置严格的压缩比限制 */
    DecompressConfig config = {
        .max_decompressed_size = DECOMPRESS_DEFAULT_MAX_SIZE,
        .max_ratio = 10.0,  /* 10:1 限制 */
        .enable_gzip = true,
        .enable_deflate = true
    };
    http_parser_set_decompress_config(parser, &config);

    HttpRequest *req = http_request_create();
    assert(req != NULL);

    /* 创建一个高压缩比数据（模拟 zip bomb） */
    /* 重复相同内容以达到高压缩比 */
    size_t original_len = 100000;  /* 100KB */
    char *original = malloc(original_len);
    for (size_t i = 0; i < original_len; i++) {
        original[i] = 'A';  /* 全是相同字符，压缩比极高 */
    }

    char *compressed = NULL;
    size_t compressed_cap = 0;
    size_t compressed_len = gzip_compress(original, original_len, &compressed, &compressed_cap);
    assert(compressed_len > 0);

    /* 压缩比应该超过 10:1 */
    double ratio = (double)original_len / (double)compressed_len;
    assert(ratio > 10.0);

    /* 设置请求 */
    req->body = compressed;
    req->body_length = compressed_len;
    req->headers[0].name = strdup("Content-Encoding");
    req->headers[0].value = strdup("gzip");
    req->header_count = 1;

    /* 解压应检测到 zip bomb */
    DecompressResult result = http_request_decompress_body(req, parser);
    assert(result == DECOMPRESS_ZIP_BOMB);
    assert(req->decompress_result == DECOMPRESS_ZIP_BOMB);

    free(original);
    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

TEST(size_limit) {
    HttpParser *parser = http_parser_create();
    assert(parser != NULL);

    /* 设置小的解压大小限制 */
    DecompressConfig config = {
        .max_decompressed_size = 1024,  /* 1KB 限制 */
        .max_ratio = 100.0,
        .enable_gzip = true,
        .enable_deflate = true
    };
    http_parser_set_decompress_config(parser, &config);

    HttpRequest *req = http_request_create();
    assert(req != NULL);

    /* 创建超过限制的数据 */
    size_t original_len = 5000;  /* 5KB */
    char *original = malloc(original_len);
    for (size_t i = 0; i < original_len; i++) {
        original[i] = 'A' + (i % 26);
    }

    char *compressed = NULL;
    size_t compressed_cap = 0;
    size_t compressed_len = gzip_compress(original, original_len, &compressed, &compressed_cap);
    assert(compressed_len > 0);

    req->body = compressed;
    req->body_length = compressed_len;
    req->headers[0].name = strdup("Content-Encoding");
    req->headers[0].value = strdup("gzip");
    req->header_count = 1;

    /* 解压应超过大小限制 */
    DecompressResult result = http_request_decompress_body(req, parser);
    assert(result == DECOMPRESS_SIZE_EXCEEDED);

    free(original);
    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

TEST(invalid_gzip) {
    HttpParser *parser = http_parser_create();
    assert(parser != NULL);

    HttpRequest *req = http_request_create();
    assert(req != NULL);

    /* 无效的 gzip 数据 */
    char invalid_data[] = "This is not valid gzip data!";
    req->body = strdup(invalid_data);
    req->body_length = strlen(invalid_data);
    req->headers[0].name = strdup("Content-Encoding");
    req->headers[0].value = strdup("gzip");
    req->header_count = 1;

    /* 解压应失败 */
    DecompressResult result = http_request_decompress_body(req, parser);
    assert(result == DECOMPRESS_ERROR);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

TEST(partial_gzip) {
    HttpParser *parser = http_parser_create();
    assert(parser != NULL);

    HttpRequest *req = http_request_create();
    assert(req != NULL);

    /* 原始数据 */
    const char *original = "Complete message for compression";
    size_t original_len = strlen(original);

    /* 压缩数据 */
    char *compressed = NULL;
    size_t compressed_cap = 0;
    size_t compressed_len = gzip_compress(original, original_len, &compressed, &compressed_cap);
    assert(compressed_len > 0);

    /* 截断数据（不完整） */
    size_t truncated_len = compressed_len / 2;

    req->body = malloc(truncated_len);
    memcpy(req->body, compressed, truncated_len);
    req->body_length = truncated_len;
    req->headers[0].name = strdup("Content-Encoding");
    req->headers[0].value = strdup("gzip");
    req->header_count = 1;

    /* 解压应失败 */
    DecompressResult result = http_request_decompress_body(req, parser);
    assert(result == DECOMPRESS_ERROR);

    free(compressed);
    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

TEST(content_encoding_parse) {
    HttpRequest *req = http_request_create();
    assert(req != NULL);

    /* 无头部时应返回 NULL */
    const char *encoding = http_request_get_content_encoding(req);
    assert(encoding == NULL);

    /* 添加头部 */
    req->headers[0].name = strdup("Content-Encoding");
    req->headers[0].value = strdup("gzip, deflate");
    req->header_count = 1;

    encoding = http_request_get_content_encoding(req);
    assert(encoding != NULL);
    assert(strstr(encoding, "gzip") != NULL);
    assert(strstr(encoding, "deflate") != NULL);

    http_request_destroy(req);
    printf("  PASS\n");
}

TEST(needs_decompression) {
    HttpRequest *req = http_request_create();
    assert(req != NULL);

    /* 无头部 */
    assert(http_request_needs_decompression(req) == false);

    /* gzip 头部 */
    req->headers[0].name = strdup("Content-Encoding");
    req->headers[0].value = strdup("gzip");
    req->header_count = 1;

    assert(http_request_needs_decompression(req) == true);

    http_request_destroy(req);

    /* deflate 头部 */
    req = http_request_create();
    req->headers[0].name = strdup("Content-Encoding");
    req->headers[0].value = strdup("deflate");
    req->header_count = 1;

    assert(http_request_needs_decompression(req) == true);

    http_request_destroy(req);

    /* 其他编码 */
    req = http_request_create();
    req->headers[0].name = strdup("Content-Encoding");
    req->headers[0].value = strdup("br");  /* Brotli，不支持 */
    req->header_count = 1;

    assert(http_request_needs_decompression(req) == false);

    http_request_destroy(req);
    printf("  PASS\n");
}

TEST(ratio_calculation) {
    /* 测试 zip bomb 检测函数 */
    assert(http_detect_zip_bomb(100, 500, 10.0) == false);   /* 5:1 < 10:1 */
    assert(http_detect_zip_bomb(100, 2000, 10.0) == true);   /* 20:1 > 10:1 */
    assert(http_detect_zip_bomb(100, 1000, 10.0) == false);  /* 10:1 = 10:1 */

    /* 边界情况 */
    assert(http_detect_zip_bomb(0, 1, 100.0) == true);       /* 原始为 0 */
    assert(http_detect_zip_bomb(0, 0, 100.0) == false);      /* 都为 0 */
    assert(http_detect_zip_bomb(100, 0, 10.0) == false);     /* 解压为 0 */

    printf("  PASS\n");
}

TEST(config_set) {
    HttpParser *parser = http_parser_create();
    assert(parser != NULL);

    /* 设置自定义配置 */
    DecompressConfig config = {
        .max_decompressed_size = 5 * 1024 * 1024,  /* 5MB */
        .max_ratio = 50.0,
        .enable_gzip = true,
        .enable_deflate = false
    };

    int ret = http_parser_set_decompress_config(parser, &config);
    assert(ret == 0);

    /* 验证配置已设置 */
    /* 通过解压 deflate 来验证配置生效 */
    HttpRequest *req = http_request_create();
    const char *original = "Test deflate disabled";
    size_t original_len = strlen(original);

    char *compressed = NULL;
    size_t compressed_cap = 0;
    size_t compressed_len = deflate_compress(original, original_len, &compressed, &compressed_cap);
    assert(compressed_len > 0);

    req->body = compressed;
    req->body_length = compressed_len;
    req->headers[0].name = strdup("Content-Encoding");
    req->headers[0].value = strdup("deflate");
    req->header_count = 1;

    /* deflate 已禁用，应返回 NOT_NEEDED */
    DecompressResult result = http_request_decompress_body(req, parser);
    assert(result == DECOMPRESS_NOT_NEEDED);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

TEST(empty_body) {
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();

    /* 空 body */
    req->body = strdup("");
    req->body_length = 0;
    req->headers[0].name = strdup("Content-Encoding");
    req->headers[0].value = strdup("gzip");
    req->header_count = 1;

    /* 空 body 应返回 NOT_NEEDED */
    DecompressResult result = http_request_decompress_body(req, parser);
    assert(result == DECOMPRESS_NOT_NEEDED);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

TEST(null_handling) {
    /* NULL 参数测试 */
    assert(http_request_decompress_body(NULL, NULL) == DECOMPRESS_ERROR);
    assert(http_request_decompress_body(NULL, http_parser_create()) == DECOMPRESS_ERROR);

    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    assert(http_request_decompress_body(req, NULL) == DECOMPRESS_ERROR);

    assert(http_request_get_content_encoding(NULL) == NULL);
    assert(http_request_needs_decompression(NULL) == false);
    assert(http_parser_set_decompress_config(NULL, NULL) == -1);

    DecompressConfig config = { .max_decompressed_size = 1000, .max_ratio = 10.0 };
    assert(http_parser_set_decompress_config(NULL, &config) == -1);
    assert(http_parser_set_decompress_config(parser, NULL) == -1);

    http_request_destroy(req);
    http_parser_destroy(parser);
    printf("  PASS\n");
}

/* ============== 主函数 ============== */

int main(void) {
    printf("=== HTTP Parser Gzip/Deflate Tests ===\n\n");

    RUN_TEST(gzip_simple);
    RUN_TEST(deflate_simple);
    RUN_TEST(no_compression);
    RUN_TEST(zip_bomb_detection);
    RUN_TEST(size_limit);
    RUN_TEST(invalid_gzip);
    RUN_TEST(partial_gzip);
    RUN_TEST(content_encoding_parse);
    RUN_TEST(needs_decompression);
    RUN_TEST(ratio_calculation);
    RUN_TEST(config_set);
    RUN_TEST(empty_body);
    RUN_TEST(null_handling);

    printf("\n=== Test Summary ===\n");
    printf("Total: %d, Passed: %d, Failed: %d\n",
           test_count, test_passed, test_count - test_passed);

    return (test_count == test_passed) ? 0 : 1;
}