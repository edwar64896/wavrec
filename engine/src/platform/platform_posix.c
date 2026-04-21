#ifndef _WIN32
#include "platform.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sched.h>

/* -------------------------------------------------------------------------
 * Internal structs
 * ---------------------------------------------------------------------- */

typedef struct {
    pthread_t        tid;
    PlatformThreadFn fn;
    void            *arg;
} PosixThread;

typedef struct {
    pthread_cond_t  cond;
    pthread_mutex_t mutex;
} PosixCondVar;

typedef struct {
    int    fd;
    void  *ptr;
    size_t size;
    char   name[128];
} PosixShm;

typedef struct {
    int  fd;
    bool is_server;
    char path[108]; /* sun_path limit */
} PosixPipe;

/* -------------------------------------------------------------------------
 * Thread
 * ---------------------------------------------------------------------- */

static void *thread_proc(void *arg)
{
    PosixThread *t = (PosixThread *)arg;
    t->fn(t->arg);
    return NULL;
}

PlatformThread platform_thread_create(PlatformThreadFn fn, void *arg,
                                      PlatformPriority priority,
                                      const char *name)
{
    PosixThread *t = (PosixThread *)malloc(sizeof(PosixThread));
    if (!t) return NULL;
    t->fn  = fn;
    t->arg = arg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);

    if (priority == PLATFORM_PRIO_REALTIME || priority == PLATFORM_PRIO_HIGH) {
        struct sched_param sp;
        int policy = SCHED_FIFO;
        sp.sched_priority = (priority == PLATFORM_PRIO_REALTIME)
                            ? sched_get_priority_max(policy)
                            : sched_get_priority_max(policy) - 1;
        /* Best-effort — may fail without root/CAP_SYS_NICE */
        pthread_attr_setschedpolicy(&attr, policy);
        pthread_attr_setschedparam(&attr, &sp);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    }

    if (pthread_create(&t->tid, &attr, thread_proc, t) != 0) {
        pthread_attr_destroy(&attr);
        free(t);
        return NULL;
    }
    pthread_attr_destroy(&attr);

#ifdef __linux__
    if (name) pthread_setname_np(t->tid, name);
#elif defined(__APPLE__)
    /* pthread_setname_np on macOS sets the name of the calling thread only;
     * set from within the thread instead — skip here */
    (void)name;
#endif

    return (PlatformThread)t;
}

void platform_thread_join(PlatformThread t)
{
    pthread_join(((PosixThread *)t)->tid, NULL);
}

void platform_thread_destroy(PlatformThread t) { free(t); }

void platform_thread_set_priority(PlatformPriority priority)
{
    if (priority == PLATFORM_PRIO_REALTIME || priority == PLATFORM_PRIO_HIGH) {
        struct sched_param sp;
        sp.sched_priority = (priority == PLATFORM_PRIO_REALTIME)
                            ? sched_get_priority_max(SCHED_FIFO)
                            : sched_get_priority_max(SCHED_FIFO) - 1;
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }
}

/* -------------------------------------------------------------------------
 * Condition variable
 * ---------------------------------------------------------------------- */

PlatformCondVar platform_condvar_create(void)
{
    PosixCondVar *cv = (PosixCondVar *)malloc(sizeof(PosixCondVar));
    if (!cv) return NULL;
    pthread_cond_init(&cv->cond, NULL);
    pthread_mutex_init(&cv->mutex, NULL);
    return (PlatformCondVar)cv;
}

void platform_condvar_signal(PlatformCondVar cv)
{
    PosixCondVar *p = (PosixCondVar *)cv;
    pthread_mutex_lock(&p->mutex);
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->mutex);
}

bool platform_condvar_wait(PlatformCondVar cv, uint32_t timeout_ms)
{
    PosixCondVar *p = (PosixCondVar *)cv;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&p->mutex);
    int rc = pthread_cond_timedwait(&p->cond, &p->mutex, &ts);
    pthread_mutex_unlock(&p->mutex);
    return rc == 0;
}

void platform_condvar_destroy(PlatformCondVar cv)
{
    PosixCondVar *p = (PosixCondVar *)cv;
    pthread_cond_destroy(&p->cond);
    pthread_mutex_destroy(&p->mutex);
    free(p);
}

/* -------------------------------------------------------------------------
 * Shared memory (POSIX shm_open)
 * ---------------------------------------------------------------------- */

static PosixShm *shm_alloc(const char *name, size_t size, bool create)
{
    PosixShm *shm = (PosixShm *)malloc(sizeof(PosixShm));
    if (!shm) return NULL;
    shm->size = size;
    snprintf(shm->name, sizeof(shm->name), "/%s", name);

    int flags = create ? (O_RDWR | O_CREAT) : O_RDWR;
    shm->fd = shm_open(shm->name, flags, 0600);
    if (shm->fd < 0) { free(shm); return NULL; }

    if (create && ftruncate(shm->fd, (off_t)size) < 0) {
        close(shm->fd); shm_unlink(shm->name); free(shm); return NULL;
    }

    shm->ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm->fd, 0);
    if (shm->ptr == MAP_FAILED) {
        close(shm->fd);
        if (create) shm_unlink(shm->name);
        free(shm);
        return NULL;
    }
    return shm;
}

PlatformShm platform_shm_create(const char *name, size_t size)
{
    return (PlatformShm)shm_alloc(name, size, true);
}

PlatformShm platform_shm_open(const char *name, size_t size)
{
    return (PlatformShm)shm_alloc(name, size, false);
}

void *platform_shm_ptr(PlatformShm shm)    { return ((PosixShm *)shm)->ptr; }

void platform_shm_close(PlatformShm shm)
{
    PosixShm *s = (PosixShm *)shm;
    if (s->ptr != MAP_FAILED) munmap(s->ptr, s->size);
    close(s->fd);
    free(s);
}

void platform_shm_unlink(const char *name)
{
    char full[128];
    snprintf(full, sizeof(full), "/%s", name);
    shm_unlink(full);
}

/* -------------------------------------------------------------------------
 * IPC transport — TCP loopback socket (matches Windows implementation)
 * ---------------------------------------------------------------------- */

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "../ipc/ipc_protocol.h"  /* WAVREC_IPC_PORT */

PlatformPipe platform_pipe_server_create(const char *name)
{
    (void)name;
    PosixPipe *p = (PosixPipe *)calloc(1, sizeof(PosixPipe));
    if (!p) return NULL;
    p->is_server = true;

    p->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (p->fd < 0) { free(p); return NULL; }

    int opt = 1;
    setsockopt(p->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((uint16_t)WAVREC_IPC_PORT);

    if (bind(p->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(p->fd, 1) < 0) {
        close(p->fd); free(p); return NULL;
    }
    return (PlatformPipe)p;
}

PlatformPipe platform_pipe_accept(PlatformPipe server)
{
    PosixPipe *s = (PosixPipe *)server;
    PosixPipe *c = (PosixPipe *)calloc(1, sizeof(PosixPipe));
    if (!c) return NULL;
    c->is_server = false;
    c->fd = accept(s->fd, NULL, NULL);
    if (c->fd < 0) { free(c); return NULL; }

    int flag = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    return (PlatformPipe)c;
}

PlatformPipe platform_pipe_client_connect(const char *name)
{
    (void)name;
    PosixPipe *p = (PosixPipe *)calloc(1, sizeof(PosixPipe));
    if (!p) return NULL;
    p->is_server = false;

    p->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (p->fd < 0) { free(p); return NULL; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((uint16_t)WAVREC_IPC_PORT);

    if (connect(p->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(p->fd); free(p); return NULL;
    }
    return (PlatformPipe)p;
}

static int tcp_io(int fd, void *buf, size_t len, bool write_op)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = write_op ? send(fd, (char *)buf + done, len - done, 0)
                             : recv(fd, (char *)buf + done, len - done, 0);
        if (n == 0) return 0;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        done += (size_t)n;
    }
    return (int)done;
}

int platform_pipe_write(PlatformPipe p, const void *data, size_t len)
{
    return tcp_io(((PosixPipe *)p)->fd, (void *)data, len, true);
}

int platform_pipe_read(PlatformPipe p, void *data, size_t len)
{
    return tcp_io(((PosixPipe *)p)->fd, data, len, false);
}

void platform_pipe_close(PlatformPipe p)
{
    PosixPipe *pp = (PosixPipe *)p;
    if (pp->fd >= 0) close(pp->fd);
    free(pp);
}

/* -------------------------------------------------------------------------
 * Time
 * ---------------------------------------------------------------------- */

uint64_t platform_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

uint64_t platform_samples_since_midnight(uint32_t sample_rate)
{
    time_t     t  = time(NULL);
    struct tm *lt = localtime(&t);
    uint64_t secs = (uint64_t)lt->tm_hour * 3600
                  + (uint64_t)lt->tm_min  * 60
                  + (uint64_t)lt->tm_sec;
    return secs * sample_rate;
}

void platform_sleep_ms(uint32_t ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int64_t platform_free_bytes(const char *path)
{
    struct statvfs st;
    if (statvfs(path, &st) != 0) return -1;
    return (int64_t)st.f_bavail * (int64_t)st.f_bsize;
}

bool platform_mkdir_p(const char *path)
{
    char buf[4096];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

#endif /* !_WIN32 */
