#pragma once
/*
 * Platform abstraction layer.
 * All platform-specific code lives in platform_win32.c / platform_posix.c.
 * Consumers include only this header.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Thread priority levels
 * ---------------------------------------------------------------------- */

typedef enum {
    PLATFORM_PRIO_REALTIME,      /* Audio I/O callback — TIME_CRITICAL / SCHED_FIFO max */
    PLATFORM_PRIO_HIGH,          /* Record / Playback Engine */
    PLATFORM_PRIO_ABOVE_NORMAL,  /* Metering */
    PLATFORM_PRIO_NORMAL,        /* IPC, Waveform */
    PLATFORM_PRIO_LOW,           /* Disk Writer, Transcription */
} PlatformPriority;

/* -------------------------------------------------------------------------
 * Thread
 * ---------------------------------------------------------------------- */

typedef void *PlatformThread;
typedef void (*PlatformThreadFn)(void *arg);

PlatformThread  platform_thread_create(PlatformThreadFn fn, void *arg,
                                       PlatformPriority priority,
                                       const char *name);
void            platform_thread_join(PlatformThread t);
void            platform_thread_destroy(PlatformThread t);

/* Set priority of the calling thread. */
void            platform_thread_set_priority(PlatformPriority priority);

/* -------------------------------------------------------------------------
 * Condition variable (for waking secondary threads without busy-waiting)
 * ---------------------------------------------------------------------- */

typedef void *PlatformCondVar;

PlatformCondVar platform_condvar_create(void);
void            platform_condvar_signal(PlatformCondVar cv);
/* Waits up to timeout_ms milliseconds. Returns true if signalled,
 * false on timeout. */
bool            platform_condvar_wait(PlatformCondVar cv, uint32_t timeout_ms);
void            platform_condvar_destroy(PlatformCondVar cv);

/* -------------------------------------------------------------------------
 * Shared memory
 * ---------------------------------------------------------------------- */

typedef void *PlatformShm;

/* Create or open a named shared memory object of at least `size` bytes.
 * Returns NULL on failure. */
PlatformShm  platform_shm_create(const char *name, size_t size);
PlatformShm  platform_shm_open(const char *name, size_t size);
void        *platform_shm_ptr(PlatformShm shm);
void         platform_shm_close(PlatformShm shm);
/* Remove the backing object (call once, from the creator, on shutdown). */
void         platform_shm_unlink(const char *name);

/* -------------------------------------------------------------------------
 * Named pipe / Unix domain socket
 * ---------------------------------------------------------------------- */

typedef void *PlatformPipe;

/* Server: create and listen. Returns handle on success, NULL on failure. */
PlatformPipe platform_pipe_server_create(const char *name);
/* Server: block until a client connects. Returns the connected handle. */
PlatformPipe platform_pipe_accept(PlatformPipe server);

/* Client: connect to a named server. Returns NULL on failure. */
PlatformPipe platform_pipe_client_connect(const char *name);

/* Write exactly `len` bytes. Returns len on success, -1 on error. */
int          platform_pipe_write(PlatformPipe p, const void *data, size_t len);
/* Read exactly `len` bytes (blocking). Returns len on success, 0 on
 * graceful close, -1 on error. */
int          platform_pipe_read(PlatformPipe p, void *data, size_t len);

void         platform_pipe_close(PlatformPipe p);

/* -------------------------------------------------------------------------
 * Time utilities
 * ---------------------------------------------------------------------- */

/* Monotonic milliseconds (for timeouts and intervals). */
uint64_t platform_time_ms(void);

/* Wall-clock sample offset since midnight, at the given sample rate.
 * Used to populate the BWF BEXT TimeReference field. */
uint64_t platform_samples_since_midnight(uint32_t sample_rate);

/* Sleep the calling thread for ms milliseconds. */
void     platform_sleep_ms(uint32_t ms);

/* Free bytes available on the volume containing path.
 * Returns -1 on error. */
int64_t  platform_free_bytes(const char *path);

/* Create directory and all intermediate components (mkdir -p equivalent).
 * Returns true on success or if the directory already exists. */
bool     platform_mkdir_p(const char *path);
