#include "eventloop.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* 平台检测 */
#if defined(__linux__)
    #define USE_EPOLL 1
    #include <sys/epoll.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    #define USE_KQUEUE 1
    #include <sys/event.h>
#else
    #define USE_POLL 1
    #include <poll.h>
#endif

/* 事件项结构 */
typedef struct {
    int fd;
    uint32_t events;
    EventCallback callback;
    void *user_data;
} EventEntry;

/* EventLoop 结构体 */
struct EventLoop {
    int backend_fd;          /* epoll/kqueue/poll fd */
    int max_events;
    int running;
    EventEntry *entries;     /* fd -> EventEntry 映射 */
    int entries_capacity;
};

/* ========== 平台后端实现 ========== */

#ifdef USE_EPOLL
/* Linux epoll 后端 */

static int backend_init_epoll(EventLoop *loop) {
    loop->backend_fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->backend_fd < 0) {
        return -1;
    }
    return 0;
}

static void backend_destroy_epoll(EventLoop *loop) {
    if (loop->backend_fd >= 0) {
        close(loop->backend_fd);
    }
}

static int backend_add_epoll(EventLoop *loop, int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events = 0;
    if (events & EV_READ) ev.events |= EPOLLIN;
    if (events & EV_WRITE) ev.events |= EPOLLOUT;
    if (events & EV_ERROR) ev.events |= EPOLLERR;
    if (events & EV_CLOSE) ev.events |= EPOLLHUP | EPOLLRDHUP;
    ev.data.fd = fd;
    return epoll_ctl(loop->backend_fd, EPOLL_CTL_ADD, fd, &ev);
}

static int backend_modify_epoll(EventLoop *loop, int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events = 0;
    if (events & EV_READ) ev.events |= EPOLLIN;
    if (events & EV_WRITE) ev.events |= EPOLLOUT;
    if (events & EV_ERROR) ev.events |= EPOLLERR;
    if (events & EV_CLOSE) ev.events |= EPOLLHUP | EPOLLRDHUP;
    ev.data.fd = fd;
    return epoll_ctl(loop->backend_fd, EPOLL_CTL_MOD, fd, &ev);
}

static int backend_remove_epoll(EventLoop *loop, int fd) {
    struct epoll_event ev;
    return epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, &ev);
}

static int backend_poll_epoll(EventLoop *loop, int timeout_ms,
                               void (*callback)(int fd, uint32_t events, void *data),
                               EventEntry *entries) {
    struct epoll_event *events = malloc(loop->max_events * sizeof(struct epoll_event));
    if (!events) return -1;

    int n = epoll_wait(loop->backend_fd, events, loop->max_events, timeout_ms);
    if (n < 0) {
        free(events);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        int fd = events[i].data.fd;
        uint32_t ev = 0;
        if (events[i].events & EPOLLIN) ev |= EV_READ;
        if (events[i].events & EPOLLOUT) ev |= EV_WRITE;
        if (events[i].events & EPOLLERR) ev |= EV_ERROR;
        if (events[i].events & (EPOLLHUP | EPOLLRDHUP)) ev |= EV_CLOSE;

        /* 查找对应的 Entry */
        if (fd >= 0 && fd < loop->entries_capacity && entries[fd].callback) {
            entries[fd].callback(fd, ev, entries[fd].user_data);
        }
    }

    free(events);
    return n;
}

#endif /* USE_EPOLL */

#ifdef USE_KQUEUE
/* macOS/BSD kqueue 后端 */

static int backend_init_kqueue(EventLoop *loop) {
    loop->backend_fd = kqueue();
    if (loop->backend_fd < 0) {
        return -1;
    }
    return 0;
}

static void backend_destroy_kqueue(EventLoop *loop) {
    if (loop->backend_fd >= 0) {
        close(loop->backend_fd);
    }
}

static int backend_add_kqueue(EventLoop *loop, int fd, uint32_t events) {
    struct kevent ev[2];
    int n = 0;

    if (events & EV_READ) {
        EV_SET(&ev[n], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
        n++;
    }
    if (events & EV_WRITE) {
        EV_SET(&ev[n], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
        n++;
    }

    if (n == 0) return 0;
    return kevent(loop->backend_fd, ev, n, NULL, 0, NULL);
}

static int backend_modify_kqueue(EventLoop *loop, int fd, uint32_t events) {
    /* kqueue modify 需要先删除再添加 */
    struct kevent ev[4];
    int n = 0;

    /* 删除所有 */
    EV_SET(&ev[n], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    n++;
    EV_SET(&ev[n], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    n++;

    /* 添加新的 */
    if (events & EV_READ) {
        EV_SET(&ev[n], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
        n++;
    }
    if (events & EV_WRITE) {
        EV_SET(&ev[n], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
        n++;
    }

    return kevent(loop->backend_fd, ev, n, NULL, 0, NULL);
}

static int backend_remove_kqueue(EventLoop *loop, int fd) {
    struct kevent ev[2];
    EV_SET(&ev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    /* kqueue EV_DELETE 对不存在的 filter 返回 ENOENT，忽略这个错误 */
    int ret = kevent(loop->backend_fd, ev, 2, NULL, 0, NULL);
    if (ret < 0 && errno != ENOENT) {
        return -1;
    }
    return 0;
}

static int backend_poll_kqueue(EventLoop *loop, int timeout_ms,
                                void (*callback)(int fd, uint32_t events, void *data),
                                EventEntry *entries) {
    struct kevent *events = malloc(loop->max_events * sizeof(struct kevent));
    if (!events) return -1;

    struct timespec ts;
    struct timespec *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        tsp = &ts;
    }

    int n = kevent(loop->backend_fd, NULL, 0, events, loop->max_events, tsp);
    if (n < 0) {
        free(events);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        int fd = (int)events[i].ident;
        uint32_t ev = 0;
        if (events[i].filter == EVFILT_READ) ev |= EV_READ;
        if (events[i].filter == EVFILT_WRITE) ev |= EV_WRITE;
        if (events[i].flags & EV_ERROR) ev |= EV_ERROR;
        if (events[i].flags & EV_EOF) ev |= EV_CLOSE;

        /* 查找对应的 Entry */
        if (fd >= 0 && fd < loop->entries_capacity && entries[fd].callback) {
            entries[fd].callback(fd, ev, entries[fd].user_data);
        }
    }

    free(events);
    return n;
}

#endif /* USE_KQUEUE */

#ifdef USE_POLL
/* 通用 poll 后端 */

static int backend_init_poll(EventLoop *loop) {
    loop->backend_fd = -1;  /* poll 不需要特殊 fd */
    return 0;
}

static void backend_destroy_poll(EventLoop *loop) {
    /* poll 无需清理 */
}

static int backend_add_poll(EventLoop *loop, int fd, uint32_t events) {
    /* poll 在 poll 时动态处理 */
    return 0;
}

static int backend_modify_poll(EventLoop *loop, int fd, uint32_t events) {
    return 0;
}

static int backend_remove_poll(EventLoop *loop, int fd) {
    return 0;
}

static int backend_poll_poll(EventLoop *loop, int timeout_ms,
                              void (*callback)(int fd, uint32_t events, void *data),
                              EventEntry *entries) {
    struct pollfd *pfds = malloc(loop->max_events * sizeof(struct pollfd));
    if (!pfds) return -1;

    /* 构建 pollfd 数组 */
    int n_fds = 0;
    for (int i = 0; i < loop->entries_capacity && n_fds < loop->max_events; i++) {
        if (entries[i].callback) {
            pfds[n_fds].fd = i;
            pfds[n_fds].events = 0;
            if (entries[i].events & EV_READ) pfds[n_fds].events |= POLLIN;
            if (entries[i].events & EV_WRITE) pfds[n_fds].events |= POLLOUT;
            pfds[n_fds].revents = 0;
            n_fds++;
        }
    }

    if (n_fds == 0) {
        free(pfds);
        return 0;
    }

    int n = poll(pfds, n_fds, timeout_ms);
    if (n < 0) {
        free(pfds);
        return -1;
    }

    /* 处理事件 */
    for (int i = 0; i < n_fds; i++) {
        if (pfds[i].revents) {
            int fd = pfds[i].fd;
            uint32_t ev = 0;
            if (pfds[i].revents & POLLIN) ev |= EV_READ;
            if (pfds[i].revents & POLLOUT) ev |= EV_WRITE;
            if (pfds[i].revents & POLLERR) ev |= EV_ERROR;
            if (pfds[i].revents & POLLHUP) ev |= EV_CLOSE;

            if (fd >= 0 && fd < loop->entries_capacity && entries[fd].callback) {
                entries[fd].callback(fd, ev, entries[fd].user_data);
            }
        }
    }

    free(pfds);
    return n;
}

#endif /* USE_POLL */

/* ========== 公共 API 实现 ========== */

EventLoop *eventloop_create(int max_events) {
    EventLoop *loop = malloc(sizeof(EventLoop));
    if (!loop) return NULL;

    loop->max_events = max_events;
    loop->running = 0;

    /* 预分配 entries 数组（按 FD 数量） */
    loop->entries_capacity = 1024;  /* 初始容量 */
    loop->entries = malloc(loop->entries_capacity * sizeof(EventEntry));
    if (!loop->entries) {
        free(loop);
        return NULL;
    }
    memset(loop->entries, 0, loop->entries_capacity * sizeof(EventEntry));

    /* 初始化后端 */
#ifdef USE_EPOLL
    if (backend_init_epoll(loop) < 0) {
        free(loop->entries);
        free(loop);
        return NULL;
    }
#elif defined(USE_KQUEUE)
    if (backend_init_kqueue(loop) < 0) {
        free(loop->entries);
        free(loop);
        return NULL;
    }
#elif defined(USE_POLL)
    backend_init_poll(loop);
#endif

    return loop;
}

void eventloop_destroy(EventLoop *loop) {
    if (!loop) return;

#ifdef USE_EPOLL
    backend_destroy_epoll(loop);
#elif defined(USE_KQUEUE)
    backend_destroy_kqueue(loop);
#elif defined(USE_POLL)
    backend_destroy_poll(loop);
#endif

    free(loop->entries);
    free(loop);
}

int eventloop_add(EventLoop *loop, int fd, uint32_t events,
                  EventCallback cb, void *user_data) {
    if (!loop || fd < 0) return -1;

    /* 扩容 entries 数组 */
    if (fd >= loop->entries_capacity) {
        int new_cap = fd + 1;
        if (new_cap < loop->entries_capacity * 2) {
            new_cap = loop->entries_capacity * 2;
        }
        EventEntry *new_entries = realloc(loop->entries, new_cap * sizeof(EventEntry));
        if (!new_entries) return -1;
        memset(new_entries + loop->entries_capacity, 0,
               (new_cap - loop->entries_capacity) * sizeof(EventEntry));
        loop->entries = new_entries;
        loop->entries_capacity = new_cap;
    }

    /* 设置 entry */
    loop->entries[fd].fd = fd;
    loop->entries[fd].events = events;
    loop->entries[fd].callback = cb;
    loop->entries[fd].user_data = user_data;

    /* 添加到后端 */
#ifdef USE_EPOLL
    return backend_add_epoll(loop, fd, events);
#elif defined(USE_KQUEUE)
    return backend_add_kqueue(loop, fd, events);
#elif defined(USE_POLL)
    return backend_add_poll(loop, fd, events);
#endif
}

int eventloop_modify(EventLoop *loop, int fd, uint32_t events) {
    if (!loop || fd < 0 || fd >= loop->entries_capacity) return -1;
    if (!loop->entries[fd].callback) return -1;  /* 未注册 */

    loop->entries[fd].events = events;

#ifdef USE_EPOLL
    return backend_modify_epoll(loop, fd, events);
#elif defined(USE_KQUEUE)
    return backend_modify_kqueue(loop, fd, events);
#elif defined(USE_POLL)
    return backend_modify_poll(loop, fd, events);
#endif
}

int eventloop_remove(EventLoop *loop, int fd) {
    if (!loop || fd < 0 || fd >= loop->entries_capacity) return -1;

    loop->entries[fd].callback = NULL;
    loop->entries[fd].user_data = NULL;
    loop->entries[fd].events = 0;

#ifdef USE_EPOLL
    return backend_remove_epoll(loop, fd);
#elif defined(USE_KQUEUE)
    return backend_remove_kqueue(loop, fd);
#elif defined(USE_POLL)
    return backend_remove_poll(loop, fd);
#endif
}

void eventloop_run(EventLoop *loop) {
    if (!loop) return;
    loop->running = 1;

    while (loop->running) {
        eventloop_poll(loop, -1);
    }
}

void eventloop_stop(EventLoop *loop) {
    if (!loop) return;
    loop->running = 0;
}

int eventloop_poll(EventLoop *loop, int timeout_ms) {
    if (!loop) return -1;

#ifdef USE_EPOLL
    return backend_poll_epoll(loop, timeout_ms, NULL, loop->entries);
#elif defined(USE_KQUEUE)
    return backend_poll_kqueue(loop, timeout_ms, NULL, loop->entries);
#elif defined(USE_POLL)
    return backend_poll_poll(loop, timeout_ms, NULL, loop->entries);
#endif
}