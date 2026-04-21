# HTTP 服务器性能优化方案

**创建日期**: 2026-04-21
**项目**: Chase (HTTP Server Library)
**状态**: 待实施

---

## 概述

本文档基于设计文档和实施计划的性能分析，整理了各层面的性能瓶颈及优化方案，供后续优化阶段参考。

---

## 一、架构层面优化

### 1.1 SO_REUSEPORT 潜在问题

| 问题 | 说明 | 影响 |
|------|------|------|
| 内核负载均衡不完美 | Linux 早期版本（3.9-4.5）负载均衡算法简单 | Worker 负载不均衡 |
| 连接迁移问题 | 客户端重连时可能被分发到不同 Worker，SSL Session 无法复用 | SSL 握手开销增加 |
| Worker 数量限制 | 过多 Worker（> CPU 核数）会增加内核调度开销 | CPU 竞争加剧 |

**优化方案**:

```c
// 优化 1: Worker 数量与 CPU 核数匹配
int optimal_workers = sysconf(_SC_NPROCESSORS_ONLN);

// 优化 2: Linux 4.5+ 使用 SO_REUSEPORT_GROUP
#ifdef SO_REUSEPORT_GROUP
setsockopt(fd, SOL_SOCKET, SO_REUSEPORT_GROUP, &optval, sizeof(optval));
#endif

// 优化 3: CPU 亲和性绑定
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(worker_id % cpu_cores, &cpuset);
sched_setaffinity(0, sizeof(cpuset), &cpuset);
```

### 1.2 进程管理开销

| 开销来源 | 时间 | 优化方案 |
|----------|------|----------|
| fork() Worker | ~1ms | 预创建 Worker 池（prefork） |
| Worker 崩溃重启 | ~3s | 快速 fork + 状态恢复 |
| 进程间共享数据 | 需 mmap/shm | 减少 Worker 间依赖 |

**优化方案: 预创建 Worker 池**:

```c
typedef struct {
    pid_t pid;
    int state;  // ACTIVE, IDLE, CRASHED, DISABLED
    int restart_count;
    uint64_t last_crash_time;
} WorkerSlot;

WorkerSlot worker_pool[MAX_WORKERS];

void on_worker_crash(int worker_id) {
    WorkerSlot* slot = &worker_pool[worker_id];
    
    // 防止频繁重启（1秒内最多重启3次）
    if (slot->restart_count > 3 && 
        get_current_ms() - slot->last_crash_time < 1000) {
        log_error("Worker %d crash loop, disabling", worker_id);
        slot->state = DISABLED;
        return;
    }
    
    slot->restart_count++;
    slot->last_crash_time = get_current_ms();
    
    // 快速 fork
    pid_t new_pid = fork();
    if (new_pid == 0) {
        worker_run(worker_id);
    }
    slot->pid = new_pid;
    slot->state = ACTIVE;
}
```

---

## 二、I/O 层面优化

### 2.1 accept 批量处理

**问题**: 单次 accept() 只返回一个连接，高并发时开销大

**优化方案 1: 批量 accept**:

```c
#ifdef __linux__
int accept_batch(int server_fd, int* client_fds, int max_count) {
    int count = 0;
    while (count < max_count) {
        int fd = accept4(server_fd, NULL, NULL, 
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EMFILE) {
                handle_fd_exhaustion();
                break;
            }
            return -1;
        }
        client_fds[count++] = fd;
    }
    return count;
}
#endif
```

**优化方案 2: 边缘触发模式 (ET)**:

```c
// 使用 epoll ET 模式
struct epoll_event ev;
ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

// ET 模式必须循环 accept 直到 EAGAIN
void on_accept_event_et(int server_fd) {
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EAGAIN) break;
            // 错误处理
        }
        handle_new_connection(client_fd);
    }
}
```

**预期收益**: +20% 吞吐量

### 2.2 缓冲区大小配置

| 参数 | 当前设计 | 问题 | 优化建议 |
|------|----------|------|----------|
| read_buf_cap | 16KB | 可能不足以容纳完整请求 | 动态调整 |
| write_buf_cap | 64KB | 大响应需要多次 write | chunked 分段 |
| socket buffer | 未提及 | 内核缓冲区默认值可能不够 | 调整 SO_RCVBUF/SO_SNDBUF |

**优化方案**:

```c
// 1. 动态缓冲区大小
size_t get_optimal_buf_size(HttpRequest* req) {
    if (req->method == HTTP_POST && req->content_length > 0) {
        return MIN(req->content_length + 1024, MAX_BUF_SIZE);
    }
    return DEFAULT_BUF_SIZE;  // 16KB
}

// 2. 调整 socket 缓冲区
void optimize_socket_buffer(int fd) {
    int rcvbuf = 256 * 1024;  // 256KB
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    
    int sndbuf = 512 * 1024;  // 512KB
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
}

// 3. TCP_NODELAY（禁用 Nagle 算法）
int nodelay = 1;
setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

// 4. TCP_QUICKACK（Linux 特有）
#ifdef TCP_QUICKACK
int quickack = 1;
setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
#endif
```

**预期收益**: +10% 吞吐量

### 2.3 epoll/kqueue 配置优化

| 参数 | 当前设计 | 优化建议 |
|------|----------|----------|
| max_events | 未明确 | 1024-4096 |
| epoll_wait timeout | 未明确 | 根据 Timer 最小值动态计算 |
| 触发模式 | 未明确 | ET 模式更高效 |

**优化方案**:

```c
#define MAX_EVENTS 1024

// 动态 timeout 计算
int calculate_timeout(EventLoop* loop) {
    Timer* top = timer_heap_peek(&loop->timer_heap);
    if (!top) return -1;
    
    uint64_t now = get_current_ms();
    int timeout = (int)(top->expire_time - now);
    return timeout < 0 ? 0 : timeout;
}

// ET 模式循环读
void on_read_event_et(int fd) {
    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN) break;
            // 错误处理
        }
        if (n == 0) break;  // 连接关闭
        // 处理数据
    }
}
```

**预期收益**: +15% 吞吐量

---

## 三、HTTP 解析层面优化

### 3.1 解析状态机效率

**问题**: 逐字节解析效率低

**优化方案 1: SIMD 加速（查找分隔符）**:

```c
#ifdef __AVX2__
#include <immintrin.h>

int find_delimiter_simd(const char* data, size_t len, char delim) {
    __m256i delim_vec = _mm256_set1_epi8(delim);
    
    size_t i = 0;
    for (; i + 32 <= len; i += 32) {
        __m256i data_vec = _mm256_loadu_si256((__m256i*)(data + i));
        __m256i cmp = _mm256_cmpeq_epi8(data_vec, delim_vec);
        int mask = _mm256_movemask_epi8(cmp);
        if (mask) {
            return i + __builtin_ctz(mask);
        }
    }
    
    for (; i < len; i++) {
        if (data[i] == delim) return i;
    }
    return -1;
}
#endif
```

**优化方案 2: 批量解析 Header**:

```c
int parse_headers_batch(const char* data, size_t len, HttpHeader* headers) {
    int line_ends[MAX_HEADERS];
    int line_count = 0;
    
    // SIMD 查找 \r\n
    for (size_t i = 0; i < len - 1 && line_count < MAX_HEADERS; i++) {
        if (data[i] == '\r' && data[i+1] == '\n') {
            line_ends[line_count++] = i;
        }
    }
    
    // 批量解析
    int start = 0;
    for (int i = 0; i < line_count; i++) {
        parse_header_line(data + start, line_ends[i] - start, &headers[i]);
        start = line_ends[i] + 2;
    }
    
    return line_count;
}
```

**预期收益**: +15% 解析性能

### 3.2 字符串处理优化

| 操作 | 当前开销 | 优化方案 |
|------|----------|----------|
| Header 名称查找 | O(n) 遍历 | 哈希表 O(1) |
| 字符串复制 | malloc + memcpy | 引用计数 + 零拷贝 |
| 内存分配 | 多次 malloc | 内存池预分配 |

**优化方案: Header 哈希表**:

```c
#define HEADER_HASH_SIZE 32

typedef struct {
    char* name;
    char* value;
} HttpHeader;

typedef struct {
    HttpHeader headers[HEADER_HASH_SIZE];
    int count;
} HeaderTable;

// 完美哈希（预计算常见 Header）
static const struct {
    const char* name;
    uint32_t hash;
} known_headers[] = {
    {"Content-Length", 0x12345678},
    {"Content-Type", 0x23456789},
    {"Host", 0x34567890},
    {"Connection", 0x45678901},
    {"Accept", 0x56789012},
};

uint32_t header_hash(const char* name) {
    for (int i = 0; i < sizeof(known_headers)/sizeof(known_headers[0]); i++) {
        if (strcasecmp(name, known_headers[i].name) == 0) {
            return known_headers[i].hash % HEADER_HASH_SIZE;
        }
    }
    return generic_hash(name) % HEADER_HASH_SIZE;
}

// 零拷贝 Header 存储
typedef struct {
    const char* name_start;
    size_t name_len;
    const char* value_start;
    size_t value_len;
    Buffer* source_buf;  // 引用计数
} ZeroCopyHeader;
```

---

## 四、SSL/TLS 层面优化

### 4.1 SSL 握手开销

| 环节 | 开销 | 占比 | 优化方案 |
|------|------|------|----------|
| 证书验证 | ~20ms | 40% | OCSP Stapling |
| 密钥交换 | ~10ms | 20% | TLS 1.3 (0-RTT) |
| Session 复用 | ~5ms | 10% | Session Ticket |
| Cipher 操作 | ~5ms | 10% | 硬件加速 (AES-NI) |

**优化方案 1: OCSP Stapling**:

```c
void ssl_enable_ocsp_stapling(SSL_CTX* ctx, const char* cert_file) {
    unsigned char* ocsp_response = NULL;
    size_t ocsp_len = 0;
    
    if (fetch_ocsp_response(cert_file, &ocsp_response, &ocsp_len) == 0) {
        SSL_CTX_set_ocsp_response(ctx, ocsp_response, ocsp_len);
    }
    
    // 定期更新（每小时）
    eventloop_add_timer(loop, 3600000, refresh_ocsp, ctx);
}
```

**优化方案 2: TLS 1.3 0-RTT**:

```c
void ssl_enable_0rtt(SSL_CTX* ctx) {
    SSL_CTX_set_max_early_data(ctx, 1024);
    SSL_CTX_set_recv_max_early_data(ctx, 1024);
}
```

**优化方案 3: 硬件加速**:

```c
void ssl_enable_hw_acceleration(SSL_CTX* ctx) {
    SSL_CTX_set_ciphersuites(ctx, 
        "TLS_AES_128_GCM_SHA256:"      // AES-NI 优化
        "TLS_AES_256_GCM_SHA384:"
        "TLS_CHACHA20_POLY1305_SHA256" // 无 AES-NI 时使用
    );
}
```

**优化方案 4: Session Ticket 密钥轮换**:

```c
void ssl_rotate_ticket_keys(SSLContext* ctx) {
    unsigned char new_keys[48];
    RAND_bytes(new_keys, 48);
    SSL_CTX_set_tlsext_ticket_keys(ctx->ssl_ctx, new_keys, 48);
    
    // 每小时轮换
    eventloop_add_timer(ctx->loop, 3600000, ssl_rotate_ticket_keys, ctx);
}
```

**预期收益**: +30% SSL 握手性能

### 4.2 SSL 数据读写优化

**优化方案: 批量读写 + 缓冲区配置**:

```c
int ssl_read_batch(SSL* ssl, Buffer* buf, size_t max_len) {
    size_t total = 0;
    while (total < max_len) {
        int n = SSL_read(ssl, buf->data + buf->tail, 
                         buf->capacity - buf->size);
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ) break;
            return -1;
        }
        buf->tail = (buf->tail + n) % buf->capacity;
        buf->size += n;
        total += n;
    }
    return total;
}

void ssl_optimize_buffers(SSL* ssl) {
    SSL_set_read_ahead(ssl, 1);  // 预读模式
    SSL_set_max_send_fragment(ssl, 16384);  // 16KB
}
```

---

## 五、路由层面优化

### 5.1 路由匹配算法

| 匹配类型 | 当前设计 | 问题 | 优化方案 |
|----------|----------|------|----------|
| 精确匹配 | strcmp | O(n) 遍历 | 哈希表 O(1) |
| 前缀匹配 | strncmp | O(n) 遍历 | Trie 树 O(m) |
| 正则匹配 | regex | O(n*m) 慢 | 预编译 + 缓存 |

**优化方案 1: 路由哈希表**:

```c
#define ROUTE_HASH_SIZE 256

typedef struct {
    Route* routes[ROUTE_HASH_SIZE];
    TrieNode* prefix_trie;
    Route** regex_routes;
    int regex_count;
} RouterOptimized;

uint32_t route_hash(const char* path, HttpMethod method) {
    uint32_t h = hash_string(path);
    h ^= (uint32_t)method;
    return h % ROUTE_HASH_SIZE;
}

Route* router_match_optimized(RouterOptimized* r, const char* path, 
                               HttpMethod method) {
    // 1. 哈希查找精确匹配
    uint32_t h = route_hash(path, method);
    if (r->routes[h] && strcmp(r->routes[h]->pattern, path) == 0) {
        return r->routes[h];
    }
    
    // 2. Trie 查找前缀匹配
    Route* route = trie_match(r->prefix_trie, path);
    if (route) return route;
    
    // 3. 正则匹配（按优先级）
    for (int i = 0; i < r->regex_count; i++) {
        if (regex_match(r->regex_routes[i]->regex, path)) {
            return r->regex_routes[i];
        }
    }
    
    return NULL;
}
```

**优化方案 2: Trie 树（前缀匹配）**:

```c
typedef struct TrieNode {
    char ch;
    Route* route;
    struct TrieNode** children;
    int child_count;
} TrieNode;

Route* trie_match(TrieNode* root, const char* path) {
    TrieNode* node = root;
    Route* last_match = NULL;
    
    for (size_t i = 0; path[i]; i++) {
        TrieNode* child = find_child(node, path[i]);
        if (!child) break;
        
        node = child;
        if (node->route) last_match = node->route;
    }
    
    return last_match;
}
```

**优化方案 3: 正则结果缓存**:

```c
#define REGEX_CACHE_SIZE 128

typedef struct {
    char* path;
    Route* route;
    uint64_t timestamp;
} RegexCacheEntry;

RegexCacheEntry regex_cache[REGEX_CACHE_SIZE];

Route* regex_match_cached(Router* r, const char* path) {
    uint32_t h = hash_string(path) % REGEX_CACHE_SIZE;
    
    if (regex_cache[h].path && strcmp(regex_cache[h].path, path) == 0) {
        return regex_cache[h].route;
    }
    
    Route* route = regex_match_slow(r, path);
    
    free(regex_cache[h].path);
    regex_cache[h].path = strdup(path);
    regex_cache[h].route = route;
    
    return route;
}
```

**预期收益**: +5% 路由匹配性能

---

## 六、内存层面优化

### 6.1 内存分配开销

| 场景 | malloc 调用次数 | 优化方案 |
|------|----------------|----------|
| 每请求 | ~10 次 | 对象池 |
| SSL 连接 | SSL_new + 内部缓冲区 | SSL 连接池 |
| 大响应 | 大 Buffer malloc | 预分配 + 分段 |

**优化方案 1: Slab Allocator**:

```c
#define SLAB_SLOT_COUNT 8
#define SLAB_BATCH_SIZE 64

typedef struct {
    size_t slot_sizes[SLAB_SLOT_COUNT];  // 64, 128, 256, 512, 1024, 2048, 4096, 8192
    void** free_lists[SLAB_SLOT_COUNT];
    int free_counts[SLAB_SLOT_COUNT];
    pthread_mutex_t lock;
} SlabAllocator;

void* slab_alloc(SlabAllocator* slab, size_t size) {
    int slot = find_slot_index(size);
    if (slot >= SLAB_SLOT_COUNT) return malloc(size);
    
    pthread_mutex_lock(&slab->lock);
    
    if (slab->free_counts[slot] > 0) {
        void* ptr = slab->free_lists[slot][--slab->free_counts[slot]];
        pthread_mutex_unlock(&slab->lock);
        return ptr;
    }
    
    // 预分配一批
    for (int i = 0; i < SLAB_BATCH_SIZE; i++) {
        slab->free_lists[slot][slab->free_counts[slot++] = 
            malloc(slab->slot_sizes[slot]);
    }
    
    void* ptr = slab->free_lists[slot][--slab->free_counts[slot]];
    pthread_mutex_unlock(&slab->lock);
    return ptr;
}

void slab_free(SlabAllocator* slab, void* ptr, size_t size) {
    int slot = find_slot_index(size);
    if (slot >= SLAB_SLOT_COUNT) {
        free(ptr);
        return;
    }
    
    pthread_mutex_lock(&slab->lock);
    slab->free_lists[slot][slab->free_counts[slot++] = ptr;
    pthread_mutex_unlock(&slab->lock);
}
```

**优化方案 2: Connection 对象池**:

```c
#define MAX_CONNECTIONS 10000

typedef struct {
    Connection connections[MAX_CONNECTIONS];
    int free_indices[MAX_CONNECTIONS];
    int free_count;
    pthread_mutex_t lock;
} ConnectionPool;

Connection* connection_pool_get(ConnectionPool* pool, int fd, EventLoop* loop) {
    pthread_mutex_lock(&pool->lock);
    
    if (pool->free_count == 0) {
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }
    
    int idx = pool->free_indices[--pool->free_count];
    Connection* conn = &pool->connections[idx];
    
    conn->fd = fd;
    conn->state = CONN_STATE_CONNECTING;
    conn->loop = loop;
    
    pthread_mutex_unlock(&pool->lock);
    return conn;
}

void connection_pool_put(ConnectionPool* pool, Connection* conn) {
    pthread_mutex_lock(&pool->lock);
    
    int idx = conn - pool->connections;
    connection_reset(conn);
    pool->free_indices[pool->free_count++] = idx;
    
    pthread_mutex_unlock(&pool->lock);
}
```

**优化方案 3: jemalloc/tcmalloc**:

```bash
# 编译时链接更高效的内存分配器
cmake -DCMAKE_EXE_LINKER_FLAGS="-ljemalloc" ..

# 或
cmake -DCMAKE_EXE_LINKER_FLAGS="-ltcmalloc" ..
```

**预期收益**: +20% 内存分配性能

### 6.2 内存对齐优化

```c
#define CACHE_LINE_SIZE 64

// 缓存行对齐
typedef struct __attribute__((aligned(CACHE_LINE_SIZE))) {
    // 经常访问的字段（热点数据）
    int fd;
    ConnState state;
    EventLoop* loop;
    Buffer* read_buf;
    Buffer* write_buf;
    
    // 填充到缓存行边界
    char _pad[CACHE_LINE_SIZE - sizeof(int) - sizeof(ConnState) 
              - sizeof(EventLoop*) - sizeof(Buffer*) * 2];
    
    // 不常访问的字段（分开缓存行）
    char client_ip[64];
    SSL* ssl;
    Timer* timeout_timer;
} ConnectionAligned;

// Buffer 对齐（AVX2 需要 32 字节）
Buffer* buffer_create_aligned(size_t capacity) {
    Buffer* buf = malloc(sizeof(Buffer));
    buf->data = aligned_alloc(32, capacity);
    buf->capacity = capacity;
    return buf;
}
```

---

## 七、系统层面优化

### 7.1 CPU 亲和性

```c
void set_worker_affinity(int worker_id, int cpu_cores) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(worker_id % cpu_cores, &cpuset);
    
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
    
    log_info("Worker %d bound to CPU %d", worker_id, worker_id % cpu_cores);
}
```

**预期收益**: +10% 多核性能

### 7.2 NUMA 优化

```c
#ifdef __linux__
#include <numa.h>

void optimize_numa(int worker_id) {
    if (numa_available() >= 0) {
        int node_count = numa_num_configured_nodes();
        numa_set_preferred(worker_id % node_count);
    }
}
#endif
```

**预期收益**: +5% 多 CPU 服务器性能

### 7.3 网络参数调优

```bash
# /etc/sysctl.conf

# 最大连接数
net.core.somaxconn = 65535

# TCP 缓冲区
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.ipv4.tcp_rmem = 4096 87380 16777216
net.ipv4.tcp_wmem = 4096 65536 16777216

# TCP 连接时间优化
net.ipv4.tcp_fin_timeout = 30
net.ipv4.tcp_tw_reuse = 1

# SYN Flood 防护
net.ipv4.tcp_syncookies = 1
net.ipv4.tcp_max_syn_backlog = 8192

# Keepalive 优化
net.ipv4.tcp_keepalive_time = 600
net.ipv4.tcp_keepalive_intvl = 30
net.ipv4.tcp_keepalive_probes = 3
```

```c
// 应用代码调优
void tune_system_limits() {
    struct rlimit rl;
    rl.rlim_cur = 100000;
    rl.rlim_max = 100000;
    setrlimit(RLIMIT_NOFILE, &rl);
}
```

### 7.4 高精度定时器

```c
#ifdef __linux__
#include <sys/timerfd.h>

int create_high_precision_timer(uint64_t interval_us) {
    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    
    struct itimerspec spec;
    spec.it_interval.tv_sec = interval_us / 1000000;
    spec.it_interval.tv_nsec = (interval_us % 1000000) * 1000;
    spec.it_value = spec.it_interval;
    
    timerfd_settime(timerfd, 0, &spec, NULL);
    
    // 加入 epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = timerfd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timerfd, &ev);
    
    return timerfd;
}
#endif

#ifdef __APPLE__
// kqueue EVFILT_TIMER
void create_macos_timer(int kq, int timer_id, uint64_t interval_ms) {
    struct kevent ev;
    EV_SET(&ev, timer_id, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
           NOTE_MSECONDS, interval_ms, callback);
    kevent(kq, &ev, 1, NULL, 0, NULL);
}
#endif
```

---

## 八、大文件传输优化

### 8.1 sendfile 分段优化

```c
int send_large_file_chunked(int out_fd, int in_fd, size_t file_size,
                              size_t chunk_size, ProgressCallback cb) {
    off_t offset = 0;
    size_t remaining = file_size;
    
    while (remaining > 0) {
        size_t to_send = MIN(chunk_size, remaining);
        
        #ifdef __linux__
        ssize_t sent = sendfile(out_fd, in_fd, &offset, to_send);
        #elif defined(__APPLE__)
        off_t sent_len = 0;
        sendfile(in_fd, out_fd, offset, &sent_len, NULL, 0);
        ssize_t sent = sent_len;
        offset += sent_len;
        #endif
        
        if (sent <= 0) {
            if (errno == EAGAIN) {
                wait_for_write_ready(out_fd);
                continue;
            }
            return -1;
        }
        
        remaining -= sent;
        if (cb) cb(file_size - remaining, file_size);
    }
    
    return 0;
}
```

### 8.2 文件预读优化

```c
#ifdef __linux__
void optimize_file_read(int fd, off_t offset, size_t len) {
    // 提示内核预读
    posix_fadvise(fd, offset, len, POSIX_FADV_SEQUENTIAL);
    
    // 大文件预读
    if (len > 1024 * 1024) {
        readahead(fd, offset, len);
    }
}
#endif
```

### 8.3 静态文件缓存

```c
#define MAX_CACHE_ENTRIES 1024
#define CACHE_EXPIRE_MS 60000

typedef struct {
    char* path;
    int fd;
    size_t size;
    time_t mtime;
    uint64_t last_access;
    int ref_count;
} FileCacheEntry;

typedef struct {
    FileCacheEntry entries[MAX_CACHE_ENTRIES];
    int count;
    pthread_mutex_t lock;
} FileCache;

int file_cache_open(FileCache* cache, const char* path, 
                    FileCacheEntry** entry) {
    pthread_mutex_lock(&cache->lock);
    
    // 查缓存
    for (int i = 0; i < cache->count; i++) {
        if (strcmp(cache->entries[i].path, path) == 0) {
            struct stat st;
            stat(path, &st);
            
            if (st.st_mtime == cache->entries[i].mtime) {
                cache->entries[i].ref_count++;
                cache->entries[i].last_access = get_current_ms();
                *entry = &cache->entries[i];
                pthread_mutex_unlock(&cache->lock);
                return 0;
            }
            
            // 文件已修改，重新打开
            close(cache->entries[i].fd);
            cache->entries[i].fd = open(path, O_RDONLY);
            cache->entries[i].mtime = st.st_mtime;
            cache->entries[i].size = st.st_size;
            cache->entries[i].ref_count++;
            *entry = &cache->entries[i];
            pthread_mutex_unlock(&cache->lock);
            return 0;
        }
    }
    
    // 新文件加入缓存
    if (cache->count < MAX_CACHE_ENTRIES) {
        FileCacheEntry* e = &cache->entries[cache->count++];
        e->path = strdup(path);
        e->fd = open(path, O_RDONLY);
        struct stat st;
        stat(path, &st);
        e->size = st.st_size;
        e->mtime = st.st_mtime;
        e->ref_count = 1;
        e->last_access = get_current_ms();
        *entry = e;
        pthread_mutex_unlock(&cache->lock);
        return 0;
    }
    
    // 缓存满，LRU 淘汰
    int oldest = 0;
    uint64_t oldest_time = cache->entries[0].last_access;
    for (int i = 1; i < cache->count; i++) {
        if (cache->entries[i].last_access < oldest_time &&
            cache->entries[i].ref_count == 0) {
            oldest = i;
            oldest_time = cache->entries[i].last_access;
        }
    }
    
    if (cache->entries[oldest].ref_count == 0) {
        close(cache->entries[oldest].fd);
        free(cache->entries[oldest].path);
        
        cache->entries[oldest].path = strdup(path);
        cache->entries[oldest].fd = open(path, O_RDONLY);
        struct stat st;
        stat(path, &st);
        cache->entries[oldest].size = st.st_size;
        cache->entries[oldest].mtime = st.st_mtime;
        cache->entries[oldest].ref_count = 1;
        cache->entries[oldest].last_access = get_current_ms();
        *entry = &cache->entries[oldest];
        pthread_mutex_unlock(&cache->lock);
        return 0;
    }
    
    pthread_mutex_unlock(&cache->lock);
    return -1;
}

void file_cache_close(FileCache* cache, FileCacheEntry* entry) {
    pthread_mutex_lock(&cache->lock);
    entry->ref_count--;
    
    if (entry->ref_count == 0 && 
        get_current_ms() - entry->last_access > CACHE_EXPIRE_MS) {
        close(entry->fd);
        free(entry->path);
        // 从缓存移除（标记为空）
        entry->path = NULL;
        entry->fd = -1;
    }
    
    pthread_mutex_unlock(&cache->lock);
}
```

**预期收益**: +50% 静态文件吞吐量

---

## 九、性能对比目标

### 9.1 与 nginx 对比

| 指标 | nginx | 本项目目标 | 优化后目标 |
|------|-------|-----------|-----------|
| 单核吞吐 | 50k req/s | 30k req/s | **50k req/s** |
| SSL 吞吐 | 20k req/s | 10k req/s | **15k req/s** |
| 静态文件 | sendfile | 分段 sendfile | **接近 nginx** |
| 内存占用 | ~10MB/Worker | 未优化 | **8-10MB/Worker** |

### 9.2 优化优先级

| 优化项 | 预期收益 | 实现难度 | 优先级 | 实施阶段 |
|--------|----------|----------|--------|----------|
| 批量 accept | +20% | 低 | **P0** | Phase 2 |
| epoll ET 模式 | +15% | 低 | **P0** | Phase 1 |
| 缓冲区大小调优 | +10% | 低 | **P0** | Phase 1 |
| TCP_NODELAY/QUICKACK | +5% | 低 | **P0** | Phase 1 |
| 内存池 (Slab) | +20% | 中 | **P1** | Phase 5 |
| Connection 对象池 | +10% | 中 | **P1** | Phase 5 |
| CPU 亲和性 | +10% | 低 | **P1** | Phase 2 |
| 路由哈希表 | +5% | 中 | **P1** | Phase 5 |
| SSL Session Ticket | +30%（握手） | 中 | **P1** | Phase 4 |
| OCSP Stapling | +20%（握手） | 中 | **P1** | Phase 4 |
| SIMD HTTP 解析 | +15% | 高 | **P2** | Phase 6 |
| 文件缓存 | +50%（静态） | 高 | **P2** | Phase 5 |
| NUMA 优化 | +5%（多核） | 高 | **P3** | Phase 6 |
| jemalloc/tcmalloc | +10% | 低 | **P3** | Phase 6 |

---

## 十、实施建议

### 10.1 Phase 1 优化（核心框架）

- [ ] epoll ET 模式
- [ ] 缓冲区大小配置优化
- [ ] TCP_NODELAY 启用
- [ ] 动态 timeout 计算

### 10.2 Phase 2 优化（多进程）

- [ ] 批量 accept
- [ ] CPU 亲和性绑定
- [ ] Worker 数量与 CPU 核数匹配
- [ ] 防崩溃循环重启限制

### 10.3 Phase 4 优化（SSL）

- [ ] TLS 1.3 Session Ticket
- [ ] OCSP Stapling
- [ ] AES-NI 硬件加速
- [ ] Session Ticket 密钥轮换

### 10.4 Phase 5 优化（完善功能）

- [ ] Slab Allocator
- [ ] Connection 对象池
- [ ] 路由哈希表 + Trie
- [ ] 正则结果缓存
- [ ] 静态文件缓存

### 10.5 Phase 6 优化（优化）

- [ ] SIMD Header 解析
- [ ] NUMA 优化
- [ ] jemalloc/tcmalloc 集成
- [ ] 高精度定时器（timerfd）
- [ ] 性能对比测试（vs nginx）

---

## 十一、监控与验证

### 11.1 性能监控指标

| 指标 | 工具 | 目标值 |
|------|------|--------|
| 吞吐量 | wrk | ≥ 50k req/s |
| 延迟 P99 | wrk --latency | < 5ms |
| SSL 握手延迟 | openssl s_client | < 30ms |
| 内存占用 | top/htop | < 10MB/Worker |
| CPU 使用率 | top/htop | < 80%（满负载） |
| 连接均衡度 | 自定义脚本 | 偏差 < 5% |

### 11.2 验证脚本

```bash
#!/bin/bash
# scripts/performance_verify.sh

echo "=== 性能验证测试 ==="

# 1. 吞吐量测试
echo ">>> 吞吐量测试"
wrk -t4 -c100 -d30s --latency http://localhost:8080/ > throughput.log

# 2. SSL 性能测试
echo ">>> SSL 握手延迟测试"
for i in {1..10}; do
    time openssl s_client -connect localhost:8443 -sess_out session.pem 2>&1 | grep "real"
    openssl s_client -connect localhost:8443 -sess_in session.pem 2>&1 | grep "real"
done > ssl_latency.log

# 3. Worker 负载均衡
echo ">>> Worker 负载均衡测试"
# 统计各 Worker 连接数
for pid in $(pgrep -f "worker"); do
    echo "Worker $pid: $(lsof -p $pid | grep -c TCP)"
done > load_balance.log

# 4. 内存占用
echo ">>> 内存占用测试"
ps aux | grep -E "master|worker" | awk '{print $2, $6}' > memory.log

echo "=== 验证完成 ==="
```

---

**下一步**: 在 Phase 6 优化阶段实施本文档中的优化方案。