#ifdef _WIN32
#include "platform.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Internal structs
 * ---------------------------------------------------------------------- */

typedef struct {
    HANDLE          handle;
    PlatformThreadFn fn;
    void            *arg;
} Win32Thread;

typedef struct {
    CONDITION_VARIABLE cv;
    CRITICAL_SECTION   cs;
} Win32CondVar;

typedef struct {
    HANDLE map;
    void  *ptr;
    size_t size;
} Win32Shm;

typedef struct {
    HANDLE h;
    bool   is_server;
} Win32Pipe;

/* -------------------------------------------------------------------------
 * Thread
 * ---------------------------------------------------------------------- */

static DWORD WINAPI thread_proc(LPVOID arg)
{
    Win32Thread *t = (Win32Thread *)arg;
    t->fn(t->arg);
    return 0;
}

PlatformThread platform_thread_create(PlatformThreadFn fn, void *arg,
                                      PlatformPriority priority,
                                      const char *name)
{
    Win32Thread *t = (Win32Thread *)malloc(sizeof(Win32Thread));
    if (!t) return NULL;
    t->fn  = fn;
    t->arg = arg;

    t->handle = CreateThread(NULL, 0, thread_proc, t, CREATE_SUSPENDED, NULL);
    if (!t->handle) { free(t); return NULL; }

    int prio;
    switch (priority) {
        case PLATFORM_PRIO_REALTIME:     prio = THREAD_PRIORITY_TIME_CRITICAL; break;
        case PLATFORM_PRIO_HIGH:         prio = THREAD_PRIORITY_HIGHEST;       break;
        case PLATFORM_PRIO_ABOVE_NORMAL: prio = THREAD_PRIORITY_ABOVE_NORMAL;  break;
        case PLATFORM_PRIO_NORMAL:       prio = THREAD_PRIORITY_NORMAL;        break;
        case PLATFORM_PRIO_LOW:          prio = THREAD_PRIORITY_BELOW_NORMAL;  break;
        default:                         prio = THREAD_PRIORITY_NORMAL;        break;
    }
    SetThreadPriority(t->handle, prio);

    (void)name; /* SetThreadDescription available on Win10+; skip for now */

    ResumeThread(t->handle);
    return (PlatformThread)t;
}

void platform_thread_join(PlatformThread t)
{
    Win32Thread *wt = (Win32Thread *)t;
    WaitForSingleObject(wt->handle, INFINITE);
}

void platform_thread_destroy(PlatformThread t)
{
    Win32Thread *wt = (Win32Thread *)t;
    CloseHandle(wt->handle);
    free(wt);
}

void platform_thread_set_priority(PlatformPriority priority)
{
    int prio;
    switch (priority) {
        case PLATFORM_PRIO_REALTIME:     prio = THREAD_PRIORITY_TIME_CRITICAL; break;
        case PLATFORM_PRIO_HIGH:         prio = THREAD_PRIORITY_HIGHEST;       break;
        case PLATFORM_PRIO_ABOVE_NORMAL: prio = THREAD_PRIORITY_ABOVE_NORMAL;  break;
        case PLATFORM_PRIO_LOW:          prio = THREAD_PRIORITY_BELOW_NORMAL;  break;
        default:                         prio = THREAD_PRIORITY_NORMAL;        break;
    }
    SetThreadPriority(GetCurrentThread(), prio);
}

/* -------------------------------------------------------------------------
 * Condition variable
 * ---------------------------------------------------------------------- */

PlatformCondVar platform_condvar_create(void)
{
    Win32CondVar *cv = (Win32CondVar *)malloc(sizeof(Win32CondVar));
    if (!cv) return NULL;
    InitializeConditionVariable(&cv->cv);
    InitializeCriticalSection(&cv->cs);
    return (PlatformCondVar)cv;
}

void platform_condvar_signal(PlatformCondVar cv)
{
    WakeConditionVariable(&((Win32CondVar *)cv)->cv);
}

bool platform_condvar_wait(PlatformCondVar cv, uint32_t timeout_ms)
{
    Win32CondVar *w = (Win32CondVar *)cv;
    EnterCriticalSection(&w->cs);
    BOOL ok = SleepConditionVariableCS(&w->cv, &w->cs, (DWORD)timeout_ms);
    LeaveCriticalSection(&w->cs);
    return ok != 0;
}

void platform_condvar_destroy(PlatformCondVar cv)
{
    Win32CondVar *w = (Win32CondVar *)cv;
    DeleteCriticalSection(&w->cs);
    free(w);
}

/* -------------------------------------------------------------------------
 * Shared memory
 * ---------------------------------------------------------------------- */

PlatformShm platform_shm_create(const char *name, size_t size)
{
    Win32Shm *shm = (Win32Shm *)malloc(sizeof(Win32Shm));
    if (!shm) return NULL;
    shm->size = size;

    /* Prefix "Global\" so the object is visible across sessions (requires
     * SeCreateGlobalPrivilege; falls back to "Local\" automatically). */
    char full_name[256];
    snprintf(full_name, sizeof(full_name), "Global\\%s", name);

    shm->map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                  PAGE_READWRITE,
                                  (DWORD)(size >> 32), (DWORD)(size & 0xFFFFFFFF),
                                  full_name);
    if (!shm->map) {
        /* Retry without Global\ prefix */
        shm->map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                      PAGE_READWRITE,
                                      (DWORD)(size >> 32), (DWORD)(size & 0xFFFFFFFF),
                                      name);
    }
    if (!shm->map) { free(shm); return NULL; }

    shm->ptr = MapViewOfFile(shm->map, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!shm->ptr) { CloseHandle(shm->map); free(shm); return NULL; }

    return (PlatformShm)shm;
}

PlatformShm platform_shm_open(const char *name, size_t size)
{
    Win32Shm *shm = (Win32Shm *)malloc(sizeof(Win32Shm));
    if (!shm) return NULL;
    shm->size = size;

    char full_name[256];
    snprintf(full_name, sizeof(full_name), "Global\\%s", name);
    shm->map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, full_name);
    if (!shm->map)
        shm->map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!shm->map) { free(shm); return NULL; }

    shm->ptr = MapViewOfFile(shm->map, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!shm->ptr) { CloseHandle(shm->map); free(shm); return NULL; }

    return (PlatformShm)shm;
}

void *platform_shm_ptr(PlatformShm shm)    { return ((Win32Shm *)shm)->ptr; }

void platform_shm_close(PlatformShm shm)
{
    Win32Shm *s = (Win32Shm *)shm;
    if (s->ptr)  UnmapViewOfFile(s->ptr);
    if (s->map)  CloseHandle(s->map);
    free(s);
}

void platform_shm_unlink(const char *name) { (void)name; /* Windows: handle close destroys object */ }

/* -------------------------------------------------------------------------
 * IPC transport — TCP loopback socket
 *
 * Named pipes on Windows serialise concurrent ReadFile/WriteFile on the
 * same handle, deadlocking our separate Rx/Tx threads.  TCP sockets
 * support true full-duplex concurrent reads and writes.
 * ---------------------------------------------------------------------- */

#include <winsock2.h>
#include "../ipc/ipc_protocol.h"  /* WAVREC_IPC_PORT */

typedef struct {
    SOCKET   sock;
    SOCKET   server_sock; /* only set on the accept-side pipe */
    bool     is_server;
} Win32TcpPipe;

static int s_wsa_init = 0;
static void wsa_ensure_init(void) {
    if (!s_wsa_init) {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        s_wsa_init = 1;
    }
}

PlatformPipe platform_pipe_server_create(const char *name)
{
    (void)name;
    wsa_ensure_init();

    Win32TcpPipe *p = (Win32TcpPipe *)calloc(1, sizeof(Win32TcpPipe));
    if (!p) return NULL;
    p->is_server = true;

    p->server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (p->server_sock == INVALID_SOCKET) { free(p); return NULL; }

    /* Allow fast restart after engine crash */
    int opt = 1;
    setsockopt(p->server_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((u_short)WAVREC_IPC_PORT);

    if (bind(p->server_sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(p->server_sock, 1) == SOCKET_ERROR) {
        closesocket(p->server_sock);
        free(p);
        return NULL;
    }

    p->sock = INVALID_SOCKET;
    return (PlatformPipe)p;
}

PlatformPipe platform_pipe_accept(PlatformPipe server)
{
    Win32TcpPipe *s = (Win32TcpPipe *)server;

    Win32TcpPipe *c = (Win32TcpPipe *)calloc(1, sizeof(Win32TcpPipe));
    if (!c) return NULL;
    c->is_server   = false;
    c->server_sock = INVALID_SOCKET;

    c->sock = accept(s->server_sock, NULL, NULL);
    if (c->sock == INVALID_SOCKET) { free(c); return NULL; }

    /* TCP_NODELAY: disable Nagle — we send small framed messages */
    int flag = 1;
    setsockopt(c->sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag));
    return (PlatformPipe)c;
}

PlatformPipe platform_pipe_client_connect(const char *name)
{
    (void)name;
    wsa_ensure_init();

    Win32TcpPipe *p = (Win32TcpPipe *)calloc(1, sizeof(Win32TcpPipe));
    if (!p) return NULL;
    p->is_server   = false;
    p->server_sock = INVALID_SOCKET;

    p->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (p->sock == INVALID_SOCKET) { free(p); return NULL; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((u_short)WAVREC_IPC_PORT);

    if (connect(p->sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(p->sock);
        free(p);
        return NULL;
    }
    return (PlatformPipe)p;
}

static int tcp_io(SOCKET sock, void *buf, int len, bool write_op)
{
    int done = 0;
    while (done < len) {
        int n = write_op
            ? send(sock, (char *)buf + done, len - done, 0)
            : recv(sock, (char *)buf + done, len - done, 0);
        if (n == 0) return 0;   /* graceful close */
        if (n == SOCKET_ERROR) return -1;
        done += n;
    }
    return done;
}

int platform_pipe_write(PlatformPipe p, const void *data, size_t len)
{
    return tcp_io(((Win32TcpPipe *)p)->sock, (void *)data, (int)len, true);
}

int platform_pipe_read(PlatformPipe p, void *data, size_t len)
{
    return tcp_io(((Win32TcpPipe *)p)->sock, data, (int)len, false);
}

void platform_pipe_close(PlatformPipe p)
{
    Win32TcpPipe *tp = (Win32TcpPipe *)p;
    if (tp->sock        != INVALID_SOCKET) { closesocket(tp->sock);        tp->sock        = INVALID_SOCKET; }
    if (tp->server_sock != INVALID_SOCKET) { closesocket(tp->server_sock); tp->server_sock = INVALID_SOCKET; }
    free(tp);
}

/* -------------------------------------------------------------------------
 * Time
 * ---------------------------------------------------------------------- */

uint64_t platform_time_ms(void)
{
    return (uint64_t)GetTickCount64();
}

uint64_t platform_samples_since_midnight(uint32_t sample_rate)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    uint64_t seconds = (uint64_t)st.wHour   * 3600
                     + (uint64_t)st.wMinute * 60
                     + (uint64_t)st.wSecond;
    uint64_t ms      = st.wMilliseconds;
    return seconds * sample_rate + (ms * sample_rate / 1000);
}

void platform_sleep_ms(uint32_t ms) { Sleep(ms); }

int64_t platform_free_bytes(const char *path)
{
    ULARGE_INTEGER free_bytes;
    if (!GetDiskFreeSpaceExA(path, &free_bytes, NULL, NULL))
        return -1;
    return (int64_t)free_bytes.QuadPart;
}

bool platform_mkdir_p(const char *path)
{
    char buf[MAX_PATH];
    strncpy(buf, path, MAX_PATH - 1);
    buf[MAX_PATH - 1] = '\0';
    /* Replace forward slashes with backslashes */
    for (char *p = buf; *p; p++) if (*p == '/') *p = '\\';

    for (char *p = buf + 1; *p; p++) {
        if (*p == '\\') {
            *p = '\0';
            if (!CreateDirectoryA(buf, NULL)) {
                DWORD err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS) return false;
            }
            *p = '\\';
        }
    }
    if (!CreateDirectoryA(buf, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) return false;
    }
    return true;
}

#endif /* _WIN32 */
