/**
 * @file    error.c
 * @brief   错误码和 HTTP 状态码实现
 *
 * @details
 *          - 错误码描述文本
 *          - HTTP 状态码描述文本
 *          - 错误码到状态码映射
 *
 * @layer   Core Layer
 *
 * @depends 无依赖
 * @usedby  http_parser, fileserve, handler, response, server
 *
 * @author  minghui.liu
 * @date    2026-04-21
 */

#include "error.h"

/* 错误码描述 */
static const char *error_descriptions[] = {
    "OK",
    "Socket creation failed",
    "Bind failed",
    "Listen failed",
    "Accept failed",
    "Receive failed",
    "Send failed",
    "SSL initialization failed",
    "SSL handshake failed",
    "Parse failed",
    "Memory allocation failed",
    "Configuration failed",
    "Not found",
    "Forbidden",
    "Internal error",
    "Rate limited",
    "Too many connections",
    "Buffer overflow",
    "Timer not found"
};

/* HTTP 状态码描述 */
static const char *http_status_descriptions[] = {
    [100] = "Continue",
    [200] = "OK",
    [201] = "Created",
    [204] = "No Content",
    [206] = "Partial Content",
    [400] = "Bad Request",
    [401] = "Unauthorized",
    [403] = "Forbidden",
    [404] = "Not Found",
    [405] = "Method Not Allowed",
    [413] = "Payload Too Large",
    [414] = "URI Too Long",
    [500] = "Internal Server Error",
    [501] = "Not Implemented",
    [503] = "Service Unavailable",
    [504] = "Gateway Timeout"
};

const char *error_get_description(ErrorCode code) {
    if (code < 0 || code >= (int)(sizeof(error_descriptions) / sizeof(error_descriptions[0]))) {
        return "Unknown error";
    }
    return error_descriptions[code];
}

HttpStatus error_to_http_status(ErrorCode code) {
    switch (code) {
    case ERR_OK:
        return HTTP_STATUS_OK;
    case ERR_NOT_FOUND:
        return HTTP_STATUS_NOT_FOUND;
    case ERR_FORBIDDEN:
        return HTTP_STATUS_FORBIDDEN;
    case ERR_PARSE_FAILED:
        return HTTP_STATUS_BAD_REQUEST;
    case ERR_RATE_LIMITED:
        return HTTP_STATUS_SERVICE_UNAVAIL;
    case ERR_TOO_MANY_CONNECTIONS:
        return HTTP_STATUS_SERVICE_UNAVAIL;
    case ERR_INTERNAL:
        return HTTP_STATUS_INTERNAL_ERROR;
    default:
        return HTTP_STATUS_INTERNAL_ERROR;
    }
}

const char *http_status_get_description(HttpStatus status) {
    if (status < 100 || status > 504) {
        return "Unknown status";
    }
    return http_status_descriptions[status];
}