#include "engine.h"
#include "platform/platform.h"
#include "ipc/ipc_protocol.h"
#include "ipc/ipc.h"
#include "ipc/ipc_shm.h"
#include "audio/audio_io.h"
#include "audio/record_engine.h"
#include "audio/playback_engine.h"
#include "audio/mixer.h"
#include "meter/metering.h"
#include "waveform/waveform.h"
#include "disk/disk_writer.h"
#include "transcription/transcription.h"
#include "track/track.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

/* -------------------------------------------------------------------------
 * Engine context
 * ---------------------------------------------------------------------- */

struct WavRecEngine {
    _Atomic int      state;        /* EngState */
    _Atomic uint64_t frame_counter;

    EngSession       session;

    Track            tracks[WAVREC_MAX_CHANNELS];
    int              n_tracks;

    WavRecTimecodeSource tc;

    /* Subsystem contexts */
    AudioIO                *audio_io;
    struct RecordEngine    *record_engine;
    struct PlaybackEngine  *playback_engine;
    struct MeteringEngine  *metering_engine;
    struct WaveformEngine  *waveform_engine;
    struct DiskWriter      *disk_writer;
    struct IpcContext      *ipc_ctx;
    struct IpcShm          *ipc_shm;

    /* Pipe handle for IPC command/event bus */
    PlatformPipe     pipe_server;
    PlatformPipe     pipe_conn;

    /* Shutdown signal */
    _Atomic int      shutdown_requested;
};

/* -------------------------------------------------------------------------
 * State helpers
 * ---------------------------------------------------------------------- */

const char *eng_state_name(EngState s)
{
    switch (s) {
        case ENG_STATE_IDLE:        return "IDLE";
        case ENG_STATE_ARMED:       return "ARMED";
        case ENG_STATE_RECORDING:   return "RECORDING";
        case ENG_STATE_PLAYING:     return "PLAYING";
        case ENG_STATE_RECORD_PLAY: return "RECORD_PLAY";
        case ENG_STATE_STOPPING:    return "STOPPING";
        case ENG_STATE_ERROR:       return "ERROR";
        case ENG_STATE_SHUTDOWN:    return "SHUTDOWN";
        default:                    return "UNKNOWN";
    }
}

static void set_state(WavRecEngine *eng, EngState new_state)
{
    EngState prev = (EngState)atomic_load_explicit(&eng->state,
                                                   memory_order_relaxed);
    if (prev == new_state) return;

    atomic_store_explicit(&eng->state, (int)new_state, memory_order_release);

    /* Emit EVT_STATE_CHANGE */
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"prev_state\":\"%s\",\"new_state\":\"%s\"}",
             eng_state_name(prev), eng_state_name(new_state));
    engine_emit(eng, EVT_STATE_CHANGE, buf);
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

WavRecEngine *engine_create(void)
{
    WavRecEngine *eng = (WavRecEngine *)calloc(1, sizeof(WavRecEngine));
    if (!eng) return NULL;
    atomic_init(&eng->state,         (int)ENG_STATE_IDLE);
    atomic_init(&eng->frame_counter, 0u);
    atomic_init(&eng->shutdown_requested, 0);

    /* Default session */
    eng->session.sample_rate   = 48000;
    eng->session.sample_format = WAVREC_FMT_PCM24;
    eng->session.buffer_frames = 512;
    eng->session.tc_rate       = TC_RATE_25;
    eng->session.n_targets     = 0;

    tc_init(&eng->tc, TC_RATE_25);

    return eng;
}

bool engine_start(WavRecEngine *eng)
{
    if (!ipc_shm_init(eng))        return false;
    if (!audio_io_init(eng))       return false;
    if (!record_engine_init(eng))  return false;
    if (!playback_engine_init(eng))return false;
    if (!mixer_init(eng))          return false;
    if (!metering_init(eng))       return false;
    if (!waveform_init(eng))       return false;
    if (!disk_writer_init(eng))    return false;
    if (!transcription_init(eng))  return false;
    if (!ipc_init(eng))            return false;
    ipc_start(eng);
    return true;
}

void engine_run(WavRecEngine *eng)
{
    /* EVT_READY is sent by the IPC Rx thread immediately after the UI
     * connects — nothing to do here but wait for CMD_SHUTDOWN. */
    while (!atomic_load_explicit(&eng->shutdown_requested, memory_order_acquire))
        platform_sleep_ms(100);
}

void engine_destroy(WavRecEngine *eng)
{
    if (!eng) return;
    transcription_shutdown(eng);
    disk_writer_shutdown(eng);
    waveform_shutdown(eng);
    metering_shutdown(eng);
    mixer_shutdown(eng);
    playback_engine_shutdown(eng);
    record_engine_shutdown(eng);
    audio_io_shutdown(eng);
    ipc_shm_shutdown(eng);
    ipc_shutdown(eng);
    free(eng);
}

/* -------------------------------------------------------------------------
 * State accessors
 * ---------------------------------------------------------------------- */

EngState engine_state(const WavRecEngine *eng)
{
    return (EngState)atomic_load_explicit(&eng->state, memory_order_acquire);
}

bool engine_is_recording(const WavRecEngine *eng)
{
    EngState s = engine_state(eng);
    return s == ENG_STATE_RECORDING || s == ENG_STATE_RECORD_PLAY;
}

bool engine_is_playing(const WavRecEngine *eng)
{
    EngState s = engine_state(eng);
    return s == ENG_STATE_PLAYING || s == ENG_STATE_RECORD_PLAY;
}

uint64_t engine_frame_counter(const WavRecEngine *eng)
{
    return atomic_load_explicit(&eng->frame_counter, memory_order_acquire);
}

const EngSession           *engine_session(const WavRecEngine *eng) { return &eng->session; }
const WavRecTimecodeSource *engine_tc(const WavRecEngine *eng)      { return &eng->tc; }

void engine_advance_frames(WavRecEngine *eng, uint32_t n)
{
    /* Only the audio callback writes this — relaxed load, release store. */
    uint64_t cur = atomic_load_explicit(&eng->frame_counter, memory_order_relaxed);
    atomic_store_explicit(&eng->frame_counter, cur + n, memory_order_release);
}

const struct Track *engine_get_track(const WavRecEngine *eng, int idx)
{
    if (idx < 0 || idx >= eng->n_tracks) return NULL;
    return &eng->tracks[idx];
}

int engine_n_tracks(const WavRecEngine *eng)                  { return eng->n_tracks; }
uint32_t engine_sample_rate(const WavRecEngine *eng)          { return eng->session.sample_rate; }
uint32_t engine_buffer_frames(const WavRecEngine *eng)        { return eng->session.buffer_frames; }
WavSampleFormat engine_sample_format(const WavRecEngine *eng) { return eng->session.sample_format; }
struct AudioIO *engine_audio_io(WavRecEngine *eng)         { return eng->audio_io; }
void            engine_set_audio_io(WavRecEngine *eng,
                                    struct AudioIO *aio)   { eng->audio_io = aio; }

struct PlaybackEngine *engine_playback(WavRecEngine *eng)               { return eng->playback_engine; }
void                   engine_set_playback(WavRecEngine *eng,
                                           struct PlaybackEngine *pe)   { eng->playback_engine = pe; }

struct RecordEngine  *engine_record_engine(WavRecEngine *eng)          { return eng->record_engine; }
void                  engine_set_record_engine(WavRecEngine *eng,
                                               struct RecordEngine *re){ eng->record_engine = re; }

struct MeteringEngine *engine_metering(WavRecEngine *eng)              { return eng->metering_engine; }
void                   engine_set_metering(WavRecEngine *eng,
                                           struct MeteringEngine *me)  { eng->metering_engine = me; }

struct WaveformEngine *engine_waveform(WavRecEngine *eng)               { return eng->waveform_engine; }
void                   engine_set_waveform(WavRecEngine *eng,
                                           struct WaveformEngine *we)   { eng->waveform_engine = we; }

struct DiskWriter *engine_disk_writer(WavRecEngine *eng)               { return eng->disk_writer; }
void               engine_set_disk_writer(WavRecEngine *eng,
                                          struct DiskWriter *dw)        { eng->disk_writer = dw; }

struct IpcContext *engine_ipc(WavRecEngine *eng)                       { return eng->ipc_ctx; }
void               engine_set_ipc(WavRecEngine *eng,
                                  struct IpcContext *ctx)               { eng->ipc_ctx = ctx; }

struct IpcShm *engine_ipc_shm(WavRecEngine *eng)                      { return eng->ipc_shm; }
void           engine_set_ipc_shm(WavRecEngine *eng,
                                  struct IpcShm *shm)                  { eng->ipc_shm = shm; }

/* -------------------------------------------------------------------------
 * Event emission (called by any thread)
 * ---------------------------------------------------------------------- */

void engine_emit(WavRecEngine *eng, WavRecMsgType type, const char *payload_json)
{
    ipc_send_event(eng, payload_json, (unsigned char)type);
}

/* -------------------------------------------------------------------------
 * JSON command parsers
 * ---------------------------------------------------------------------- */

static void parse_session_init(WavRecEngine *eng, const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (!root) return;

    cJSON *j;

    j = cJSON_GetObjectItem(root, "sample_rate");
    if (cJSON_IsNumber(j))
        eng->session.sample_rate = (uint32_t)j->valuedouble;

    j = cJSON_GetObjectItem(root, "buffer_frames");
    if (cJSON_IsNumber(j))
        eng->session.buffer_frames = (uint32_t)j->valuedouble;

    /* sample_format — prefer string field, fall back to bit_depth + is_float */
    j = cJSON_GetObjectItem(root, "sample_format");
    if (cJSON_IsString(j)) {
        const char *s = j->valuestring;
        if      (strcmp(s, "pcm16")   == 0) eng->session.sample_format = WAVREC_FMT_PCM16;
        else if (strcmp(s, "pcm24")   == 0) eng->session.sample_format = WAVREC_FMT_PCM24;
        else if (strcmp(s, "pcm32")   == 0) eng->session.sample_format = WAVREC_FMT_PCM32;
        else if (strcmp(s, "float32") == 0) eng->session.sample_format = WAVREC_FMT_FLOAT32;
    } else {
        cJSON *bd = cJSON_GetObjectItem(root, "bit_depth");
        cJSON *fl = cJSON_GetObjectItem(root, "is_float");
        if (cJSON_IsNumber(bd)) {
            int depth    = (int)bd->valuedouble;
            int is_float = cJSON_IsTrue(fl);
            if      (is_float && depth == 32) eng->session.sample_format = WAVREC_FMT_FLOAT32;
            else if (depth == 16)             eng->session.sample_format = WAVREC_FMT_PCM16;
            else if (depth == 32)             eng->session.sample_format = WAVREC_FMT_PCM32;
            else                              eng->session.sample_format = WAVREC_FMT_PCM24;
        }
    }

    j = cJSON_GetObjectItem(root, "device");
    if (cJSON_IsString(j)) strncpy(eng->session.device, j->valuestring, 127);

    j = cJSON_GetObjectItem(root, "scene");
    if (cJSON_IsString(j)) strncpy(eng->session.scene, j->valuestring, 31);

    j = cJSON_GetObjectItem(root, "take");
    if (cJSON_IsString(j)) strncpy(eng->session.take,  j->valuestring, 7);

    j = cJSON_GetObjectItem(root, "tape");
    if (cJSON_IsString(j)) strncpy(eng->session.tape,  j->valuestring, 15);

    j = cJSON_GetObjectItem(root, "timecode_rate");
    if (cJSON_IsString(j)) {
        const char *s = j->valuestring;
        if      (strcmp(s, "23.976") == 0) eng->session.tc_rate = TC_RATE_23976;
        else if (strcmp(s, "24")     == 0) eng->session.tc_rate = TC_RATE_24;
        else if (strcmp(s, "25")     == 0) eng->session.tc_rate = TC_RATE_25;
        else if (strcmp(s, "29.97")  == 0) eng->session.tc_rate = TC_RATE_2997_NDF;
        else if (strcmp(s, "30")     == 0) eng->session.tc_rate = TC_RATE_30_NDF;
    }

    j = cJSON_GetObjectItem(root, "timecode_df");
    if (cJSON_IsBool(j)) {
        eng->session.tc_drop_frame = cJSON_IsTrue(j);
        if (eng->session.tc_drop_frame) {
            if (eng->session.tc_rate == TC_RATE_2997_NDF)
                eng->session.tc_rate = TC_RATE_2997_DF;
            else if (eng->session.tc_rate == TC_RATE_30_NDF)
                eng->session.tc_rate = TC_RATE_30_DF;
        }
    }
    tc_init(&eng->tc, eng->session.tc_rate);

    j = cJSON_GetObjectItem(root, "record_targets");
    if (cJSON_IsArray(j)) {
        eng->session.n_targets = 0;
        cJSON *el;
        cJSON_ArrayForEach(el, j) {
            if (!cJSON_IsString(el)) continue;
            if (eng->session.n_targets >= WAVREC_MAX_TARGETS) break;
            strncpy(eng->session.record_targets[eng->session.n_targets],
                    el->valuestring, WAVREC_MAX_TARGET_PATH - 1);
            eng->session.n_targets++;
        }
    }

    cJSON_Delete(root);
}

static void parse_track_config(WavRecEngine *eng, const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (!root) return;

    cJSON *tracks = cJSON_GetObjectItem(root, "tracks");
    if (!cJSON_IsArray(tracks)) { cJSON_Delete(root); return; }

    cJSON *t;
    cJSON_ArrayForEach(t, tracks) {
        cJSON *jid = cJSON_GetObjectItem(t, "id");
        if (!cJSON_IsNumber(jid)) continue;
        int id = (int)jid->valuedouble;
        if (id < 0 || id >= WAVREC_MAX_CHANNELS) continue;

        if (id >= eng->n_tracks) eng->n_tracks = id + 1;

        Track *tr = &eng->tracks[id];
        track_init(tr, (uint8_t)id);

        cJSON *j;
        j = cJSON_GetObjectItem(t, "label");
        if (cJSON_IsString(j)) track_set_label(tr, j->valuestring);

        j = cJSON_GetObjectItem(t, "hw_input");
        if (cJSON_IsNumber(j)) tr->hw_input = (uint8_t)(int)j->valuedouble;

        j = cJSON_GetObjectItem(t, "gain_db");
        if (cJSON_IsNumber(j)) tr->gain_db = (float)j->valuedouble;

        j = cJSON_GetObjectItem(t, "armed");
        if (cJSON_IsBool(j)) {
            EngState s = engine_state(eng);
            if (s == ENG_STATE_RECORDING || s == ENG_STATE_RECORD_PLAY)
                tr->pre_armed = (int8_t)(cJSON_IsTrue(j) ? 1 : 0);
            else
                tr->armed = cJSON_IsTrue(j);
        }

        j = cJSON_GetObjectItem(t, "monitor");
        if (cJSON_IsBool(j)) tr->monitor = cJSON_IsTrue(j);
    }

    cJSON_Delete(root);
}

/* Extract a uint64_t "position_frames" field from a JSON payload. */
static uint64_t parse_position(const char *payload, uint16_t len)
{
    if (!payload || len == 0) return 0;
    cJSON *root = cJSON_Parse(payload);
    if (!root) return 0;
    cJSON *j = cJSON_GetObjectItem(root, "position_frames");
    uint64_t pos = cJSON_IsNumber(j) ? (uint64_t)j->valuedouble : 0;
    cJSON_Delete(root);
    return pos;
}

/* Apply any pending pre_armed values to armed, then clear them. */
static void apply_pre_armed(WavRecEngine *eng)
{
    for (int i = 0; i < eng->n_tracks; i++) {
        if (eng->tracks[i].pre_armed >= 0) {
            eng->tracks[i].armed     = (eng->tracks[i].pre_armed == 1);
            eng->tracks[i].pre_armed = -1;
        }
    }
}

/* Arm/disarm the tracks listed in {"tracks":[id,id,...]}; if the array
 * is absent or empty, arm/disarm all configured tracks.
 * Arming also enables monitor so the operator hears the input; disarming
 * leaves monitor untouched (input may still be auditioned). */
static void set_track_armed(WavRecEngine *eng,
                            const char *payload, uint16_t len, int armed)
{
    int set_any = 0;
    if (payload && len > 0) {
        cJSON *root = cJSON_Parse(payload);
        cJSON *arr  = root ? cJSON_GetObjectItem(root, "tracks") : NULL;
        if (cJSON_IsArray(arr)) {
            cJSON *el;
            cJSON_ArrayForEach(el, arr) {
                int id = (int)el->valuedouble;
                if (id >= 0 && id < eng->n_tracks) {
                    eng->tracks[id].armed = (armed != 0);
                    if (armed) eng->tracks[id].monitor = true;
                    set_any = 1;
                }
            }
        }
        if (root) cJSON_Delete(root);
    }
    if (!set_any) {
        for (int i = 0; i < eng->n_tracks; i++) {
            eng->tracks[i].armed = (armed != 0);
            if (armed) eng->tracks[i].monitor = true;
        }
    }
}

/* Stage a pending arm change on the tracks listed in {"tracks":[id,...]}.
 * When pre-arming (armed=1), also enable monitor immediately so the operator
 * can hear the input during the punch-in lead. */
static void set_track_pre_armed(WavRecEngine *eng,
                                const char *payload, uint16_t len, int armed)
{
    int set_any = 0;
    if (payload && len > 0) {
        cJSON *root = cJSON_Parse(payload);
        cJSON *arr  = root ? cJSON_GetObjectItem(root, "tracks") : NULL;
        if (cJSON_IsArray(arr)) {
            cJSON *el;
            cJSON_ArrayForEach(el, arr) {
                int id = (int)el->valuedouble;
                if (id >= 0 && id < eng->n_tracks) {
                    eng->tracks[id].pre_armed = (int8_t)(armed ? 1 : 0);
                    if (armed) eng->tracks[id].monitor = true;
                    set_any = 1;
                }
            }
        }
        if (root) cJSON_Delete(root);
    }
    if (!set_any) {
        for (int i = 0; i < eng->n_tracks; i++) {
            eng->tracks[i].pre_armed = (int8_t)(armed ? 1 : 0);
            if (armed) eng->tracks[i].monitor = true;
        }
    }
}

/* -------------------------------------------------------------------------
 * Command dispatch (called by IPC Rx thread)
 * ---------------------------------------------------------------------- */

void engine_dispatch(WavRecEngine *eng, WavRecMsgType type,
                     uint32_t sequence,
                     const char *payload, uint16_t payload_len)
{
    (void)sequence;

    EngState cur = engine_state(eng);
    fprintf(stderr, "[dispatch] type=0x%02x state=%s payload_len=%u\n",
            (unsigned)type, eng_state_name(cur), (unsigned)payload_len);
    if (type == CMD_SESSION_INIT && payload && payload_len > 0)
        fprintf(stderr, "[dispatch] session_init payload: %.200s\n", payload);
    fflush(stderr);

    switch (type) {

    case CMD_PING:
        engine_emit(eng, EVT_PONG, "{}");
        break;

    case CMD_SESSION_INIT:
        if (payload && payload_len > 0)
            parse_session_init(eng, payload);
        ipc_shm_update_sample_rate(eng);
        /* (Re)open device so monitoring is live immediately; safe only when idle. */
        if (cur == ENG_STATE_IDLE || cur == ENG_STATE_ARMED) {
            audio_io_close_device(eng);
            audio_io_open_device(eng);
            /* Ensure signal distribution and metering run continuously. */
            record_engine_start(eng);
            metering_start(eng);
        }
        engine_emit(eng, EVT_SESSION_INFO, payload ? payload : "{}");
        break;

    case CMD_TRACK_CONFIG:
        if (payload && payload_len > 0)
            parse_track_config(eng, payload);
        break;

    case CMD_ARM:
        if (cur == ENG_STATE_RECORDING || cur == ENG_STATE_RECORD_PLAY) {
            set_track_pre_armed(eng, payload, payload_len, 1);
        } else {
            set_track_armed(eng, payload, payload_len, 1);
            if (cur == ENG_STATE_IDLE)
                set_state(eng, ENG_STATE_ARMED);
        }
        break;

    case CMD_DISARM:
        if (cur == ENG_STATE_RECORDING || cur == ENG_STATE_RECORD_PLAY) {
            set_track_pre_armed(eng, payload, payload_len, 0);
        } else {
            set_track_armed(eng, payload, payload_len, 0);
            if (cur == ENG_STATE_ARMED) {
                bool any_armed = false;
                for (int i = 0; i < eng->n_tracks; i++)
                    if (eng->tracks[i].armed) { any_armed = true; break; }
                if (!any_armed)
                    set_state(eng, ENG_STATE_IDLE);
            }
        }
        break;

    case CMD_RECORD:
        if (cur == ENG_STATE_RECORDING || cur == ENG_STATE_RECORD_PLAY) {
            /* Punch: stop current take, apply pre_armed, start next take. */
            set_state(eng, ENG_STATE_STOPPING);
            record_engine_stop(eng);
            disk_writer_close(eng);
            apply_pre_armed(eng);
            tc_latch_free_run(&eng->tc,
                              engine_frame_counter(eng),
                              eng->session.sample_rate);
            disk_writer_open(eng);
            record_engine_start(eng);
            metering_start(eng);
            waveform_start(eng);
            set_state(eng, (cur == ENG_STATE_RECORD_PLAY)
                           ? ENG_STATE_RECORD_PLAY : ENG_STATE_RECORDING);
        } else if (cur == ENG_STATE_ARMED || cur == ENG_STATE_IDLE) {
            audio_io_open_device(eng);  /* no-op if already open */
            tc_latch_free_run(&eng->tc,
                              engine_frame_counter(eng),
                              eng->session.sample_rate);
            disk_writer_open(eng);
            record_engine_start(eng);   /* no-op if already running */
            metering_start(eng);        /* no-op if already running */
            waveform_start(eng);
            set_state(eng, ENG_STATE_RECORDING);
        } else if (cur == ENG_STATE_PLAYING) {
            tc_latch_free_run(&eng->tc,
                              engine_frame_counter(eng),
                              eng->session.sample_rate);
            disk_writer_open(eng);
            set_state(eng, ENG_STATE_RECORD_PLAY);
        }
        break;

    case CMD_PLAY: {
        uint64_t pos = parse_position(payload, payload_len);
        if (cur == ENG_STATE_IDLE || cur == ENG_STATE_ARMED) {
            audio_io_open_device(eng);  /* no-op if already open */
            playback_engine_start(eng, pos);
            set_state(eng, ENG_STATE_PLAYING);
        } else if (cur == ENG_STATE_RECORDING) {
            playback_engine_start(eng, pos);
            set_state(eng, ENG_STATE_RECORD_PLAY);
        }
        break;
    }

    case CMD_LOCATE:
        playback_engine_locate(eng, parse_position(payload, payload_len));
        break;

    case CMD_STOP:
        if (cur == ENG_STATE_RECORDING || cur == ENG_STATE_RECORD_PLAY) {
            set_state(eng, ENG_STATE_STOPPING);
            record_engine_stop(eng);
            playback_engine_stop(eng);
            disk_writer_close(eng);
            /* Restart signal distribution so monitoring/metering stay live. */
            record_engine_start(eng);
            metering_start(eng);
            set_state(eng, ENG_STATE_IDLE);
        } else if (cur == ENG_STATE_PLAYING) {
            playback_engine_stop(eng);
            set_state(eng, ENG_STATE_IDLE);
        }
        break;

    case CMD_METER_RESET:
        metering_reset_clips(eng);
        break;

    case CMD_TRANSCRIPTION_CONFIG:
        /* TODO: parse TranscriptionConfig from payload */
        break;

    case CMD_SHUTDOWN:
        set_state(eng, ENG_STATE_SHUTDOWN);
        atomic_store_explicit(&eng->shutdown_requested, 1, memory_order_release);
        break;

    default:
        break;
    }
}
