/**
 * @file    handler.c
 * @brief   预置 HTTP 请求处理器实现
 *
 * @details
 *          - 静态文件处理器：解析路径、验证安全性、发送文件
 *          - Range 请求支持（206 Partial Content, 416 Range Not Satisfiable）
 *          - JSON API 处理器：直接返回 JSON 响应
 *          - 404/500 处理器：标准错误响应
 *          - 文本处理器：简单文本响应
 *
 * @layer   Handler Layer
 *
 * @depends http_parser, response, fileserve, error
 * @usedby  server, router, examples
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "handler.h"
#include "response.h"
#include "fileserve.h"
#include "error.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ========== 预置 Handler 实现 ========== */

void handler_static_file(HttpRequest *req, HttpResponse *resp, void *user_data) {
    if (!req || !resp || !user_data) {
        if (resp) {
            response_set_status(resp, HTTP_STATUS_INTERNAL_ERROR);
            response_set_body(resp, "Internal Server Error", 21);
        }
        return;
    }

    FileServe *fs = (FileServe *)user_data;
    const char *path = req->path;

    /* 解析路径 */
    char resolved_path[1024];
    FileServeResult result = fileserve_resolve_path(fs, path, resolved_path, sizeof(resolved_path));

    if (result != FILESERVE_OK) {
        switch (result) {
            case FILESERVE_NOT_FOUND:
                response_set_status(resp, HTTP_STATUS_NOT_FOUND);
                response_set_body_html(resp, "<h1>404 Not Found</h1>", 22);
                break;
            case FILESERVE_FORBIDDEN:
                response_set_status(resp, HTTP_STATUS_FORBIDDEN);
                response_set_body_html(resp, "<h1>403 Forbidden</h1>", 22);
                break;
            default:
                response_set_status(resp, HTTP_STATUS_INTERNAL_ERROR);
                response_set_body_html(resp, "<h1>500 Internal Error</h1>", 26);
                break;
        }
        return;
    }

    /* 获取文件信息 */
    FileInfo info;
    result = fileserve_get_file_info(fs, resolved_path, &info);

    if (result != FILESERVE_OK) {
        switch (result) {
            case FILESERVE_NOT_FOUND:
                response_set_status(resp, HTTP_STATUS_NOT_FOUND);
                response_set_body_html(resp, "<h1>404 Not Found</h1>", 22);
                break;
            case FILESERVE_FORBIDDEN:
                response_set_status(resp, HTTP_STATUS_FORBIDDEN);
                response_set_body_html(resp, "<h1>403 Forbidden</h1>", 22);
                break;
            default:
                response_set_status(resp, HTTP_STATUS_INTERNAL_ERROR);
                response_set_body_html(resp, "<h1>500 Internal Error</h1>", 26);
                break;
        }
        if (info.path) free(info.path);
        return;
    }

    /* 设置 MIME 类型 */
    if (info.mime.type) {
        if (info.mime.charset) {
            char content_type[128];
            snprintf(content_type, sizeof(content_type), "%s; charset=%s",
                     info.mime.type, info.mime.charset);
            response_set_header(resp, "Content-Type", content_type);
        } else {
            response_set_header(resp, "Content-Type", info.mime.type);
        }
    } else {
        response_set_header(resp, "Content-Type", "application/octet-stream");
    }

    /* Phase 3.3.2: Range 请求处理 */
    const char *range_header = http_request_get_header_value(req, "Range");
    RangeInfo range_info = {0};

    if (range_header) {
        /* 解析 Range 头 */
        if (fileserve_parse_range(range_header, info.size, &range_info) == 0) {
            /* 有效 Range，返回 206 Partial Content */
            response_set_status(resp, HTTP_STATUS_PARTIAL_CONTENT);

            /* 设置 Content-Range 头 */
            char content_range[128];
            snprintf(content_range, sizeof(content_range),
                     "bytes %llu-%llu/%llu",
                     (unsigned long long)range_info.start,
                     (unsigned long long)range_info.end,
                     (unsigned long long)range_info.total_size);
            response_set_header(resp, "Content-Range", content_range);

            /* 设置 Content-Length 为范围大小 */
            uint64_t range_length = range_info.end - range_info.start + 1;
            char length_str[32];
            snprintf(length_str, sizeof(length_str), "%llu", (unsigned long long)range_length);
            response_set_header(resp, "Content-Length", length_str);

            /* 标记 Range 信息供 Server 使用 */
            char range_start_str[32], range_end_str[32];
            snprintf(range_start_str, sizeof(range_start_str), "%llu", (unsigned long long)range_info.start);
            snprintf(range_end_str, sizeof(range_end_str), "%llu", (unsigned long long)range_info.end);
            response_set_header(resp, "X-Range-Start", range_start_str);
            response_set_header(resp, "X-Range-End", range_end_str);
        } else {
            /* 无效 Range，返回 416 Range Not Satisfiable */
            response_set_status(resp, HTTP_STATUS_RANGE_NOT_SATISFIABLE);
            char content_range[64];
            snprintf(content_range, sizeof(content_range), "*/%llu", (unsigned long long)info.size);
            response_set_header(resp, "Content-Range", content_range);
            response_set_body_html(resp, "<h1>416 Range Not Satisfiable</h1>", 32);
            if (info.path) free(info.path);
            return;
        }
    } else {
        /* 无 Range，正常 200 OK */
        response_set_status(resp, HTTP_STATUS_OK);

        /* 设置 Content-Length */
        char length_str[32];
        snprintf(length_str, sizeof(length_str), "%llu", (unsigned long long)info.size);
        response_set_header(resp, "Content-Length", length_str);
    }

    /* 设置文件路径供 Server 处理 */
    response_set_header(resp, "X-File-Path", resolved_path);
    response_set_body(resp, NULL, 0);  /* 文件内容由 Server 处理 */

    /* 清理 */
    if (info.path) {
        free(info.path);
    }
}

void handler_json_api(HttpRequest *req, HttpResponse *resp, void *user_data) {
    if (!resp) return;

    (void)req;  /* 未使用 */

    const char *json = (const char *)user_data;
    if (!json) {
        json = "{\"error\":\"no data\"}";
    }

    response_set_status(resp, HTTP_STATUS_OK);
    response_set_body_json(resp, json);
}

void handler_404(HttpRequest *req, HttpResponse *resp, void *user_data) {
    if (!resp) return;

    (void)req;
    (void)user_data;

    response_set_status(resp, HTTP_STATUS_NOT_FOUND);
    response_set_body_html(resp, "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>", 57);
}

void handler_500(HttpRequest *req, HttpResponse *resp, void *user_data) {
    if (!resp) return;

    (void)req;
    (void)user_data;

    response_set_status(resp, HTTP_STATUS_INTERNAL_ERROR);
    response_set_body_html(resp, "<!DOCTYPE html><html><body><h1>500 Internal Server Error</h1></body></html>", 67);
}

void handler_text(HttpRequest *req, HttpResponse *resp, void *user_data) {
    if (!resp) return;

    (void)req;

    const char *text = (const char *)user_data;
    if (!text) {
        text = "";
    }

    response_set_status(resp, HTTP_STATUS_OK);
    response_set_header(resp, "Content-Type", "text/plain; charset=utf-8");
    response_set_body(resp, text, strlen(text));
}