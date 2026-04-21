/**
 * @file    handler.h
 * @brief   预置 HTTP 请求处理器，简化请求处理逻辑
 *
 * @details
 *          - 提供标准 RequestHandler 函数签名
 *          - 预置静态文件处理器
 *          - 预置 JSON API 处理器
 *          - 预置 404 处理器
 *
 * @layer   Handler Layer
 *
 * @depends http_parser, response, fileserve, error
 * @usedby  server, router, examples
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#ifndef CHASE_HANDLER_H
#define CHASE_HANDLER_H

#include "http_parser.h"
#include "response.h"
#include "fileserve.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 请求处理器函数签名
 * @param req HTTP 请求对象
 * @param resp HTTP 响应对象
 * @param user_data 用户数据（由路由配置传入）
 */
typedef void (*RequestHandler)(HttpRequest *req, HttpResponse *resp, void *user_data);

/**
 * 静态文件处理器
 * user_data: FileServe* 指针
 *
 * @param req HTTP 请求对象
 * @param resp HTTP 响应对象
 * @param user_data FileServe 指针
 */
void handler_static_file(HttpRequest *req, HttpResponse *resp, void *user_data);

/**
 * JSON API 处理器
 * user_data: const char* JSON 响应字符串
 *
 * @param req HTTP 请求对象
 * @param resp HTTP 响应对象
 * @param user_data JSON 字符串
 */
void handler_json_api(HttpRequest *req, HttpResponse *resp, void *user_data);

/**
 * 404 Not Found 处理器
 * user_data: 未使用
 *
 * @param req HTTP 请求对象
 * @param resp HTTP 响应对象
 * @param user_data 未使用
 */
void handler_404(HttpRequest *req, HttpResponse *resp, void *user_data);

/**
 * 500 Internal Error 处理器
 * user_data: 未使用
 *
 * @param req HTTP 请求对象
 * @param resp HTTP 响应对象
 * @param user_data 未使用
 */
void handler_500(HttpRequest *req, HttpResponse *resp, void *user_data);

/**
 * 简单文本处理器
 * user_data: const char* 文本内容
 *
 * @param req HTTP 请求对象
 * @param resp HTTP 响应对象
 * @param user_data 文本字符串
 */
void handler_text(HttpRequest *req, HttpResponse *resp, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_HANDLER_H */