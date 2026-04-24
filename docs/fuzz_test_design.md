# Chase HTTP Parser 和 SSL 模块 Fuzz 测试设计文档

## 目录

1. [概述](#概述)
2. [HTTP Parser Fuzz 测试](#http-parser-fuzz-测试)
3. [SSL 模块 Fuzz 测试](#ssl-模块-fuzz-测试)
4. [测试框架选择](#测试框架选择)
5. [测试目标与验收标准](#测试目标与验收标准)
6. [CMake 集成](#cmake-集成)
7. [CI 集成建议](#ci-集成建议)
8. [发现的漏洞分类与修复建议](#发现的漏洞分类与修复建议)

---

## 概述

Fuzz 测试是一种自动化安全测试技术，通过向目标程序注入大量随机或半结构化的畸形数据，发现潜在的内存安全漏洞、边界条件错误和异常处理缺陷。

本设计文档针对 Chase HTTP 服务器的两个核心模块：

- **HTTP Parser** (`src/http_parser.c`): HTTP/1.1 请求解析器，增量状态机实现
- **SSL Wrapper** (`src/ssl_wrap.c`): SSL/TLS 包装模块，OpenSSL 1.1.1 和 3.x 兼容

### 测试重要性

| 模块 | 安全风险 | 典型漏洞类型 |
|------|---------|-------------|
| HTTP Parser | 高危 | 缓冲区溢出、整数溢出、解析逻辑错误、DoS |
| SSL Wrapper | 高危 | 握手失败、证书验证绕过、协议降级、Session Ticket 操控 |

---

## HTTP Parser Fuzz 测试

### 1.1 Malformed HTTP Requests 测试场景

#### 1.1.1 无效方法测试

```c
// Fuzz 输入示例
const char *malformed_methods[] = {
    "G\x00T / HTTP/1.1\r\n\r\n",           // NULL 字符注入
    "GET\x01\x02\x03 / HTTP/1.1\r\n\r\n",  // 控制字符
    "AAAAAAAA...AAAA / HTTP/1.1\r\n\r\n",  // 超长方法名 (> MAX_METHOD_SIZE=32)
    " / HTTP/1.1\r\n\r\n",                  // 空方法
    "GET\x7f / HTTP/1.1\r\n\r\n",          // DEL 字符
    "get / HTTP/1.1\r\n\r\n",               // 小写方法（应被识别）
};
```

**预期行为**:
- 超长方法应返回 `PARSE_ERROR`（超过 `MAX_METHOD_SIZE=32`）
- 控制字符应被正确处理或拒绝
- NULL 字符不应导致缓冲区溢出

**测试代码框架**:
```c
int fuzz_http_method(const uint8_t *data, size_t size) {
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    
    size_t consumed = 0;
    ParseResult result = http_parser_parse(parser, req, 
                                           (const char *)data, size, &consumed);
    
    // 无论解析结果如何，不应崩溃
    http_request_destroy(req);
    http_parser_destroy(parser);
    return 0;  // 0 表示无崩溃
}
```

#### 1.1.2 无效请求行测试

```c
const char *malformed_request_lines[] = {
    "GET HTTP/1.1\r\n\r\n",                   // 缺少路径
    "GET / \r\n\r\n",                         // 缺少版本
    "GET / HTTP/1.1\x00\r\n\r\n",            // 版本中 NULL 字符
    "GET /\r\r\n\r\n",                        // 多个 CR
    "GET / HTTP/1.1\n\r\n",                   // LF 在 CR 之前
    "GET  /  HTTP/1.1\r\n\r\n",               // 多空格
    "GET\t/\tHTTP/1.1\r\n\r\n",               // Tab 字符
    "GET /%%20path HTTP/1.1\r\n\r\n",        // URL 编码异常
};
```

#### 1.1.3 无效头部测试

```c
const char *malformed_headers[] = {
    "GET / HTTP/1.1\r\n: value\r\n\r\n",       // 空头部名
    "GET / HTTP/1.1\r\nName:\r\n\r\n",         // 空头部值
    "GET / HTTP/1.1\r\nName=value\r\n\r\n",    // 缺少冒号
    "GET / HTTP/1.1\r\n\x00Name: value\r\n",  // NULL 在头部名
    "GET / HTTP/1.1\r\nName: \x00value\r\n",  // NULL 在头部值
    "GET / HTTP/1.1\r\nName: \r\r\n",         // CR 在头部值中间
    "GET / HTTP/1.1\r\nNa\x00me: value\r\n",  // NULL 分割头部名
};
```

### 1.2 超大 Header/Body 溢出测试

#### 1.2.1 超大 Header 测试

```c
void fuzz_large_header() {
    // 生成超过 MAX_HEADER_SIZE (64KB) 的头部
    char request[128 * 1024];
    int pos = snprintf(request, sizeof(request), 
                       "GET / HTTP/1.1\r\nX-Large: ");
    
    // 填充超长头部值
    for (int i = 0; i < 100 * 1024; i++) {
        request[pos++] = 'A';
    }
    pos += snprintf(request + pos, sizeof(request) - pos, "\r\n\r\n");
    
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    size_t consumed = 0;
    
    ParseResult result = http_parser_parse(parser, req, request, pos, &consumed);
    
    // 应返回 PARSE_ERROR，不应崩溃或内存溢出
    assert(result == PARSE_ERROR || result == PARSE_COMPLETE);
    
    http_request_destroy(req);
    http_parser_destroy(parser);
}
```

**关键边界值**:
- `MAX_HEADER_SIZE = 64 * 1024` (64KB)
- `MAX_PATH_SIZE = 8192` (8KB)
- `MAX_QUERY_SIZE = 8192` (8KB)

#### 1.2.2 Content-Length 整数溢出测试

```c
const char *overflow_content_length[] = {
    // 正常范围
    "POST / HTTP/1.1\r\nContent-Length: 100\r\n\r\n",
    
    // 边界值
    "POST / HTTP/1.1\r\nContent-Length: 104857600\r\n\r\n",  // 100MB (上限)
    
    // 超出上限
    "POST / HTTP/1.1\r\nContent-Length: 104857601\r\n\r\n",  // 100MB + 1
    
    // 整数溢出
    "POST / HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n",
    "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
    "POST / HTTP/1.1\r\nContent-Length: 18446744073709551615\r\n\r\n",  // ULLONG_MAX
    "POST / HTTP/1.1\r\nContent-Length: 0xFFFFFFFFFFFFFFFF\r\n\r\n",
    
    // 格式错误
    "POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
    "POST / HTTP/1.1\r\nContent-Length: 123abc\r\n\r\n",
    "POST / HTTP/1.1\r\nContent-Length: 12 34\r\n\r\n",
};
```

**预期行为**:
- `safe_parse_content_length()` 应拒绝超过 20 位的数字
- 应拒绝超过 100MB 的值（项目上限）
- 应拒绝非数字字符
- 应拒绝负值

#### 1.2.3 超大 Body 测试

```c
void fuzz_large_body() {
    // 测试 body 分配边界
    size_t body_sizes[] = {
        1024,       // 1KB
        64 * 1024,  // 64KB
        1024 * 1024, // 1MB
        10 * 1024 * 1024, // 10MB (默认上限)
        100 * 1024 * 1024, // 100MB (Content-Length 上限)
    };
    
    for (size_t i = 0; i < sizeof(body_sizes)/sizeof(body_sizes[0]); i++) {
        char *request = malloc(body_sizes[i] + 1024);
        if (!request) continue;
        
        int pos = snprintf(request, 1024, 
                           "POST / HTTP/1.1\r\nContent-Length: %zu\r\n\r\n",
                           body_sizes[i]);
        
        // 填充 body
        memset(request + pos, 'X', body_sizes[i]);
        
        HttpParser *parser = http_parser_create();
        HttpRequest *req = http_request_create();
        size_t consumed = 0;
        
        http_parser_parse(parser, req, request, pos + body_sizes[i], &consumed);
        
        // 验证无内存泄漏和崩溃
        http_request_destroy(req);
        http_parser_destroy(parser);
        free(request);
    }
}
```

### 1.3 特殊字符注入测试

#### 1.3.1 URL 注入测试

```c
const char *url_injection_cases[] = {
    // 路径遍历
    "GET /../../../etc/passwd HTTP/1.1\r\n\r\n",
    "GET /..%2f..%2f..%2fetc/passwd HTTP/1.1\r\n\r\n",
    "GET /..\\..\\..\\windows\\system32 HTTP/1.1\r\n\r\n",
    
    // Unicode 注入
    "GET /\u0000/test HTTP/1.1\r\n\r\n",
    "GET /test%C0%AE HTTP/1.1\r\n\r\n",       // UTF-8 overlong encoding
    
    // 控制字符
    "GET /\x01\x02\x03 HTTP/1.1\r\n\r\n",
    "GET /test\x0a HTTP/1.1\r\n\r\n",         // LF 在路径中
    "GET /test\x0d HTTP/1.1\r\n\r\n",         // CR 在路径中
    
    // 空字节注入
    "GET /test.php\x00.jpg HTTP/1.1\r\n\r\n", // 文件扩展名绕过
};
```

#### 1.3.2 Header 注入测试（CRLF 注入）

```c
const char *header_injection_cases[] = {
    // CRLF 注入 - 添加伪造头部
    "GET / HTTP/1.1\r\nHost: localhost\r\nX-Injected: value\r\r\nSet-Cookie: hacked=true\r\n\r\n",
    
    // 响应分割攻击
    "GET / HTTP/1.1\r\nHost: localhost%0d%0aSet-Cookie: session=hacked\r\n\r\n",
    
    // 头部值中的换行
    "GET / HTTP/1.1\r\nUser-Agent: test\nMalicious-Header: bad\r\n\r\n",
};
```

### 1.4 Chunked 编码边界测试

#### 1.4.1 Chunk 大小解析测试

```c
const char *chunked_size_cases[] = {
    // 正常 chunk
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nHello\r\n0\r\n\r\n",
    
    // 十六进制大小
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nFF\r\n...\r\n0\r\n\r\n",
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nffff\r\n...\r\n0\r\n\r\n",
    
    // 无效 chunk 大小
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n...\r\n0\r\n\r\n",  // 非十六进制
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n-1\r\n...\r\n0\r\n\r\n", // 负值
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n\r\n...",              // 空 chunk 大小
    
    // 超大 chunk 大小（整数溢出）
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFF\r\n...\r\n0\r\n\r\n",
    
    // chunk 扩展参数
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5;name=value;ext\r\nHello\r\n0\r\n\r\n",
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5;\x00bad\r\nHello\r\n0\r\n\r\n", // NULL 在扩展中
};
```

#### 1.4.2 Chunk 数据不匹配测试

```c
const char *chunk_data_mismatch[] = {
    // chunk 大小与数据不匹配
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n10\r\nHello\r\n0\r\n\r\n",  // 声明 16字节，实际 5
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nHello World\r\n0\r\n\r\n", // 声明 3字节，实际 11
    
    // 缺少 chunk 结束标记
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nHello0\r\n\r\n",     // 缺少 \r\n
    
    // 多个 chunk 重叠
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nHello\r\n5\r\nHello\r\n0\r\n\r\n",
};
```

#### 1.4.3 Chunk Trailer 测试

```c
const char *chunk_trailer_cases[] = {
    // 正常 trailer
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nHello\r\n0\r\nX-Trailer: value\r\n\r\n",
    
    // 无效 trailer
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nHello\r\n0\r\n\x00Trailer: bad\r\n\r\n",
    
    // 超长 trailer
    "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nHello\r\n0\r\nX-Large: AAAA...(64KB)\r\n\r\n",
};
```

#### 1.4.4 Chunked 状态机 Fuzz

```c
int fuzz_chunked_state_machine(const uint8_t *data, size_t size) {
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    
    // 构造 chunked 请求头
    char header[] = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    
    // 组合 fuzz 数据作为 chunk body
    char *request = malloc(strlen(header) + size + 64);
    memcpy(request, header, strlen(header));
    
    // 添加 chunk 大小和数据
    int chunk_size_len = snprintf(request + strlen(header), 32, "%zu\r\n", size);
    memcpy(request + strlen(header) + chunk_size_len, data, size);
    memcpy(request + strlen(header) + chunk_size_len + size, "\r\n0\r\n\r\n", 7);
    
    size_t total = strlen(header) + chunk_size_len + size + 7;
    size_t consumed = 0;
    
    ParseResult result = http_parser_parse(parser, req, request, total, &consumed);
    
    free(request);
    http_request_destroy(req);
    http_parser_destroy(parser);
    return 0;
}
```

### 1.5 Gzip 解压边界测试（Zip Bomb）

#### 1.5.1 Zip Bomb 检测测试

```c
void fuzz_zip_bomb() {
    // 创建高压缩比数据
    // 原始数据: 100KB 重复字符
    size_t original_size = 100 * 1024;
    char *original = malloc(original_size);
    memset(original, 'A', original_size);
    
    // gzip 压缩
    z_stream strm;
    deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    
    size_t compressed_cap = original_size / 100;  // ~1KB
    char *compressed = malloc(compressed_cap);
    
    strm.next_in = (Bytef *)original;
    strm.avail_in = original_size;
    strm.next_out = (Bytef *)compressed;
    strm.avail_out = compressed_cap;
    
    deflate(&strm, Z_FINISH);
    size_t compressed_size = strm.total_out;
    deflateEnd(&strm);
    
    // 压缩比约 100:1，应触发 Zip Bomb 检测
    HttpParser *parser = http_parser_create();
    
    DecompressConfig config = {
        .max_decompressed_size = DECOMPRESS_DEFAULT_MAX_SIZE,  // 10MB
        .max_ratio = 100.0,  // 默认 100:1
        .enable_gzip = true,
        .enable_deflate = true
    };
    http_parser_set_decompress_config(parser, &config);
    
    HttpRequest *req = http_request_create();
    req->body = compressed;
    req->body_length = compressed_size;
    req->headers[0].name = strdup("Content-Encoding");
    req->headers[0].value = strdup("gzip");
    req->header_count = 1;
    
    DecompressResult result = http_request_decompress_body(req, parser);
    
    // 验证 Zip Bomb 检测
    assert(result == DECOMPRESS_ZIP_BOMB || result == DECOMPRESS_OK);
    
    free(original);
    http_request_destroy(req);
    http_parser_destroy(parser);
}
```

#### 1.5.2 解压大小限制测试

```c
const size_t decompress_size_limits[] = {
    1024,           // 1KB - 极小限制
    64 * 1024,      // 64KB
    1024 * 1024,    // 1MB
    10 * 1024 * 1024, // 10MB (默认)
};
```

#### 1.5.3 无效压缩数据测试

```c
const char *invalid_compression_data[] = {
    // 无效 gzip magic
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",  // 错误头部
    
    // 截断的 gzip 数据
    "\x1f\x8b\x08\x00\x00\x00\x00\x00",          // 只有头部
    
    // zlib 格式（非 gzip）
    "\x78\x9c\x00\x00\x00\x00\x00",              // zlib header
    
    // 随机数据
    "This is not gzip data at all!",
};
```

---

## SSL 模块 Fuzz 测试

### 2.1 SSL 握手异常测试

#### 2.1.1 状态机 Fuzz

```c
int fuzz_ssl_handshake_state(const uint8_t *data, size_t size) {
    // 创建临时 SSL 配置
    SslConfig config = {
        .cert_file = "test/certs/test.crt",
        .key_file = "test/certs/test.key",
        .verify_peer = false,
        .session_timeout = 300,
        .enable_tickets = true
    };
    
    SslServerCtx *ctx = ssl_server_ctx_create(&config);
    if (!ctx) return 0;
    
    // 创建 socket pair 进行测试
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    
    SslConnection *conn = ssl_connection_create(ctx, fds[0]);
    if (!conn) {
        close(fds[0]);
        close(fds[1]);
        ssl_server_ctx_destroy(ctx);
        return 0;
    }
    
    // 向 socket 写入 fuzz 数据（模拟客户端发送）
    write(fds[1], data, size);
    close(fds[1]);
    
    // 尝试握手
    SslState state = ssl_handshake(conn);
    
    // 无论结果，不应崩溃
    ssl_connection_destroy(conn);
    close(fds[0]);
    ssl_server_ctx_destroy(ctx);
    
    return 0;  // 无崩溃
}
```

#### 2.1.2 无效 TLS 记录测试

```c
const uint8_t invalid_tls_records[][32] = {
    // 无效记录类型
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},                   // 全零
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},                   // 全 FF
    
    // 无效 TLS 版本
    {0x16, 0x00, 0x00, 0x00, 0x05, ...},                    // 版本 0.0
    {0x16, 0x99, 0x99, 0x00, 0x05, ...},                    // 版本 9999
    
    // 超大长度字段
    {0x16, 0x03, 0x01, 0xFF, 0xFF, ...},                    // 65535 字节声明
    
    // 截断的 ClientHello
    {0x16, 0x03, 0x01, 0x00, 0x05, 0x01, 0x00, 0x00, 0x00}, // 不完整
    
    // 零长度记录
    {0x16, 0x03, 0x01, 0x00, 0x00},
};
```

#### 2.1.3 ClientHello 操控测试

```c
typedef struct {
    uint8_t record_type;      // 0x16 = Handshake
    uint8_t version_major;    // 0x03
    uint8_t version_minor;    // 0x01 (TLS 1.0), 0x02 (TLS 1.1), 0x03 (TLS 1.2)
    uint16_t length;
    uint8_t handshake_type;   // 0x01 = ClientHello
    uint24_t handshake_length;
    uint16_t client_version;
    uint8_t random[32];
    uint8_t session_id_length;
    uint8_t session_id[32];
    uint16_t cipher_suites_length;
    uint8_t cipher_suites[];
    uint8_t compression_methods_length;
    uint8_t compression_methods[];
    // extensions...
} ClientHello;

const ClientHello fuzz_cases[] = {
    // TLS 版本降级攻击
    {.client_version = {0x03, 0x01}},  // 强制 TLS 1.0（应被拒绝）
    {.client_version = {0x03, 0x00}},  // SSL 3.0（应被拒绝）
    
    // 空密码套件列表
    {.cipher_suites_length = 0},
    
    // 无效密码套件
    {.cipher_suites = {0x00, 0x00}},   // TLS_NULL_WITH_NULL_NULL
    
    // 超长 session_id
    {.session_id_length = 255},        // 超过 32 字节限制
    
    // 随机数据
    {.random = {0}},                   // 全零 random
    {.random = {0xFF, ...}},           // 全 FF random
};
```

### 2.2 证书解析异常测试

#### 2.2.1 无效证书文件测试

```c
void fuzz_certificate_loading() {
    // 无效证书路径
    const char *invalid_paths[] = {
        "/dev/null",            // 空文件
        "/dev/urandom",         // 随机数据
        "/tmp/nonexistent.crt", // 不存在
        "",                     // 空路径
        NULL,                   // NULL 路径
    };
    
    for (int i = 0; i < sizeof(invalid_paths)/sizeof(invalid_paths[0]); i++) {
        SslConfig config = {
            .cert_file = invalid_paths[i],
            .key_file = "test/certs/test.key",
            .verify_peer = false
        };
        
        SslServerCtx *ctx = ssl_server_ctx_create(&config);
        
        // 应返回 NULL，不应崩溃
        assert(ctx == NULL);
    }
}
```

#### 2.2.2 证书私钥不匹配测试

```c
void fuzz_cert_key_mismatch() {
    SslConfig config = {
        .cert_file = "test/certs/test.crt",    // 证书 A
        .key_file = "test/certs/other.key",    // 私钥 B（不匹配）
        .verify_peer = false
    };
    
    SslServerCtx *ctx = ssl_server_ctx_create(&config);
    
    // 应返回 NULL（SSL_CTX_check_private_key 失败）
    assert(ctx == NULL);
}
```

#### 2.2.3 证书内容 Fuzz

```c
int fuzz_certificate_data(const uint8_t *data, size_t size) {
    // 写入临时证书文件
    char temp_cert[] = "/tmp/fuzz_cert_XXXXXX";
    mkstemp(temp_cert);
    
    FILE *f = fopen(temp_cert, "wb");
    fwrite(data, 1, size, f);
    fclose(f);
    
    SslConfig config = {
        .cert_file = temp_cert,
        .key_file = "test/certs/test.key",
        .verify_peer = false
    };
    
    SslServerCtx *ctx = ssl_server_ctx_create(&config);
    
    // 清理
    unlink(temp_cert);
    if (ctx) ssl_server_ctx_destroy(ctx);
    
    return 0;
}
```

### 2.3 TLS 版本兼容测试

#### 2.3.1 TLS 1.2/1.3 兼容性

```c
void test_tls_version_compatibility() {
    // 项目限制最低 TLS 1.2
    // SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION)
    
    // 测试各版本
    const uint16_t tls_versions[] = {
        0x0300,  // SSL 3.0（应被拒绝）
        0x0301,  // TLS 1.0（应被拒绝）
        0x0302,  // TLS 1.1（应被拒绝）
        0x0303,  // TLS 1.2（应被接受）
        0x0304,  // TLS 1.3（应被接受）
    };
    
    for (int i = 0; i < 5; i++) {
        // 构造 ClientHello 并测试
        // ...
    }
}
```

#### 2.3.2 协议降级攻击测试

```c
void test_protocol_downgrade() {
    // 测试服务器是否拒绝降级到不安全版本
    
    // ClientHello 声明 TLS 1.0
    uint8_t downgrade_client_hello[] = {
        0x16, 0x03, 0x01,        // TLS 1.0 record
        0x01,                    // ClientHello
        // ... 声明 TLS 1.0
    };
    
    // 预期: ssl_handshake 返回 SSL_STATE_ERROR
}
```

### 2.4 Session Ticket 边界测试

#### 2.4.1 Session Ticket 解析测试

```c
int fuzz_session_ticket(const uint8_t *data, size_t size) {
    // TLS 1.3 Session Ticket 格式
    // 服务器需要正确处理:
    // - 空 ticket
    // - 过长 ticket
    // - 无效加密 ticket
    // - 过期 ticket
    
    SslConfig config = {
        .cert_file = "test/certs/test.crt",
        .key_file = "test/certs/test.key",
        .enable_tickets = true,
        .session_timeout = 300
    };
    
    SslServerCtx *ctx = ssl_server_ctx_create(&config);
    if (!ctx) return 0;
    
    // 验证 ticket 配置
    assert(ssl_is_session_ticket_enabled(ctx) == true);
    
    ssl_server_ctx_destroy(ctx);
    return 0;
}
```

#### 2.4.2 Session Resumption 测试

```c
void fuzz_session_resumption() {
    // 测试 session cache 边界
    // session_timeout 配置
    
    const int timeout_values[] = {
        -1,     // 负值（应使用默认 300）
        0,      // 零值
        1,      // 1 秒
        300,    // 默认 5 分钟
        86400,  // 24 小时
        INT_MAX // 最大值
    };
    
    for (int i = 0; i < sizeof(timeout_values)/sizeof(timeout_values[0]); i++) {
        SslConfig config = {
            .cert_file = "test/certs/test.crt",
            .key_file = "test/certs/test.key",
            .session_timeout = timeout_values[i],
            .enable_tickets = true
        };
        
        SslServerCtx *ctx = ssl_server_ctx_create(&config);
        
        // 应处理各种超时值，不崩溃
        if (ctx) ssl_server_ctx_destroy(ctx);
    }
}
```

---

## 测试框架选择

### 3.1 libFuzzer 配置建议

libFuzzer 是 LLVM/Clang 内置的 Fuzz 测试引擎，适合与项目现有的 coverage 配置集成。

#### 3.1.1 编译配置

```cmake
# CMakeLists.txt 添加 Fuzz 目标
option(ENABLE_FUZZ "Enable fuzz testing with libFuzzer" OFF)

if(ENABLE_FUZZ AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    # libFuzzer 编译选项
    set(FUZZ_COMPILE_FLAGS "-fsanitize=fuzzer,address -fprofile-instr-generate -fcoverage-mapping")
    
    # HTTP Parser Fuzz 目标
    add_executable(fuzz_http_parser test/fuzz/fuzz_http_parser.c)
    target_compile_options(fuzz_http_parser PRIVATE ${FUZZ_COMPILE_FLAGS})
    target_link_options(fuzz_http_parser PRIVATE ${FUZZ_COMPILE_FLAGS})
    target_link_libraries(fuzz_http_parser chase_core)
    
    # SSL Fuzz 目标
    add_executable(fuzz_ssl_wrap test/fuzz/fuzz_ssl_wrap.c)
    target_compile_options(fuzz_ssl_wrap PRIVATE ${FUZZ_COMPILE_FLAGS})
    target_link_options(fuzz_ssl_wrap PRIVATE ${FUZZ_COMPILE_FLAGS})
    target_link_libraries(fuzz_ssl_wrap chase_core OpenSSL::SSL OpenSSL::Crypto)
endif()
```

#### 3.1.2 HTTP Parser Fuzz Harness

```c
/**
 * @file    fuzz_http_parser.c
 * @brief   libFuzzer harness for HTTP Parser
 */

#include "http_parser.h"
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    HttpParser *parser = http_parser_create();
    if (!parser) return 0;
    
    HttpRequest *req = http_request_create();
    if (!req) {
        http_parser_destroy(parser);
        return 0;
    }
    
    size_t consumed = 0;
    ParseResult result = http_parser_parse(parser, req, 
                                           (const char *)data, size, &consumed);
    
    // 测试解压功能（如果数据包含 gzip 标识）
    if (result == PARSE_COMPLETE && http_request_needs_decompression(req)) {
        http_request_decompress_body(req, parser);
    }
    
    http_request_destroy(req);
    http_parser_destroy(parser);
    
    return 0;  // libFuzzer: 0 = 无崩溃
}
```

#### 3.1.3 SSL Fuzz Harness

```c
/**
 * @file    fuzz_ssl_wrap.c
 * @brief   libFuzzer harness for SSL Wrapper
 */

#include "ssl_wrap.h"
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>

// 预加载的测试证书
static SslServerCtx *g_ssl_ctx = NULL;

// 初始化（只在 fuzz 启动时调用一次）
void fuzz_ssl_init(void) {
    SslConfig config = {
        .cert_file = "test/certs/test.crt",
        .key_file = "test/certs/test.key",
        .verify_peer = false,
        .session_timeout = 300,
        .enable_tickets = true
    };
    g_ssl_ctx = ssl_server_ctx_create(&config);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!g_ssl_ctx) return 0;
    if (size < 5) return 0;  // TLS 记录需要至少 5 字节
    
    // 创建 socket pair
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) return 0;
    
    SslConnection *conn = ssl_connection_create(g_ssl_ctx, fds[0]);
    if (!conn) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }
    
    // 发送 fuzz 数据模拟客户端
    write(fds[1], data, size);
    close(fds[1]);
    
    // 尝试握手和读数据
    SslState state = ssl_handshake(conn);
    
    if (state == SSL_STATE_CONNECTED || state == SSL_STATE_WANT_READ) {
        char buf[4096];
        ssl_read(conn, buf, sizeof(buf));
    }
    
    ssl_connection_destroy(conn);
    close(fds[0]);
    
    return 0;
}

// 清理（可选）
void fuzz_ssl_cleanup(void) {
    if (g_ssl_ctx) ssl_server_ctx_destroy(g_ssl_ctx);
}
```

#### 3.1.4 运行命令

```bash
# 编译 fuzz 目标
cmake -B build -DENABLE_FUZZ=ON -DCMAKE_C_COMPILER=clang
cmake --build build

# 运行 HTTP Parser fuzz（1 小时）
./build/fuzz_http_parser -max_total_time=3600 -max_len=65536 -rss_limit_mb=512 corpus/

# 运行 SSL fuzz
./build/fuzz_ssl_wrap -max_total_time=3600 -max_len=16384 corpus_ssl/

# 生成 coverage 报告
./build/fuzz_http_parser -runs=100000 -print_coverage=1
llvm-profdata merge -sparse default.profraw -o fuzz.profdata
llvm-cov show ./build/fuzz_http_parser -instr-profile=fuzz.profdata
```

### 3.2 AFL 配置建议

AFL (American Fuzzy Lop) 是经典的 Fuzz 工具，适合生成更复杂的测试输入。

#### 3.2.1 编译配置

```bash
# 使用 afl-gcc 编译
export CC=afl-gcc
cmake -B build-afl -DENABLE_FUZZ=OFF -DENABLE_TESTS=OFF
cmake --build build-afl

# 或使用 afl-clang-fast（更快）
export CC=afl-clang-fast
cmake -B build-afl-fast
cmake --build build-afl-fast
```

#### 3.2.2 独立 Fuzz 程序

```c
/**
 * @file    afl_http_parser.c
 * @brief   AFL harness for HTTP Parser
 */

#include "http_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    // AFL 从 stdin 读取数据
    char buf[65536];
    ssize_t len = read(STDIN_FILENO, buf, sizeof(buf) - 1);
    if (len < 0) return 1;
    buf[len] = '\0';
    
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    
    size_t consumed = 0;
    http_parser_parse(parser, req, buf, len, &consumed);
    
    http_request_destroy(req);
    http_parser_destroy(parser);
    
    return 0;
}
```

#### 3.2.3 AFL 运行命令

```bash
# 创建初始 corpus（种子输入）
mkdir -p corpus_http
cat > corpus_http/get_request.txt << 'EOF'
GET / HTTP/1.1
Host: localhost

EOF

cat > corpus_http/post_request.txt << 'EOF'
POST /api HTTP/1.1
Host: localhost
Content-Length: 5

Hello
EOF

cat > corpus_http/chunked_request.txt << 'EOF'
POST / HTTP/1.1
Host: localhost
Transfer-Encoding: chunked

5
Hello
0

EOF

# 运行 AFL
afl-fuzz -i corpus_http -o findings_http -M fuzzer01 ./build-afl/afl_http_parser

# 并行运行（多核）
afl-fuzz -i corpus_http -o findings_http -S fuzzer02 ./build-afl/afl_http_parser
afl-fuzz -i corpus_http -o findings_http -S fuzzer03 ./build-afl/afl_http_parser
```

### 3.3 独立 Fuzz 测试程序设计

#### 3.3.1 综合测试程序

```c
/**
 * @file    standalone_fuzz.c
 * @brief   独立 Fuzz 测试程序（不依赖 libFuzzer/AFL）
 */

#include "http_parser.h"
#include "ssl_wrap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

#define MAX_ITERATIONS 1000000
#define MAX_INPUT_SIZE 65536

// 统计计数器
static unsigned long total_tests = 0;
static unsigned long parse_errors = 0;
static unsigned long zip_bombs = 0;
static unsigned long ssl_errors = 0;

// 信号处理（超时）
static volatile bool timeout_flag = false;
void handle_timeout(int sig) {
    timeout_flag = true;
}

// 生成随机 HTTP 请求
void generate_random_http(char *buf, size_t *size) {
    // 随机选择测试场景
    int scenario = rand() % 10;
    
    switch (scenario) {
    case 0:  // GET
        snprintf(buf, MAX_INPUT_SIZE, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
        *size = strlen(buf);
        break;
        
    case 1:  // POST with body
        snprintf(buf, MAX_INPUT_SIZE, "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: %d\r\n\r%n", 
                 rand() % 1000, (int *)size);
        break;
        
    case 2:  // Chunked
        // ... chunked request generation
        break;
        
    case 3:  // Malformed
        // ... malformed request
        break;
        
    default:
        // 随机字节
        *size = rand() % 1000 + 10;
        for (size_t i = 0; i < *size; i++) {
            buf[i] = rand() % 256;
        }
        break;
    }
}

// HTTP Parser fuzz 测试
void fuzz_http_parser(void) {
    char buf[MAX_INPUT_SIZE];
    size_t size;
    
    generate_random_http(buf, &size);
    
    HttpParser *parser = http_parser_create();
    HttpRequest *req = http_request_create();
    
    size_t consumed = 0;
    ParseResult result = http_parser_parse(parser, req, buf, size, &consumed);
    
    if (result == PARSE_ERROR) parse_errors++;
    
    // 测试解压
    if (result == PARSE_COMPLETE && http_request_needs_decompression(req)) {
        DecompressResult dr = http_request_decompress_body(req, parser);
        if (dr == DECOMPRESS_ZIP_BOMB) zip_bombs++;
    }
    
    http_request_destroy(req);
    http_parser_destroy(parser);
    total_tests++;
}

// SSL fuzz 测试
void fuzz_ssl_wrapper(void) {
    // ... SSL fuzz logic
}

// 主函数
int main(int argc, char **argv) {
    unsigned long iterations = (argc > 1) ? atol(argv[1]) : MAX_ITERATIONS;
    unsigned long timeout_sec = (argc > 2) ? atol(argv[2]) : 3600;
    
    printf("=== Standalone Fuzz Testing ===\n");
    printf("Iterations: %lu\n", iterations);
    printf("Timeout: %lu seconds\n", timeout_sec);
    printf("Seed: %u\n", (unsigned)time(NULL));
    
    srand(time(NULL));
    
    // 设置超时
    signal(SIGALRM, handle_timeout);
    alarm(timeout_sec);
    
    while (!timeout_flag && total_tests < iterations) {
        fuzz_http_parser();
        if (total_tests % 10000 == 0) {
            printf("\rTests: %lu, Errors: %lu, ZipBombs: %lu", 
                   total_tests, parse_errors, zip_bombs);
        }
    }
    
    printf("\n\n=== Fuzz Test Results ===\n");
    printf("Total Tests:     %lu\n", total_tests);
    printf("Parse Errors:    %lu\n", parse_errors);
    printf("Zip Bomoms:      %lu\n", zip_bombs);
    printf("SSL Errors:      %lu\n", ssl_errors);
    printf("Status:          %s\n", timeout_flag ? "TIMEOUT" : "COMPLETE");
    
    return 0;
}
```

---

## 测试目标与验收标准

### 4.1 运行目标

| 目标 | HTTP Parser | SSL Wrapper |
|------|-------------|-------------|
| 运行时间 | 1 小时无崩溃 | 1 小时无崩溃 |
| 执行次数 | >= 1,000,000 次 | >= 100,000 次 |
| 内存限制 | 512MB RSS | 256MB RSS |
| 输入大小 | <= 64KB | <= 16KB |

### 4.2 验收标准

#### 4.2.1 必须满足

- [ ] **无崩溃**: 运行期间无 SIGSEGV、SIGABRT、SIGBUS 等致命信号
- [ ] **无内存泄漏**: Valgrind 检测无新增内存泄漏
- [ ] **无 ASAN 错误**: AddressSanitizer 无 buffer-overflow、use-after-free 报告
- [ ] **超时控制**: 单个测试输入处理时间 < 1 秒

#### 4.2.2 建议满足

- [ ] **Coverage >= 80%**: HTTP Parser 行覆盖率 >= 80%
- [ ] **边界覆盖**: 所有边界条件代码路径被触发
- [ ] **错误路径覆盖**: 所有 PARSE_ERROR 返回路径被触发

### 4.3 发现的漏洞分类

| 严重性 | 类型 | 描述 | 修复优先级 |
|--------|------|------|-----------|
| Critical | 崩溃 | SIGSEGV/SIGABRT 导致服务终止 | P0 |
| Critical | RCE | 缓冲区溢出可被利用执行代码 | P0 |
| High | DoS | 无限循环或资源耗尽 | P1 |
| High | Info Leak | 内存泄露敏感数据 | P1 |
| Medium | 逻辑错误 | 解析结果不正确 | P2 |
| Low | 性能问题 | 处理时间过长 | P3 |

---

## CMake 集成

### 5.1 Fuzz 测试目标配置

```cmake
# test/CMakeLists.txt 添加 Fuzz 配置

# Fuzz 测试源文件
set(FUZZ_SOURCES
    fuzz/fuzz_http_parser.c
    fuzz/fuzz_ssl_wrap.c
    fuzz/standalone_fuzz.c
)

# libFuzzer 配置（仅 Clang）
if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND ENABLE_FUZZ)
    # Fuzz 编译选项
    set(FUZZ_FLAGS 
        "-fsanitize=fuzzer,address,undefined"
        "-fprofile-instr-generate"
        "-fcoverage-mapping"
        "-g"
        "-O1"
    )
    
    # HTTP Parser Fuzz
    add_executable(fuzz_http_parser fuzz/fuzz_http_parser.c)
    target_compile_options(fuzz_http_parser PRIVATE ${FUZZ_FLAGS})
    target_link_options(fuzz_http_parser PRIVATE ${FUZZ_FLAGS})
    target_link_libraries(fuzz_http_parser chase_core)
    
    # SSL Fuzz
    add_executable(fuzz_ssl_wrap fuzz/fuzz_ssl_wrap.c)
    target_compile_options(fuzz_ssl_wrap PRIVATE ${FUZZ_FLAGS})
    target_link_options(fuzz_ssl_wrap PRIVATE ${FUZZ_FLAGS})
    target_link_libraries(fuzz_ssl_wrap chase_core OpenSSL::SSL OpenSSL::Crypto)
    
    # 独立 Fuzz（不依赖 libFuzzer）
    add_executable(standalone_fuzz fuzz/standalone_fuzz.c)
    target_compile_options(standalone_fuzz PRIVATE "-g" "-O1")
    target_link_libraries(standalone_fuzz chase_core OpenSSL::SSL OpenSSL::Crypto)
    
    # Corpus 目录
    add_custom_command(TARGET fuzz_http_parser POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/corpus_http
        COMMENT "Creating HTTP corpus directory"
    )
    
    add_custom_command(TARGET fuzz_ssl_wrap POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/corpus_ssl
        COMMENT "Creating SSL corpus directory"
    )
    
    # Fuzz 运行目标
    add_custom_target(run_fuzz_http
        COMMAND fuzz_http_parser
            -max_total_time=3600
            -max_len=65536
            -rss_limit_mb=512
            ${CMAKE_BINARY_DIR}/corpus_http
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running HTTP Parser fuzz for 1 hour"
    )
    
    add_custom_target(run_fuzz_ssl
        COMMAND fuzz_ssl_wrap
            -max_total_time=3600
            -max_len=16384
            -rss_limit_mb=256
            ${CMAKE_BINARY_DIR}/corpus_ssl
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running SSL fuzz for 1 hour"
    )
    
    # 快速 Fuzz 测试（5 分钟）
    add_custom_target(run_fuzz_quick
        COMMAND fuzz_http_parser -max_total_time=300 ${CMAKE_BINARY_DIR}/corpus_http
        COMMAND fuzz_ssl_wrap -max_total_time=300 ${CMAKE_BINARY_DIR}/corpus_ssl
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running quick fuzz tests (5 min each)"
    )
endif()

# AFL 配置（可选）
if(ENABLE_AFL AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(STATUS "AFL requires Clang compiler or afl-gcc")
endif()
```

### 5.2 Corpus 文件结构

```
test/fuzz/
├── fuzz_http_parser.c
├── fuzz_ssl_wrap.c
├── standalone_fuzz.c
└── corpus/
    ├── http/
    │   ├── 01_get_simple.txt          # 简单 GET 请求
    │   ├── 02_post_content_length.txt # POST + Content-Length
    │   ├── 03_chunked_simple.txt      # 简单 chunked
    │   ├── 04_chunked_multi.txt       # 多 chunk
    │   ├── 05_malformed_method.txt    # 无效方法
    │   ├── 06_malformed_path.txt      # 无效路径
    │   ├── 07_malformed_header.txt    # 无效头部
    │   ├── 08_large_header.txt        # 超大头部
    │   ├── 09_large_body.txt          # 超大 body
    │   ├── 10_special_chars.txt       # 特殊字符
    │   ├── 11_gzip_compressed.txt     # gzip 数据
    │   ├── 12_zip_bomb.txt            # Zip Bomb 样例
    │   └── 13_integer_overflow.txt    # 整数溢出
    └── ssl/
        ├── 01_valid_client_hello.bin   # 有效 ClientHello
        ├── 02_invalid_version.bin      # 无效 TLS 版本
        ├── 03_truncated_record.bin     # 截断记录
        ├── 04_invalid_record_type.bin  # 无效记录类型
        ├── 05_large_length.bin         # 超大长度
        └── 06_malformed_handshake.bin  # 无效握手
```

---

## CI 集成建议

### 6.1 GitHub Actions 配置

```yaml
# .github/workflows/fuzz.yml
name: Fuzz Testing

on:
  schedule:
    - cron: '0 2 * * 0'  # 每周日凌晨 2 点
  workflow_dispatch:
    inputs:
      duration:
        description: 'Fuzz duration (seconds)'
        default: '3600'
        required: true

jobs:
  fuzz-http-parser:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y clang llvm libssl-dev zlib1g-dev
      
      - name: Build fuzz targets
        run: |
          cmake -B build -DENABLE_FUZZ=ON -DCMAKE_C_COMPILER=clang
          cmake --build build
      
      - name: Prepare corpus
        run: |
          mkdir -p corpus_http
          cp test/fuzz/corpus/http/*.txt corpus_http/
      
      - name: Run HTTP Parser fuzz
        run: |
          timeout ${{ github.event.inputs.duration || '3600' }} \
            ./build/fuzz_http_parser \
            -max_total_time=${{ github.event.inputs.duration || '3600' }} \
            -max_len=65536 \
            -rss_limit_mb=512 \
            -artifact_prefix=crashes_http \
            corpus_http/
      
      - name: Check for crashes
        run: |
          if [ -d crashes_http ] && [ "$(ls -A crashes_http)" ]; then
            echo "::error::Fuzz testing found crashes!"
            exit 1
          fi
      
      - name: Upload crash artifacts
        if: failure()
        uses: actions/upload-artifact@v3
        with:
          name: fuzz-crashes-http
          path: crashes_http/

  fuzz-ssl:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y clang llvm libssl-dev zlib1g-dev
      
      - name: Build fuzz targets
        run: |
          cmake -B build -DENABLE_FUZZ=ON -DCMAKE_C_COMPILER=clang
          cmake --build build
      
      - name: Run SSL fuzz
        run: |
          timeout ${{ github.event.inputs.duration || '3600' }} \
            ./build/fuzz_ssl_wrap \
            -max_total_time=${{ github.event.inputs.duration || '3600' }} \
            -max_len=16384 \
            -rss_limit_mb=256 \
            -artifact_prefix=crashes_ssl \
            corpus_ssl/
      
      - name: Check for crashes
        run: |
          if [ -d crashes_ssl ] && [ "$(ls -A crashes_ssl)" ]; then
            echo "::error::SSL fuzz testing found crashes!"
            exit 1
          fi
      
      - name: Upload crash artifacts
        if: failure()
        uses: actions/upload-artifact@v3
        with:
          name: fuzz-crashes-ssl
          path: crashes_ssl/

  coverage-report:
    runs-on: ubuntu-latest
    needs: [fuzz-http-parser, fuzz-ssl]
    steps:
      - uses: actions/checkout@v3
      
      - name: Generate coverage report
        run: |
          llvm-profdata merge -sparse default.profraw -o fuzz.profdata
          llvm-cov show ./build/fuzz_http_parser \
            -instr-profile=fuzz.profdata \
            -show-line-counts-or-regions \
            > fuzz_coverage.txt
      
      - name: Upload coverage
        uses: actions/upload-artifact@v3
        with:
          name: fuzz-coverage
          path: fuzz_coverage.txt
```

### 6.2 本地开发脚本

```bash
# scripts/run_fuzz.sh
#!/bin/bash
# 快速运行 fuzz 测试脚本

set -e

BUILD_DIR="${BUILD_DIR:-build}"
DURATION="${FUZZ_DURATION:-300}"  # 默认 5 分钟

echo "=== Fuzz Testing Script ==="
echo "Build dir: $BUILD_DIR"
echo "Duration: $DURATION seconds"

# 检查 fuzz 目标是否存在
if [ ! -f "$BUILD_DIR/fuzz_http_parser" ]; then
    echo "Building fuzz targets..."
    cmake -B "$BUILD_DIR" -DENABLE_FUZZ=ON -DCMAKE_C_COMPILER=clang
    cmake --build "$BUILD_DIR"
fi

# 创建 corpus 目录
mkdir -p "$BUILD_DIR/corpus_http"
mkdir -p "$BUILD_DIR/corpus_ssl"

# 复制种子文件
if [ -d test/fuzz/corpus/http ]; then
    cp test/fuzz/corpus/http/*.txt "$BUILD_DIR/corpus_http/"
fi

# 运行 HTTP Parser fuzz
echo ""
echo "Running HTTP Parser fuzz..."
"$BUILD_DIR/fuzz_http_parser" \
    -max_total_time=$DURATION \
    -max_len=65536 \
    -rss_limit_mb=512 \
    -artifact_prefix="$BUILD_DIR/crashes_http" \
    "$BUILD_DIR/corpus_http"

# 运行 SSL fuzz
echo ""
echo "Running SSL fuzz..."
"$BUILD_DIR/fuzz_ssl_wrap" \
    -max_total_time=$DURATION \
    -max_len=16384 \
    -rss_limit_mb=256 \
    -artifact_prefix="$BUILD_DIR/crashes_ssl" \
    "$BUILD_DIR/corpus_ssl"

# 检查崩溃
echo ""
echo "=== Checking for crashes ==="

if [ -d "$BUILD_DIR/crashes_http" ] && [ "$(ls -A $BUILD_DIR/crashes_http 2>/dev/null)" ]; then
    echo "HTTP Parser crashes found:"
    ls -la "$BUILD_DIR/crashes_http"
    exit 1
fi

if [ -d "$BUILD_DIR/crashes_ssl" ] && [ "$(ls -A $BUILD_DIR/crashes_ssl 2>/dev/null)" ]; then
    echo "SSL crashes found:"
    ls -la "$BUILD_DIR/crashes_ssl"
    exit 1
fi

echo "No crashes found. Fuzz testing passed."
```

---

## 发现的漏洞分类与修复建议

### 7.1 漏洞分类模板

发现漏洞时，按以下模板记录：

```markdown
## Vulnerability Report

### 基本信息
- **发现时间**: YYYY-MM-DD
- **Fuzz 工具**: libFuzzer / AFL
- **触发输入**: [crash-xxx 文件路径]
- **模块**: HTTP Parser / SSL Wrapper
- **函数**: http_parser_parse() / ssl_handshake()

### 漏洞类型
- [ ] 堆缓冲区溢出
- [ ] 栈缓冲区溢出
- [ ] 整数溢出
- [ ] Use-after-free
- [ ] Double-free
- [ ] NULL 指针解引用
- [ ] 内存泄漏
- [ ] 无限循环/DoS
- [ ] 逻辑错误

### 严重性
- Critical / High / Medium / Low

### 触发条件
[描述触发漏洞的具体输入条件]

### ASAN/UBSAN 输出
[粘贴 AddressSanitizer 或 UndefinedBehaviorSanitizer 输出]

### 根因分析
[代码分析，定位问题根源]

### 修复建议
[具体修复方案和代码修改建议]

### 测试验证
[修复后的回归测试方案]
```

### 7.2 修复优先级矩阵

| 严重性 | 影响范围 | 修复时间 | 优先级 |
|--------|---------|---------|--------|
| Critical + RCE | 公网暴露 | 24h 内 | P0 |
| Critical + 崩溃 | 核心模块 | 48h 内 | P0 |
| High + DoS | 高频接口 | 7 天 | P1 |
| Medium + 逻辑错误 | 数据处理 | 14 天 | P2 |
| Low + 性能问题 | 低频路径 | 30 天 | P3 |

### 7.3 常见漏洞修复建议

#### 7.3.1 缓冲区溢出

```c
// 问题代码
char buf[256];
strcpy(buf, input);  // 危险！

// 修复方案
char buf[256];
snprintf(buf, sizeof(buf), "%s", input);  // 安全

// 或使用动态缓冲区
size_t input_len = strlen(input);
char *buf = malloc(input_len + 1);
if (buf) {
    memcpy(buf, input, input_len);
    buf[input_len] = '\0';
}
```

#### 7.3.2 整数溢出

```c
// 问题代码
size_t total = a + b;  // 可能溢出

// 修复方案
if (a > SIZE_MAX - b) {
    return ERROR_OVERFLOW;
}
size_t total = a + b;

// 或使用安全函数
#include <limits.h>
unsigned long safe_add(unsigned long a, unsigned long b) {
    if (a > ULONG_MAX - b) return ULONG_MAX;
    return a + b;
}
```

#### 7.3.3 NULL 指针解引用

```c
// 问题代码
HttpParser *parser = http_parser_create();
parser->state = ...;  // 可能 parser 为 NULL

// 修复方案
HttpParser *parser = http_parser_create();
if (!parser) {
    return PARSE_ERROR;
}
parser->state = ...;
```

#### 7.3.4 DoS 防护

```c
// 问题代码：无限制循环
while (more_data) {
    // 处理数据，可能无限执行
}

// 修复方案：添加限制
int max_iterations = 10000;
while (more_data && iterations < max_iterations) {
    // 处理数据
    iterations++;
}
if (iterations >= max_iterations) {
    return PARSE_ERROR;  // 达到限制，拒绝请求
}
```

---

## 附录：测试用例种子文件

### A. HTTP Parser Corpus 种子

```
# 01_get_simple.txt
GET / HTTP/1.1
Host: localhost

# 02_post_content_length.txt
POST /api HTTP/1.1
Host: localhost
Content-Length: 13

Hello, World!

# 03_chunked_simple.txt
POST /upload HTTP/1.1
Host: localhost
Transfer-Encoding: chunked

5
Hello
0

# 04_gzip_compressed.txt (gzip 压缩的 "Hello World")
POST /api HTTP/1.1
Host: localhost
Content-Encoding: gzip
Content-Length: [compressed_size]

[gzip compressed bytes]

# 05_malformed_null_in_method.txt
G\x00T / HTTP/1.1

# 06_overflow_content_length.txt
POST / HTTP/1.1
Content-Length: 99999999999999999999

# 07_crlf_injection.txt
GET / HTTP/1.1
Host: localhost%0d%0aSet-Cookie: hacked

# 08_path_traversal.txt
GET /../../../etc/passwd HTTP/1.1

# 09_large_header.txt
GET / HTTP/1.1
X-Large: AAAAAAAAA...[64KB]

# 10_invalid_chunk_size.txt
POST / HTTP/1.1
Transfer-Encoding: chunked

ZZ
Hello
0
```

### B. SSL Corpus 种子

```
# 01_valid_tls12_client_hello.bin (二进制)
# TLS 1.2 ClientHello 结构
0x16 0x03 0x01 [length] 0x01 [handshake_length] 
[client_version 0x03 0x03]
[random 32 bytes]
[session_id_length 0x00]
[cipher_suites_length]
[cipher_suites]
[compression_methods_length 0x01 0x00]
[extensions...]

# 02_tls10_downgrade.bin
# 强制 TLS 1.0（应被拒绝）
0x16 0x03 0x01 ... [client_version 0x03 0x01]

# 03_invalid_record_type.bin
# 无效记录类型
0xFF 0x03 0x01 0x00 0x05 ...

# 04_zero_length_record.bin
0x16 0x03 0x01 0x00 0x00

# 05_truncated_client_hello.bin
0x16 0x03 0x01 0x00 0x05 0x01 [不完整]
```

---

## 版本信息

- **文档版本**: v1.0
- **创建日期**: 2026-04-24
- **作者**: minghui.liu
- **项目**: Chase HTTP Server Library
- **目标模块**: HTTP Parser (`http_parser.c`), SSL Wrapper (`ssl_wrap.c`)

---

## 参考资源

1. [libFuzzer Documentation](https://llvm.org/docs/LibFuzzer.html)
2. [AFL User Guide](https://lcamtuf.coredump.cx/afl/)
3. [OWASP Fuzz Testing Guide](https://owasp.org/www-community/Fuzzing)
4. [OpenSSL Fuzz Testing](https://www.openssl.org/docs/fuzz-testing.html)
5. [HTTP/1.1 RFC 7230](https://tools.ietf.org/html/rfc7230)
6. [TLS 1.2 RFC 5246](https://tools.ietf.org/html/rfc5246)
7. [TLS 1.3 RFC 8446](https://tools.ietf.org/html/rfc8446)