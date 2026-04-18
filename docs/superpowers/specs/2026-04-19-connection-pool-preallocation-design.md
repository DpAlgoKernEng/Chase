# Connection Pool Preallocation Design

**Date**: 2026-04-19
**Project**: Chase (HTTP Server Library)
**Module**: connection pool optimization
**Version**: 1.0

---

## Overview

Optimize connection object allocation by preallocating a pool of Connection structures in each Worker thread, reducing malloc overhead during accept, minimizing memory fragmentation, and improving memory usage predictability.

---

## Design Goals

1. **Reduce allocation latency**: Avoid malloc overhead during accept
2. **Reduce memory fragmentation**: Preallocate large memory block
3. **Predictable memory usage**: Fixed base capacity + monitored dynamic expansion

---

## Core Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| base_capacity | max_connections / worker_count | Per-worker base pool capacity |
| expand_threshold | 10% | Trigger temp malloc when free_count < 10% of base |
| lazy_release_delay | 60s | Free temp connections after 60s idle |

---

## Data Structures

```c
typedef struct ConnectionPool {
    // Preallocated array (fixed memory block)
    Connection* preallocated;      // malloc(base_capacity * sizeof(Connection))
    int base_capacity;
    
    // Free list (linked list)
    Connection* free_list_head;
    Connection* free_list_tail;
    int free_count;
    
    // Active list (linked list)
    Connection* active_list_head;
    int active_count;
    
    // Lazy release queue
    Connection* lazy_release_queue;
    int lazy_release_count;
    
    // Statistics
    int total_allocated;           // Total including temp malloc
    int temp_allocated;            // Temp malloc count only
} ConnectionPool;

// Connection structure extension
struct Connection {
    // ... existing fields ...
    
    // Pool management (internal)
    Connection* next;              // Linked list next pointer
    Connection* prev;              // Linked list prev pointer
    uint64_t release_time;         // For lazy release tracking
    int is_temp_allocated;         // Flag: 0 = preallocated, 1 = temp malloc
};
```

---

## Lifecycle Flow

### 1. Worker Startup (Preallocation)

```c
ConnectionPool* connection_pool_create(int base_capacity) {
    ConnectionPool* pool = malloc(sizeof(ConnectionPool));
    
    // 1. Allocate fixed memory block
    pool->preallocated = malloc(base_capacity * sizeof(Connection));
    pool->base_capacity = base_capacity;
    
    // 2. Initialize each Connection
    for (int i = 0; i < base_capacity; i++) {
        Connection* conn = &pool->preallocated[i];
        conn->fd = -1;
        conn->state = CONN_STATE_CLOSED;
        conn->is_temp_allocated = 0;
        conn->next = NULL;
        conn->prev = NULL;
    }
    
    // 3. Add all to free_list
    pool->free_list_head = &pool->preallocated[0];
    pool->free_list_tail = &pool->preallocated[base_capacity - 1];
    
    for (int i = 0; i < base_capacity - 1; i++) {
        pool->preallocated[i].next = &pool->preallocated[i + 1];
    }
    
    pool->free_count = base_capacity;
    pool->active_count = 0;
    pool->temp_allocated = 0;
    pool->lazy_release_count = 0;
    
    return pool;
}
```

### 2. Accept New Connection (Get from Pool)

```c
Connection* connection_pool_get(ConnectionPool* pool) {
    Connection* conn;
    
    // 1. Check if should expand (threshold-based)
    if (should_expand(pool)) {
        // Trigger temp malloc
        conn = malloc(sizeof(Connection));
        conn->is_temp_allocated = 1;
        conn->fd = -1;
        conn->state = CONN_STATE_CLOSED;
        pool->temp_allocated++;
        pool->total_allocated++;
        
        // Add to active_list
        conn->next = pool->active_list_head;
        conn->prev = NULL;
        if (pool->active_list_head) {
            pool->active_list_head->prev = conn;
        }
        pool->active_list_head = conn;
        pool->active_count++;
        
        return conn;
    }
    
    // 2. Get from free_list (O(1))
    conn = pool->free_list_head;
    pool->free_list_head = conn->next;
    if (pool->free_list_head) {
        pool->free_list_head->prev = NULL;
    } else {
        pool->free_list_tail = NULL;
    }
    pool->free_count--;
    
    // 3. Add to active_list
    conn->next = pool->active_list_head;
    conn->prev = NULL;
    if (pool->active_list_head) {
        pool->active_list_head->prev = conn;
    }
    pool->active_list_head = conn;
    pool->active_count++;
    
    return conn;
}

// Threshold check
int should_expand(ConnectionPool* pool) {
    return pool->free_count < pool->base_capacity * 0.1;
}
```

### 3. Connection Close (Release to Pool)

```c
void connection_pool_release(ConnectionPool* pool, Connection* conn) {
    // 1. Remove from active_list
    if (conn->prev) {
        conn->prev->next = conn->next;
    } else {
        pool->active_list_head = conn->next;
    }
    if (conn->next) {
        conn->next->prev = conn->prev;
    }
    pool->active_count--;
    
    // 2. Reset Connection state
    conn->fd = -1;
    conn->state = CONN_STATE_CLOSED;
    
    // 3. Route based on allocation type
    if (conn->is_temp_allocated == 0) {
        // Preallocated: add to free_list
        conn->next = pool->free_list_head;
        conn->prev = NULL;
        if (pool->free_list_head) {
            pool->free_list_head->prev = conn;
        }
        pool->free_list_head = conn;
        pool->free_count++;
    } else {
        // Temp malloc: add to lazy_release_queue
        conn->release_time = get_current_ms();
        conn->next = pool->lazy_release_queue;
        conn->prev = NULL;
        if (pool->lazy_release_queue) {
            pool->lazy_release_queue->prev = conn;
        }
        pool->lazy_release_queue = conn;
        pool->lazy_release_count++;
    }
}
```

### 4. Lazy Release Timer (Periodic Check)

```c
void connection_pool_lazy_release_check(ConnectionPool* pool) {
    // Called by Worker EventLoop timer (every 10s)
    uint64_t now = get_current_ms();
    Connection* conn = pool->lazy_release_queue;
    
    while (conn) {
        Connection* next = conn->next;
        
        if (now - conn->release_time > 60000) {  // 60 seconds
            // Remove from lazy_release_queue
            if (conn->prev) {
                conn->prev->next = conn->next;
            } else {
                pool->lazy_release_queue = conn->next;
            }
            if (conn->next) {
                conn->next->prev = conn->prev;
            }
            
            // Free memory
            free(conn);
            pool->temp_allocated--;
            pool->total_allocated--;
            pool->lazy_release_count--;
        }
        
        conn = next;
    }
}
```

---

## Configuration Extension

```c
struct HttpConfig {
    // ... existing fields ...
    
    // Connection pool configuration (optional override)
    int connection_pool_size_per_worker;  // 0 = auto calculate
    float connection_pool_expand_threshold; // Default 0.1 (10%)
    int connection_pool_lazy_release_delay_ms; // Default 60000
};
```

**Auto calculation formula**:

```c
int calculate_base_capacity(HttpConfig* cfg, int worker_count) {
    if (cfg->connection_pool_size_per_worker > 0) {
        return cfg->connection_pool_size_per_worker;  // User override
    }
    return cfg->max_connections / worker_count;  // Auto
}
```

---

## Monitoring Interface

```c
// Get pool statistics
typedef struct PoolStats {
    int base_capacity;
    int free_count;
    int active_count;
    int temp_allocated;
    int lazy_release_count;
    float utilization;  // active_count / (base_capacity + temp_allocated)
} PoolStats;

PoolStats connection_pool_get_stats(ConnectionPool* pool);

// Example monitoring output:
// Worker-0 pool: free=2400, active=100, temp=0, lazy=0, utilization=4.0%
// Worker-1 pool: free=2450, active=50, temp=5, lazy=3, utilization=2.0%
```

---

## Testing Criteria

| Test | Acceptance Criteria |
|------|---------------------|
| Preallocation correct | After Worker startup: free_count == base_capacity |
| Get/Release correct | 1000 accept/close cycles: free_count restored to initial |
| Threshold triggered | free_count < 10% causes temp_allocated to increase |
| Lazy release effective | Temp connections idle 60s: temp_allocated decreases |
| Performance improvement | Accept latency vs malloc version: > 50% reduction |

---

## Integration Points

1. **Phase 1**: Add ConnectionPool struct and pool management fields to Connection
2. **Phase 2**: WorkerThread startup calls connection_pool_create()
3. **Phase 2**: Worker add_connection uses connection_pool_get()
4. **Phase 2**: Connection close calls connection_pool_release()
5. **Phase 2**: Worker EventLoop adds lazy_release_check timer (10s interval)

---

## Memory Overhead Analysis

| Item | Size | Calculation |
|------|------|-------------|
| Connection struct | ~200 bytes | fd, state, buffers, ssl, timers, pool fields |
| Preallocated block | base_capacity * 200 | e.g., 2500 * 200 = 500KB per worker |
| Linked list pointers | 2 pointers per conn | Included in Connection struct |
| Total per worker | ~500KB + temp | Predictable, stable |

**Comparison with malloc-per-accept**:

| Scenario | malloc-per-accept | Pool preallocation |
|----------|-------------------|--------------------|
| 10000 connections peak | 10000 malloc calls | 4 * 2500 prealloc |
| Memory fragmentation | High | Low (large block) |
| Accept latency | malloc overhead | O(1) pointer op |
| Memory predictability | Unpredictable | Fixed base + monitored temp |

---

## Risk Analysis

| Risk | Mitigation |
|------|------------|
| Pool exhaustion | Threshold-based early expansion + temp malloc fallback |
| Memory waste | Auto-calculate base_capacity, lazy release temp connections |
| Lazy release timer overhead | 10s interval, O(n) scan but n is small (temp_allocated) |

---

## Summary

This design provides a simple, efficient connection pool with:
- O(1) get/release operations
- Predictable memory usage (fixed base + monitored temp)
- Threshold-based early expansion (avoids boundary contention)
- Lazy release for temp connections (memory stable over time)

Implementation is straightforward (~100 lines) and integrates cleanly with existing Worker/Connection architecture.