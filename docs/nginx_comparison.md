# Nginx 对照测试方案

## 1. 测试目标

将 Chase HTTP 服务器与 nginx 进行性能对照测试，验证 Chase 的性能水平，并发现优化方向。

### 1.1 测试环境要求

| 项目 | 要求 |
|------|------|
| 操作系统 | Linux/macOS |
| CPU | >= 4 核心 |
| 内存 | >= 8GB |
| 测试工具 | wrk / hey / ab |
| nginx 版本 | >= 1.18 |
| OpenSSL | >= 1.1.1 |

### 1.2 测试原则

1. **公平对比**: 使用相同配置参数（Worker 数量、端口、连接限制）
2. **多场景覆盖**: 静态文件、API 响应、HTTPS
3. **多次采样**: 每个场景运行 3 次取平均值
4. **资源监控**: 同时收集 CPU/内存使用数据

---

## 2. 测试方案设计

### 2.1 服务器配置对比

| 配置项 | nginx | Chase |
|--------|-------|-------|
| Worker 数量 | 4 | 4 |
| 监听端口 | 8080 / 8443 | 8081 / 8444 |
| 最大连接数 | 1024 | 1024 |
| Keep-Alive 超时 | 5s | 5s |
| backlog | 1024 | 1024 |
| SO_REUSEPORT | 启用 | 启用 |

### 2.2 测试场景

| 场景 | 描述 | 测试路径 |
|------|------|----------|
| **静态文件** | 小文件（1KB HTML） | `/index.html` |
| **静态文件** | 中等文件（10KB） | `/medium.html` |
| **静态文件** | 大文件（1MB） | `/large.bin` |
| **API 响应** | JSON API（简单） | `/api` |
| **API 响应** | JSON API（健康检查） | `/health` |
| **HTTPS** | SSL/TLS 加密传输 | `/api` (HTTPS) |
| **高并发** | 1000 连接压力 | `/api` |
| **长连接** | Keep-Alive 性能 | `/api` |

### 2.3 测试工具参数

#### wrk 参数配置

```bash
# 基础测试
wrk -t4 -c100 -d30s --latency http://localhost:${PORT}/api

# 高并发测试
wrk -t8 -c1000 -d60s --latency http://localhost:${PORT}/api

# HTTPS 测试
wrk -t4 -c100 -d30s --latency https://localhost:${PORT}/api
```

#### hey 参数配置（可选）

```bash
# hey 支持更多指标
hey -n 10000 -c 100 -q 10 http://localhost:${PORT}/api
```

---

## 3. Nginx 配置文件示例

### 3.1 nginx.conf（HTTP 测试）

```nginx
# nginx_http.conf
# Chase 对照测试配置 - HTTP

worker_processes 4;
worker_rlimit_nofile 1024;
error_log logs/error.log warn;
pid logs/nginx.pid;

events {
    worker_connections 1024;
    use epoll;  # Linux 使用 epoll，macOS 使用 kqueue
    multi_accept on;
}

http {
    include mime.types;
    default_type application/octet-stream;
    
    # 性能优化
    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;
    keepalive_timeout 5;
    keepalive_requests 100;
    
    # 日志格式
    log_format main '$remote_addr - $remote_user [$time_local] "$request" '
                    '$status $body_bytes_sent "$http_referer" '
                    '"$http_user_agent" "$http_x_forwarded_for"';
    access_log logs/access.log main;
    
    # 测试用静态文件目录
    root /tmp/chase_test_files;
    
    server {
        listen 8080 reuseport backlog=1024;
        server_name localhost;
        
        # 主页
        location = / {
            return 200 '<!DOCTYPE html><html><head><title>Nginx Test</title></head><body><h1>Hello from Nginx!</h1></body></html>';
            add_header Content-Type text/html;
        }
        
        # API 响应
        location = /api {
            return 200 '{"status":"ok","version":"nginx","arch":"worker"}';
            add_header Content-Type application/json;
        }
        
        # 健康检查
        location = /health {
            return 200 '{"status":"healthy"}';
            add_header Content-Type application/json;
        }
        
        # 静态文件
        location /static/ {
            alias /tmp/chase_test_files/;
        }
    }
}
```

### 3.2 nginx.conf（HTTPS 测试）

```nginx
# nginx_https.conf
# Chase 对照测试配置 - HTTPS

worker_processes 4;
worker_rlimit_nofile 1024;
error_log logs/error.log warn;
pid logs/nginx.pid;

events {
    worker_connections 1024;
    use epoll;
    multi_accept on;
}

http {
    include mime.types;
    default_type application/octet-stream;
    
    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;
    keepalive_timeout 5;
    keepalive_requests 100;
    
    # SSL 优化
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_prefer_server_ciphers on;
    ssl_ciphers ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256;
    ssl_session_cache shared:SSL:10m;
    ssl_session_timeout 10m;
    
    server {
        listen 8443 ssl reuseport backlog=1024;
        server_name localhost;
        
        ssl_certificate /Users/ninebot/code/open/dpalgokerneng/self/Chase/test/certs/test.crt;
        ssl_certificate_key /Users/ninebot/code/open/dpalgokerneng/self/Chase/test/certs/test.key;
        
        location = /api {
            return 200 '{"status":"ok","version":"nginx-ssl"}';
            add_header Content-Type application/json;
        }
        
        location = /health {
            return 200 '{"status":"healthy"}';
            add_header Content-Type application/json;
        }
    }
}
```

---

## 4. Chase 配置

### 4.1 测试数据准备

```bash
# 创建测试文件目录
mkdir -p /tmp/chase_test_files

# 创建测试文件
echo '<!DOCTYPE html><html><head><title>Test</title></head><body><h1>Hello</h1></body></html>' > /tmp/chase_test_files/index.html

# 中等文件（10KB）
dd if=/dev/urandom of=/tmp/chase_test_files/medium.html bs=1024 count=10

# 大文件（1MB）
dd if=/dev/urandom of=/tmp/chase_test_files/large.bin bs=1024 count=1024
```

### 4.2 Chase 启动参数

```bash
# HTTP 服务器
./build/examples/production_server 8081 4

# HTTPS 服务器（需要 SSL 配置）
./build/examples/production_server 8444 4 \
    --ssl-cert test/certs/test.crt \
    --ssl-key test/certs/test.key
```

---

## 5. 对照指标

### 5.1 性能指标

| 指标 | 说明 | 测量方法 |
|------|------|----------|
| **吞吐量 (RPS)** | 每秒处理请求数 | wrk `Requests/sec` |
| **P50 延迟** | 50% 请求延迟 | wrk `Latency Distribution` |
| **P90 延迟** | 90% 请求延迟 | wrk `Latency Distribution` |
| **P99 延迟** | 99% 请求延迟 | wrk `Latency Distribution` |
| **平均延迟** | 平均响应时间 | wrk `Avg` |
| **最大延迟** | 最大响应时间 | wrk `Max` |

### 5.2 资源指标

| 擃标 | 说明 | 测量方法 |
|------|------|----------|
| **CPU 使用率** | 服务器 CPU 占用 | `ps -p $PID -o %cpu` |
| **内存使用** | 服务器内存占用 | `ps -p $PID -o rss` |
| **进程数** | Worker 进程数量 | `pgrep -f server_name` |
| **连接数** | 活跃连接数 | `netstat/lsof` |

### 5.3 可靠性指标

| 指标 | 说明 | 测量方法 |
|------|------|----------|
| **错误率** | 非 200 响应比例 | wrk `Non-2xx` |
| **超时率** | 超时请求比例 | wrk `Socket errors` |
| **连接失败** | 连接失败数 | wrk output |

---

## 6. 结果报告模板

### 6.1 测试结果表格

#### HTTP 性能对比（4 Workers, 100 连接）

| 指标 | nginx | Chase | 差异 | 结论 |
|------|-------|-------|------|------|
| 吞吐量 (req/s) | | | | |
| P50 延迟 | | | | |
| P90 延迟 | | | | |
| P99 延迟 | | | | |
| CPU 使用率 | | | | |
| 内存使用 (MB) | | | | |

#### HTTPS 性能对比

| 指标 | nginx | Chase | 差异 | 结论 |
|------|-------|-------|------|------|
| 吞吐量 (req/s) | | | | |
| SSL 握手延迟 | | | | |
| P50 延迟 | | | | |
| CPU 使用率 | | | | |

#### 高并发对比（1000 连接）

| 指标 | nginx | Chase | 差异 | 结论 |
|------|-------|-------|------|------|
| 吞吐量 (req/s) | | | | |
| 错误率 | | | | |
| 最大延迟 | | | | |

### 6.2 性能差异分析模板

```
### 性能差异分析

#### 吞吐量分析
- nginx 吞吐量: X req/s
- Chase 吞吐量: Y req/s
- Chase 相对 nginx: (Y/X) * 100%
- 分析结论: [优/劣/持平]

#### 延迟分析
- P50 差异: ...
- P99 差异: ...
- 延迟稳定性: ...

#### 资源使用分析
- CPU 效率: 吞吐量/CPU使用率
- 内存效率: ...

#### HTTPS 性能分析
- SSL 实现对比: OpenSSL 版本、session cache
- 握手性能: ...
```

### 6.3 结论与建议模板

```
### 结论与建议

#### 整体评价
[Chase 与 nginx 的整体性能对比结论]

#### 优势分析
1. [Chase 的性能优势点]
2. ...

#### 待优化点
1. [性能瓶颈分析]
2. ...

#### 优化建议
1. [具体优化建议]
2. ...

#### 后续测试建议
1. [其他测试场景建议]
2. ...
```

---

## 7. 测试注意事项

### 7.1 测试前准备

1. 关闭其他占用 CPU/网络的进程
2. 清理系统缓存（可选）
3. 验证 nginx 和 Chase 配置一致
4. 检查测试文件准备完毕

### 7.2 测试中监控

1. 使用 `htop` 监控 CPU/内存
2. 使用 `dstat` 监控系统资源
3. 检查服务器日志输出

### 7.3 测试后验证

1. 检查错误日志
2. 验证结果数据完整性
3. 多次运行确认稳定性

---

## 8. 附录

### 8.1 测试环境信息记录模板

```
测试日期: YYYY-MM-DD
操作系统: 
CPU 型号: 
CPU 核心数: 
内存大小: 
nginx 版本: 
Chase 版本: 
wrk 版本: 
OpenSSL 版本: 
```

### 8.2 常见问题排查

| 问题 | 可能原因 | 解决方案 |
|------|----------|----------|
| 吞吐量低 | 配置不一致 | 检查 Worker 数、连接限制 |
| 延迟高 | 系统负载高 | 关闭其他进程 |
| SSL 错误 | 证书问题 | 检查证书路径 |
| 连接失败 | backlog 太小 | 增加 backlog 参数 |