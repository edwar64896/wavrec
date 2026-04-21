#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "ipc_protocol.h"
#include "../platform/platform.h"

struct WavRecEngine;

/* -------------------------------------------------------------------------
 * Event queue (MPSC — multiple producers, single IPC Tx consumer)
 *
 * Producers call engine_emit() which acquires a lightweight spinlock,
 * copies the event into the queue, then releases.  The Tx thread reads
 * without a lock (sole consumer).  Queue capacity is generous; if full,
 * the event is silently dropped.
 * ---------------------------------------------------------------------- */

#define IPC_EVENT_QUEUE_SLOTS 32    /* power of two — fewer slots, larger payloads */
#define IPC_MAX_PAYLOAD_LEN   65535 /* EVT_READY device list can be large */

typedef struct {
    uint8_t  msg_type;
    uint16_t payload_len;
    char     payload[IPC_MAX_PAYLOAD_LEN];
} IpcEventItem;

/* -------------------------------------------------------------------------
 * IpcContext
 * ---------------------------------------------------------------------- */

typedef struct IpcContext {
    /* MPSC event queue */
    IpcEventItem      events[IPC_EVENT_QUEUE_SLOTS];
    _Atomic int       lock;   /* spinlock: 0=free, 1=held */
    _Atomic uint32_t  wp;     /* write pos (producers advance under lock) */
    _Atomic uint32_t  rp;     /* read pos  (Tx thread advances, no lock)  */

    /* Message sequence counter */
    _Atomic uint32_t  seq;

    /* Pipe handles */
    PlatformPipe      server;
    PlatformPipe      conn;   /* NULL until client connects */

    /* Transport status cadence */
    uint64_t          last_transport_ms;

    /* Threads */
    void             *rx_thread;
    void             *tx_thread;
    _Atomic int       running;

    struct WavRecEngine *eng;
} IpcContext;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

bool ipc_init(struct WavRecEngine *eng);
void ipc_start(struct WavRecEngine *eng);

/* Enqueue an event for the Tx thread to send.  Safe to call from any thread.
 * Drops silently if queue is full or no client connected. */
void ipc_send_event(struct WavRecEngine *eng, const char *json_payload,
                    unsigned char msg_type);

void ipc_shutdown(struct WavRecEngine *eng);
