#include "ipc.h"
#include "ipc_protocol.h"
#include "../engine.h"
#include "../platform/platform.h"
#include "../timecode/timecode.h"
#include "../audio/playback_engine.h"
#include "../audio/audio_io.h"
#include "../util/crc32.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static IpcContext *get_ctx(struct WavRecEngine *eng)
{
    return (IpcContext *)engine_ipc(eng);
}

/* -------------------------------------------------------------------------
 * MPSC event queue — spinlock helpers
 * ---------------------------------------------------------------------- */

static void spin_lock(IpcContext *ctx)
{
    int expected = 0;
    while (!atomic_compare_exchange_weak_explicit(
               &ctx->lock, &expected, 1,
               memory_order_acquire, memory_order_relaxed))
        expected = 0;
}

static void spin_unlock(IpcContext *ctx)
{
    atomic_store_explicit(&ctx->lock, 0, memory_order_release);
}

/* -------------------------------------------------------------------------
 * ipc_send_event — safe to call from any thread
 * ---------------------------------------------------------------------- */

void ipc_send_event(struct WavRecEngine *eng,
                    const char *json_payload,
                    unsigned char msg_type)
{
    IpcContext *ctx = get_ctx(eng);
    if (!ctx) return;

    uint16_t len = (uint16_t)(json_payload ? strlen(json_payload) : 0);
    if (len > IPC_MAX_PAYLOAD_LEN) len = IPC_MAX_PAYLOAD_LEN;

    spin_lock(ctx);

    uint32_t w = atomic_load_explicit(&ctx->wp, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&ctx->rp, memory_order_relaxed);

    if ((w - r) < IPC_EVENT_QUEUE_SLOTS) {
        IpcEventItem *item = &ctx->events[w & (IPC_EVENT_QUEUE_SLOTS - 1)];
        item->msg_type    = msg_type;
        item->payload_len = len;
        if (len) memcpy(item->payload, json_payload, len);
        atomic_store_explicit(&ctx->wp, w + 1, memory_order_release);
    }
    /* else: queue full — drop silently */

    spin_unlock(ctx);
}

/* -------------------------------------------------------------------------
 * EVT_READY payload builder
 * ---------------------------------------------------------------------- */

static void send_ready(struct WavRecEngine *eng)
{
    /* Include device list so the UI can populate selectors immediately. */
    fprintf(stderr, "[ipc] send_ready: enumerating devices...\n"); fflush(stderr);
    char *devices = audio_io_list_devices(eng);
    fprintf(stderr, "[ipc] send_ready: devices=%s\n", devices ? devices : "(null)"); fflush(stderr);
    size_t extra  = devices ? strlen(devices) : 2;
    char  *buf    = (char *)malloc(512 + extra);
    if (!buf) { if (devices) free(devices); return; }

    snprintf(buf, 512 + extra,
        "{"
        "\"engine_version\":\"0.1.0\","
        "\"max_channels\":%d,"
        "\"shared_mem_meters\":\"%s\","
        "\"shared_mem_waveforms\":\"%s\","
        "\"sample_rate\":%u,"
        "\"devices\":%s"
        "}",
        WAVREC_MAX_CHANNELS,
        WAVREC_SHM_METERS,
        WAVREC_SHM_WAVEFORMS,
        engine_sample_rate(eng),
        devices ? devices : "[]");
    ipc_send_event(eng, buf, EVT_READY);
    free(buf);
    if (devices) free(devices);
}

/* -------------------------------------------------------------------------
 * EVT_TRANSPORT — emitted by the Tx thread at ~10Hz
 * ---------------------------------------------------------------------- */

static void maybe_emit_transport(struct WavRecEngine *eng, IpcContext *ctx)
{
    uint64_t now = platform_time_ms();
    if (now - ctx->last_transport_ms < 100) return;
    ctx->last_transport_ms = now;

    uint64_t frame        = engine_frame_counter(eng);
    uint32_t sample_rate  = engine_sample_rate(eng);
    const WavRecTimecodeSource *tc = engine_tc(eng);

    char tc_str[12] = "00:00:00:00";
    if (tc && sample_rate > 0) {
        uint64_t tc_frames = tc->locked
            ? tc_frames_now(tc, frame, sample_rate)
            : tc_frames_wallclock(tc, sample_rate);
        tc_format(tc, tc_frames, tc_str, sizeof(tc_str));
    }

    uint64_t play_head = playback_engine_play_head(eng);

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"record_head_frames\":%llu,"
        "\"play_head_frames\":%llu,"
        "\"recording\":%s,"
        "\"playing\":%s,"
        "\"timecode\":\"%s\"}",
        (unsigned long long)frame,
        (unsigned long long)play_head,
        engine_is_recording(eng) ? "true" : "false",
        engine_is_playing(eng)   ? "true" : "false",
        tc_str);
    ipc_send_event(eng, buf, EVT_TRANSPORT);
}

/* -------------------------------------------------------------------------
 * Write a framed message to the pipe
 * ---------------------------------------------------------------------- */

static bool pipe_write_msg(PlatformPipe conn,
                           uint8_t msg_type, uint32_t seq,
                           const char *payload, uint16_t len)
{
    WavRecMsgHeader hdr;
    hdr.magic          = WAVREC_MSG_MAGIC;
    hdr.msg_type       = msg_type;
    hdr.flags          = 0;
    hdr.payload_length = len;
    hdr.sequence       = seq;
    hdr.crc32          = len ? crc32_compute(payload, len) : 0;

    if (platform_pipe_write(conn, &hdr, sizeof(hdr)) != (int)sizeof(hdr))
        return false;
    if (len && platform_pipe_write(conn, payload, len) != (int)len)
        return false;
    return true;
}

/* -------------------------------------------------------------------------
 * IPC Rx thread — reads commands from the pipe, dispatches to engine
 * ---------------------------------------------------------------------- */

static void ipc_rx_thread(void *arg)
{
    struct WavRecEngine *eng = (struct WavRecEngine *)arg;
    IpcContext          *ctx = get_ctx(eng);

    while (atomic_load_explicit(&ctx->running, memory_order_acquire)) {

        /* Block until a client connects */
        if (!ctx->conn) {
            ctx->conn = platform_pipe_accept(ctx->server);
            if (!ctx->conn) {
                platform_sleep_ms(100);
                continue;
            }
            /* Send EVT_READY now the connection is live */
            send_ready(eng);
            ctx->last_transport_ms = platform_time_ms();
            continue;
        }

        /* Read the fixed-size header */
        WavRecMsgHeader hdr;
        int n = platform_pipe_read(ctx->conn, &hdr, sizeof(hdr));
        if (n == 0) {
            /* Client disconnected */
            platform_pipe_close(ctx->conn);
            ctx->conn = NULL;
            continue;
        }
        if (n < 0 || n != (int)sizeof(hdr)) continue;

        /* Validate magic */
        if (hdr.magic != WAVREC_MSG_MAGIC) continue;

        /* Read payload */
        char payload[65536];
        uint16_t plen = hdr.payload_length;
        if (plen > sizeof(payload)) { plen = 0; } /* guard against huge lengths */
        if (plen > 0) {
            int pn = platform_pipe_read(ctx->conn, payload, plen);
            if (pn != (int)plen) continue;

            /* CRC check */
            if (hdr.crc32 != crc32_compute(payload, plen)) continue;
        }
        payload[plen] = '\0';

        /* Dispatch to engine state machine */
        engine_dispatch(eng, (WavRecMsgType)hdr.msg_type,
                        hdr.sequence,
                        plen ? payload : NULL, plen);
    }
}

/* -------------------------------------------------------------------------
 * IPC Tx thread — drains event queue, writes to pipe
 * ---------------------------------------------------------------------- */

static void ipc_tx_thread(void *arg)
{
    struct WavRecEngine *eng = (struct WavRecEngine *)arg;
    IpcContext          *ctx = get_ctx(eng);

    while (atomic_load_explicit(&ctx->running, memory_order_acquire)) {

        maybe_emit_transport(eng, ctx);

        if (!ctx->conn) {
            platform_sleep_ms(10);
            continue;
        }

        /* Drain event queue */
        bool sent_any = false;
        for (;;) {
            uint32_t r = atomic_load_explicit(&ctx->rp, memory_order_relaxed);
            uint32_t w = atomic_load_explicit(&ctx->wp, memory_order_acquire);
            if (r == w) break; /* empty */

            IpcEventItem *item = &ctx->events[r & (IPC_EVENT_QUEUE_SLOTS - 1)];

            uint32_t seq = atomic_fetch_add_explicit(&ctx->seq, 1,
                                                     memory_order_relaxed);
            bool ok = pipe_write_msg(ctx->conn,
                                     item->msg_type, seq,
                                     item->payload, item->payload_len);
            atomic_store_explicit(&ctx->rp, r + 1, memory_order_release);

            if (!ok) {
                /* Pipe broken — close and wait for reconnect */
                platform_pipe_close(ctx->conn);
                ctx->conn = NULL;
                break;
            }
            sent_any = true;
        }

        if (!sent_any) platform_sleep_ms(1);
    }
}

/* -------------------------------------------------------------------------
 * Init / start / shutdown
 * ---------------------------------------------------------------------- */

bool ipc_init(struct WavRecEngine *eng)
{
    IpcContext *ctx = (IpcContext *)calloc(1, sizeof(IpcContext));
    if (!ctx) return false;

    ctx->eng = eng;
    atomic_init(&ctx->lock,    0);
    atomic_init(&ctx->wp,      0u);
    atomic_init(&ctx->rp,      0u);
    atomic_init(&ctx->seq,     0u);
    atomic_init(&ctx->running, 0);

#ifdef _WIN32
    const char *pipe_name = WAVREC_PIPE_NAME_WIN;
#else
    const char *pipe_name = WAVREC_PIPE_NAME_POSIX;
#endif

    ctx->server = platform_pipe_server_create(pipe_name);
    if (!ctx->server) {
        free(ctx);
        return false;
    }

    engine_set_ipc(eng, ctx);
    return true;
}

void ipc_start(struct WavRecEngine *eng)
{
    IpcContext *ctx = get_ctx(eng);
    if (!ctx || atomic_load_explicit(&ctx->running, memory_order_relaxed)) return;

    atomic_store_explicit(&ctx->running, 1, memory_order_release);

    ctx->rx_thread = platform_thread_create(ipc_rx_thread, eng,
                                            PLATFORM_PRIO_NORMAL, "ipc_rx");
    ctx->tx_thread = platform_thread_create(ipc_tx_thread, eng,
                                            PLATFORM_PRIO_NORMAL, "ipc_tx");
}

void ipc_shutdown(struct WavRecEngine *eng)
{
    IpcContext *ctx = get_ctx(eng);
    if (!ctx) return;

    atomic_store_explicit(&ctx->running, 0, memory_order_release);

    if (ctx->conn)   { platform_pipe_close(ctx->conn);   ctx->conn   = NULL; }
    if (ctx->server) { platform_pipe_close(ctx->server); ctx->server = NULL; }

    if (ctx->rx_thread) {
        platform_thread_join(ctx->rx_thread);
        platform_thread_destroy(ctx->rx_thread);
    }
    if (ctx->tx_thread) {
        platform_thread_join(ctx->tx_thread);
        platform_thread_destroy(ctx->tx_thread);
    }

    free(ctx);
    engine_set_ipc(eng, NULL);
}
