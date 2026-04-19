#ifndef CHASE_ERROR_H
#define CHASE_ERROR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 错误码 */
typedef enum {
    ERR_OK = 0,
    ERR_SOCKET_FAILED,
    ERR_BIND_FAILED,
    ERR_LISTEN_FAILED,
    ERR_ACCEPT_FAILED,
    ERR_RECV_FAILED,
    ERR_SEND_FAILED,
    ERR_SSL_INIT_FAILED,
    ERR_SSL_HANDSHAKE_FAILED,
    ERR_PARSE_FAILED,
    ERR_MEMORY_FAILED,
    ERR_CONFIG_FAILED,
    ERR_NOT_FOUND,
    ERR_FORBIDDEN,
    ERR_INTERNAL,
    ERR_RATE_LIMITED,
    ERR_TOO_MANY_CONNECTIONS,
    ERR_BUFFER_OVERFLOW,
    ERR_TIMER_NOT_FOUND
} ErrorCode;

/* HTTP 状态码 */
typedef enum {
    HTTP_STATUS_CONTINUE           = 100,
    HTTP_STATUS_OK                 = 200,
    HTTP_STATUS_CREATED            = 201,
    HTTP_STATUS_NO_CONTENT         = 204,
    HTTP_STATUS_PARTIAL_CONTENT    = 206,
    HTTP_STATUS_BAD_REQUEST        = 400,
    HTTP_STATUS_UNAUTHORIZED       = 401,
    HTTP_STATUS_FORBIDDEN          = 403,
    HTTP_STATUS_NOT_FOUND          = 404,
    HTTP_STATUS_METHOD_NOT_ALLOWED = 405,
    HTTP_STATUS_PAYLOAD_TOO_LARGE  = 413,
    HTTP_STATUS_URI_TOO_LONG       = 414,
    HTTP_STATUS_INTERNAL_ERROR     = 500,
    HTTP_STATUS_NOT_IMPLEMENTED    = 501,
    HTTP_STATUS_SERVICE_UNAVAIL    = 503,
    HTTP_STATUS_GATEWAY_TIMEOUT    = 504
} HttpStatus;

/**
 * 获取错误码描述
 * @param code 错误码
 * @return 描述字符串
 */
const char *error_get_description(ErrorCode code);

/**
 * 错误码转换为 HTTP 状态码
 * @param code 错误码
 * @return HTTP 状态码
 */
HttpStatus error_to_http_status(ErrorCode code);

/**
 * 获取 HTTP 状态码描述
 * @param status HTTP 状态码
 * @return 描述字符串
 */
const char *http_status_get_description(HttpStatus status);

#ifdef __cplusplus
}
#endif

#endif /* CHASE_ERROR_H */