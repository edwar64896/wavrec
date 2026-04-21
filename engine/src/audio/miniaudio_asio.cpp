/*
 * miniaudio_asio.cpp — ASIO custom backend for miniaudio (Windows only).
 *
 * Architecture
 * ============
 * miniaudio's custom backend API lets us plug in any audio system as if it
 * were a first-class backend.  We implement:
 *
 *   onContextInit             – register all other callbacks
 *   onContextUninit           – COM cleanup
 *   onContextEnumerateDevices – read ASIO drivers from the registry
 *   onContextGetDeviceInfo    – load the driver briefly, query channels/rates
 *   onDeviceInit              – load driver, create ASIO buffers, store state
 *   onDeviceUninit            – release driver
 *   onDeviceStart             – driver->start()
 *   onDeviceStop              – driver->stop()
 *
 * The ASIO bufferSwitch callback owns the audio thread.  It converts ASIO's
 * non-interleaved native buffers to interleaved float32, then calls
 *   ma_device_handle_backend_data_callback(pDevice, pOutput, pInput, frames)
 * which invokes the user's dataCallback (audio_io.c::audio_callback) exactly
 * as WASAPI would, then converts the filled output buffer back to ASIO format.
 *
 * ASIO is inherently a singleton — only one driver can be loaded at a time —
 * so all driver state is kept in a single static struct.
 */

#ifdef WAVREC_HAVE_ASIO

#include <windows.h>
#include <objbase.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>

/* ASIO SDK – common/ must be on the include path */
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

/* miniaudio — implementation already in miniaudio_impl.c, just the header here */
#include "miniaudio.h"

/* Our own header */
#include "miniaudio_asio.h"

extern "C" {
#include "../engine.h"
#include "../ipc/ipc_protocol.h"
}

/* ============================================================================
 * Global singleton driver state
 * ========================================================================= */

#define ASIO_MAX_CHANNELS 128
#define ASIO_MAX_FRAMES   16384   /* pre-allocated scratch ceiling */

struct AsioState {
    IASIO          *driver;
    ASIOBufferInfo  bufInfos[ASIO_MAX_CHANNELS * 2]; /* in + out */
    ASIOChannelInfo chanInfo[ASIO_MAX_CHANNELS * 2];
    long            nIn;          /* actual input channels */
    long            nOut;         /* output channels allocated (≤ 2) */
    long            bufSize;      /* frames per buffer */
    bool            running;

    ma_device      *pDevice;      /* set in onDeviceInit, used in bufferSwitch */

    /* Signaled by onDeviceDataLoopWakeup so the data loop unblocks on stop */
    HANDLE          stop_event;

    /* Pre-allocated scratch to avoid hot-path allocs */
    float           capFloat[ASIO_MAX_CHANNELS * ASIO_MAX_FRAMES]; /* interleaved */
    float           pbFloat[2 * ASIO_MAX_FRAMES];                  /* stereo out */
};

static AsioState g_asio = {};

/* ============================================================================
 * Dedicated ASIO STA thread
 *
 * All ASIO driver calls that require COM STA message dispatch are serialised
 * through this thread.  It runs a proper GetMessage loop so COM's internal
 * STA machinery can dispatch messages while driver methods block (e.g.
 * VoiceMeeter VASIO uses cross-process COM during getChannels).
 * ========================================================================= */

#define WM_ASIO_WORK (WM_USER + 42)

struct AsioSta {
    DWORD   tid;
    HANDLE  thread;
};

struct AsioWork {
    void     (*fn)(void *);
    void      *arg;
    HANDLE     done;
};

static AsioSta g_asio_sta = {};

static DWORD WINAPI asio_sta_proc(LPVOID)
{
    CoInitialize(nullptr); /* STA */

    /* Force the thread's message queue to exist before signalling readiness */
    MSG probe;
    PeekMessageA(&probe, NULL, WM_USER, WM_USER, PM_NOREMOVE);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_QUIT) break;
        if (msg.message == WM_ASIO_WORK) {
            AsioWork *w = reinterpret_cast<AsioWork *>(msg.lParam);
            if (w->fn) w->fn(w->arg);
            SetEvent(w->done);
        } else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    CoUninitialize();
    return 0;
}

/* Run fn(arg) on the dedicated ASIO STA thread and block until it returns. */
static void asio_sta_run(void (*fn)(void *), void *arg)
{
    AsioWork w;
    w.fn   = fn;
    w.arg  = arg;
    w.done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    PostThreadMessageA(g_asio_sta.tid, WM_ASIO_WORK, 0,
                       reinterpret_cast<LPARAM>(&w));
    WaitForSingleObject(w.done, 30000);
    CloseHandle(w.done);
}

static void asio_sta_start(void)
{
    if (g_asio_sta.thread) return;

    /* Create thread first, then retrieve its TID */
    DWORD tid = 0;
    HANDLE h = CreateThread(nullptr, 0, asio_sta_proc, nullptr,
                            0, &tid);
    g_asio_sta.thread = h;
    g_asio_sta.tid    = tid;

    /* Wait until the thread's message queue exists (first PeekMessage in
     * asio_sta_proc forces queue creation before it enters GetMessage). */
    Sleep(50);
}

static void asio_sta_stop(void)
{
    if (!g_asio_sta.thread) return;
    PostThreadMessageA(g_asio_sta.tid, WM_QUIT, 0, 0);
    WaitForSingleObject(g_asio_sta.thread, 5000);
    CloseHandle(g_asio_sta.thread);
    g_asio_sta = {};
}

/* ============================================================================
 * SEH wrappers — isolate crashes in third-party ASIO driver DLLs.
 * Each wrapper returns 1 on success, 0 on driver failure, -1 on crash.
 * These must be plain functions with no C++ local objects (SEH constraint).
 * ========================================================================= */

static int safe_asio_init(IASIO *drv)
{
    __try { return drv->init(GetDesktopWindow()) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

/* Encapsulates all driver calls after init() in one SEH block.
 * Only POD locals — required by MSVC for __try/__except. */
struct AsioSetupIn  { IASIO *drv; ma_uint32 sr; long reqBuf; };
struct AsioSetupOut {
    long nIn, nOut, bufSize;
    double actualSr;
    ASIOBufferInfo  bufInfos[ASIO_MAX_CHANNELS * 2];
    ASIOChannelInfo chanInfo[ASIO_MAX_CHANNELS * 2];
    ASIOCallbacks  *callbacks;
    int status; /* 0=ok, -1=crash, -2=driver-error */
};

static void safe_asio_setup(const AsioSetupIn *in, AsioSetupOut *out)
{
    out->status = -1; /* assume crash */
    __try {
        IASIO *drv = in->drv;

        /* VoiceMeeter-style virtual ASIO drivers finish initialization
         * asynchronously — they post a message after init() returns.
         * Pump the message queue and retry until channels are non-zero,
         * or until 3 seconds have elapsed. */
        long nIn = 0, nOut = 0;
        for (int attempt = 0; attempt < 60; attempt++) {
            MSG m;
            while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m); DispatchMessage(&m);
            }
            drv->getChannels(&nIn, &nOut);
            fflush(stderr); fprintf(stderr, "[setup] getChannels attempt %d: nIn=%ld nOut=%ld\n",
                                    attempt, nIn, nOut);
            if (nIn > 0 || nOut > 0) break;
            Sleep(50); /* 60 × 50ms = 3 seconds max */
        }
        if (nIn == 0 && nOut == 0) { out->status = -2; return; } /* driver not ready */

        out->nIn  = nIn;
        out->nOut = (nOut < 2) ? nOut : 2;

        ma_uint32 sr = in->sr ? in->sr : 48000;
        fflush(stderr); fprintf(stderr, "[setup] canSampleRate %u\n", sr);
        if (drv->canSampleRate((ASIOSampleRate)sr) == ASE_OK)
            drv->setSampleRate((ASIOSampleRate)sr);
        ASIOSampleRate actualSr = 48000.0;
        fflush(stderr); fprintf(stderr, "[setup] getSampleRate\n");
        drv->getSampleRate(&actualSr);
        out->actualSr = (double)actualSr;
        fflush(stderr); fprintf(stderr, "[setup] actualSr=%.0f\n", (double)actualSr);

        long mn, mx, pref, gran;
        fflush(stderr); fprintf(stderr, "[setup] getBufferSize\n");
        drv->getBufferSize(&mn, &mx, &pref, &gran);
        long req = in->reqBuf ? in->reqBuf : pref;
        if (req < mn) req = mn;
        if (req > mx) req = mx;
        if (gran > 1 && req % gran) req = ((req / gran) + 1) * gran;
        if (req > mx) req = mx;
        out->bufSize = req;
        fflush(stderr); fprintf(stderr, "[setup] bufSize=%ld total=%ld\n", req, nIn + out->nOut);

        long total = nIn + out->nOut;
        if (total == 0 || total > ASIO_MAX_CHANNELS * 2) {
            out->status = -2; return;
        }

        memset(out->bufInfos, 0, sizeof(out->bufInfos));
        for (long i = 0; i < nIn;        i++) { out->bufInfos[i].isInput = ASIOTrue;  out->bufInfos[i].channelNum = i; }
        for (long i = 0; i < out->nOut;  i++) { out->bufInfos[nIn+i].isInput = ASIOFalse; out->bufInfos[nIn+i].channelNum = i; }

        fflush(stderr); fprintf(stderr, "[setup] createBuffers total=%ld req=%ld\n", total, req);
        if (drv->createBuffers(out->bufInfos, total, req, out->callbacks) != ASE_OK) {
            fflush(stderr); fprintf(stderr, "[setup] createBuffers failed\n");
            out->status = -2; return;
        }
        fflush(stderr); fprintf(stderr, "[setup] createBuffers OK\n");

        for (long i = 0; i < total; i++) {
            out->chanInfo[i].channel = out->bufInfos[i].channelNum;
            out->chanInfo[i].isInput = out->bufInfos[i].isInput;
            drv->getChannelInfo(&out->chanInfo[i]);
        }

        out->status = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->status = -1;
    }
}

/* ============================================================================
 * Sample format conversion helpers
 * ========================================================================= */

static inline float asio_to_f32(const void *buf, ASIOSampleType t, long frame)
{
    const auto *p = static_cast<const unsigned char *>(buf);
    switch (t) {
    case ASIOSTInt16LSB: {
        short s; memcpy(&s, p + frame*2, 2);
        return s * (1.0f / 32768.0f);
    }
    case ASIOSTInt24LSB: {
        const unsigned char *b = p + frame*3;
        int s = (int)b[0] | ((int)b[1]<<8) | ((int)b[2]<<16);
        if (s & 0x800000) s |= 0xFF000000;
        return s * (1.0f / 8388608.0f);
    }
    case ASIOSTInt32LSB: {
        int s; memcpy(&s, p + frame*4, 4);
        return s * (1.0f / 2147483648.0f);
    }
    case ASIOSTFloat32LSB: {
        float v; memcpy(&v, p + frame*4, 4);
        return v;
    }
    case ASIOSTFloat64LSB: {
        double v; memcpy(&v, p + frame*8, 8);
        return (float)v;
    }
    default: return 0.0f;
    }
}

static inline void f32_to_asio(void *buf, ASIOSampleType t, long frame, float val)
{
    auto *p = static_cast<unsigned char *>(buf);
    if (val >  1.0f) val =  1.0f;
    if (val < -1.0f) val = -1.0f;
    switch (t) {
    case ASIOSTInt16LSB: {
        short s = (short)(val * 32767.0f);
        memcpy(p + frame*2, &s, 2);
        break;
    }
    case ASIOSTInt24LSB: {
        int s = (int)(val * 8388607.0f);
        p[frame*3+0] = (unsigned char)(s & 0xFF);
        p[frame*3+1] = (unsigned char)((s>>8) & 0xFF);
        p[frame*3+2] = (unsigned char)((s>>16) & 0xFF);
        break;
    }
    case ASIOSTInt32LSB: {
        int s = (int)(val * 2147483647.0f);
        memcpy(p + frame*4, &s, 4);
        break;
    }
    case ASIOSTFloat32LSB: {
        memcpy(p + frame*4, &val, 4);
        break;
    }
    case ASIOSTFloat64LSB: {
        double v = val;
        memcpy(p + frame*8, &v, 8);
        break;
    }
    default: break;
    }
}

/* ============================================================================
 * ASIO buffer switch — the real-time audio processing entry point
 * ========================================================================= */

static void do_buffer_switch(long index)
{
    if (!g_asio.running || !g_asio.pDevice) return;

    static long switch_count = 0;
    if ((switch_count++ % 100) == 0)
        fprintf(stderr, "[asio/cb] buffer_switch #%ld state=%d onData=%p\n",
                switch_count, (int)ma_device_get_state(g_asio.pDevice),
                (void *)g_asio.pDevice->onData);

    long fc  = g_asio.bufSize;
    long nIn = g_asio.nIn;
    long nOut= g_asio.nOut;

    /* Build interleaved float32 capture buffer from ASIO's per-channel buffers */
    for (long ch = 0; ch < nIn && ch < ASIO_MAX_CHANNELS; ch++) {
        const void *ibuf = g_asio.bufInfos[ch].buffers[index];
        ASIOSampleType t = g_asio.chanInfo[ch].type;
        if (ibuf) {
            for (long f = 0; f < fc; f++)
                g_asio.capFloat[f * nIn + ch] = asio_to_f32(ibuf, t, f);
        } else {
            for (long f = 0; f < fc; f++)
                g_asio.capFloat[f * nIn + ch] = 0.0f;
        }
    }

    /* Zero the playback buffer — the callback will fill it */
    memset(g_asio.pbFloat, 0, nOut * fc * sizeof(float));

    /* Call the user's data callback directly.  Our buffers are already in
     * the exact format the user requested (f32 interleaved, matching channel
     * counts), so miniaudio's ma_device_handle_backend_data_callback would
     * only add state-check overhead and has been observed to drop callbacks
     * during state transitions. */
    if (g_asio.pDevice->onData)
        g_asio.pDevice->onData(g_asio.pDevice, g_asio.pbFloat, g_asio.capFloat,
                               (ma_uint32)fc);

    /* Write interleaved float32 playback data back to ASIO per-channel buffers */
    for (long ch = 0; ch < nOut; ch++) {
        void *obuf = g_asio.bufInfos[nIn + ch].buffers[index];
        ASIOSampleType t = g_asio.chanInfo[nIn + ch].type;
        if (obuf) {
            for (long f = 0; f < fc; f++)
                f32_to_asio(obuf, t, f, g_asio.pbFloat[f * nOut + ch]);
        }
    }

    /* Tell the driver output is ready (can reduce output latency) */
    if (g_asio.driver) g_asio.driver->outputReady();
}

/* ASIO callback functions — must be free static functions (no user-data ptr) */
static void asio_buffer_switch(long index, ASIOBool) { do_buffer_switch(index); }
static ASIOTime *asio_buffer_switch_time_info(ASIOTime *p, long index, ASIOBool)
    { do_buffer_switch(index); return p; }
static void asio_sample_rate_changed(ASIOSampleRate) {}
static long asio_message(long selector, long value, void *, double *)
{
    if (selector == kAsioSelectorSupported)
        return (value == kAsioEngineVersion || value == kAsioResetRequest) ? 1L : 0L;
    if (selector == kAsioEngineVersion) return 2L;
    if (selector == kAsioResetRequest)  return 1L;
    return 0L;
}

static ASIOCallbacks g_callbacks = {
    asio_buffer_switch,
    asio_sample_rate_changed,
    asio_message,
    asio_buffer_switch_time_info,
};

/* ============================================================================
 * Registry helpers
 * ========================================================================= */

struct AsioDriverEntry { char name[128]; char clsid[64]; };

static int reg_enumerate_drivers(AsioDriverEntry *out, int maxN)
{
    HKEY hRoot;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &hRoot) != ERROR_SUCCESS)
        return 0;
    int n = 0;
    for (DWORD i = 0; n < maxN; i++) {
        char name[128]; DWORD nLen = sizeof(name);
        if (RegEnumKeyExA(hRoot, i, name, &nLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        HKEY hDrv;
        if (RegOpenKeyExA(hRoot, name, 0, KEY_READ, &hDrv) == ERROR_SUCCESS) {
            char clsid[64] = {}; DWORD type, sz = sizeof(clsid);
            RegQueryValueExA(hDrv, "CLSID", NULL, &type, (LPBYTE)clsid, &sz);
            RegCloseKey(hDrv);
            if (clsid[0]) {
                strncpy(out[n].name,  name,  127);
                strncpy(out[n].clsid, clsid, 63);
                n++;
            }
        }
    }
    RegCloseKey(hRoot);
    return n;
}

static IASIO *reg_load_driver(const char *name, HRESULT *hr_out = nullptr)
{
    HKEY hRoot, hDrv;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &hRoot) != ERROR_SUCCESS)
        return nullptr;
    char clsid_str[64] = {};
    if (RegOpenKeyExA(hRoot, name, 0, KEY_READ, &hDrv) == ERROR_SUCCESS) {
        DWORD type, sz = sizeof(clsid_str);
        RegQueryValueExA(hDrv, "CLSID", NULL, &type, (LPBYTE)clsid_str, &sz);
        RegCloseKey(hDrv);
    }
    RegCloseKey(hRoot);
    if (!clsid_str[0]) return nullptr;

    wchar_t wclsid[64];
    MultiByteToWideChar(CP_ACP, 0, clsid_str, -1, wclsid, 64);
    CLSID clsid;
    if (FAILED(CLSIDFromString(wclsid, &clsid))) return nullptr;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    /* Steinberg ASIO SDK convention: CLSID is also used as the IID.
     * This gives the correct IASIO vtable for drivers that implement
     * IUnknown and IASIO as distinct interfaces (e.g. VB-Matrix).
     * Fall back to IID_IUnknown for drivers that only expose IUnknown
     * (IASIO inherits single-hierarchy — same vtable). */
    IASIO  *drv = nullptr;
    HRESULT hr  = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                   clsid, (void **)&drv);
    fflush(stderr); fprintf(stderr, "[asio/sta] CoCreateInstance CLSID-as-IID hr=0x%08lX drv=%p\n",
                            (unsigned long)(DWORD)hr, (void *)drv);
    if (FAILED(hr) || !drv) {
        hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IUnknown, (void **)&drv);
        fflush(stderr); fprintf(stderr, "[asio/sta] CoCreateInstance IID_IUnknown hr=0x%08lX drv=%p\n",
                                (unsigned long)(DWORD)hr, (void *)drv);
    }
    if (hr_out) *hr_out = hr;
    return SUCCEEDED(hr) ? drv : nullptr;
}

/* ============================================================================
 * miniaudio backend callbacks
 * ========================================================================= */

static ma_result asio_on_context_uninit(ma_context *)
{
    asio_sta_stop();
    CoUninitialize();
    return MA_SUCCESS;
}

static ma_result asio_on_enumerate_devices(ma_context *pCtx,
                                            ma_enum_devices_callback_proc cb,
                                            void *pUser)
{
    AsioDriverEntry drivers[32];
    int n = reg_enumerate_drivers(drivers, 32);
    for (int i = 0; i < n; i++) {
        /* Expose each ASIO driver as both capture and playback */
        ma_device_info info;
        memset(&info, 0, sizeof(info));
        strncpy(info.name, drivers[i].name, sizeof(info.name) - 1);
        info.isDefault = (i == 0) ? MA_TRUE : MA_FALSE;
        /* Generate a stable device ID from the driver name */
        memset(&info.id, 0, sizeof(info.id));
        strncpy(info.id.custom.s, drivers[i].name, sizeof(info.id.custom.s) - 1);

        if (cb(pCtx, ma_device_type_capture,  &info, pUser) == MA_FALSE) return MA_SUCCESS;
        if (cb(pCtx, ma_device_type_playback, &info, pUser) == MA_FALSE) return MA_SUCCESS;
    }
    return MA_SUCCESS;
}

static ma_result asio_on_get_device_info(ma_context *, ma_device_type type,
                                          const ma_device_id *pID,
                                          ma_device_info *pInfo)
{
    const char *name = pID->custom.s;
    memset(pInfo, 0, sizeof(*pInfo));
    strncpy(pInfo->name, name, sizeof(pInfo->name) - 1);
    pInfo->id = *pID;

    /* If we already have this driver open, return cached info.  Loading a
     * second instance and calling init() while the first is live resets the
     * driver's hardware state and causes start() to fail. */
    if (g_asio.driver && g_asio.pDevice) {
        long ch = (type == ma_device_type_capture) ? g_asio.nIn : g_asio.nOut;
        if (ch > 0) {
            pInfo->nativeDataFormatCount = 1;
            pInfo->nativeDataFormats[0].format     = ma_format_f32;
            pInfo->nativeDataFormats[0].channels   = (ma_uint32)ch;
            pInfo->nativeDataFormats[0].sampleRate = 48000;
            pInfo->nativeDataFormats[0].flags      = 0;
        }
        return MA_SUCCESS;
    }

    /* Cold path — driver not open.  Briefly load + init + query + release.
     * Must run on the STA thread so COM + init() work correctly. */
    struct GetInfoWork {
        const char   *name;
        ma_device_type type;
        ma_device_info *pInfo;
    } w = { name, type, pInfo };

    auto worker = +[](void *arg) {
        GetInfoWork *w = (GetInfoWork *)arg;
        IASIO *drv = reg_load_driver(w->name);
        if (!drv) return;
        if (safe_asio_init(drv) > 0) {
            long nIn = 0, nOut = 0;
            drv->getChannels(&nIn, &nOut);
            long ch = (w->type == ma_device_type_capture) ? nIn : nOut;
            if (ch > 0) {
                w->pInfo->nativeDataFormatCount = 1;
                w->pInfo->nativeDataFormats[0].format     = ma_format_f32;
                w->pInfo->nativeDataFormats[0].channels   = (ma_uint32)ch;
                w->pInfo->nativeDataFormats[0].sampleRate = 48000;
                w->pInfo->nativeDataFormats[0].flags      = 0;
            }
        }
        drv->Release();
    };
    asio_sta_run(worker, &w);
    return MA_SUCCESS;
}

/* Work struct for the STA thread: load + init + full setup in one shot. */
struct AsioOpenWork {
    /* inputs */
    char        driverName[128];
    ma_uint32   sr;
    long        reqBuf;
    /* outputs */
    IASIO      *drv;
    AsioSetupOut setup;
    HRESULT     hr_create;
    int         init_result;  /* 1=ok, 0=false, -1=crash */
};

static void asio_open_on_sta(void *arg)
{
    AsioOpenWork *w = reinterpret_cast<AsioOpenWork *>(arg);
    w->drv         = nullptr;
    w->init_result = -1;
    w->setup.status = -1;

    fflush(stderr); fprintf(stderr, "[asio/sta] load driver '%s'\n", w->driverName);
    IASIO *drv = reg_load_driver(w->driverName, &w->hr_create);
    if (!drv) {
        fflush(stderr); fprintf(stderr, "[asio/sta] CoCreateInstance failed hr=0x%08lX\n",
                                (unsigned long)(DWORD)w->hr_create);
        return;
    }

    fflush(stderr); fprintf(stderr, "[asio/sta] calling init\n");
    w->init_result = safe_asio_init(drv);
    fflush(stderr); fprintf(stderr, "[asio/sta] init returned %d\n", w->init_result);
    if (w->init_result <= 0) {
        drv->Release();
        return;
    }

    AsioSetupIn si;
    si.drv    = drv;
    si.sr     = w->sr;
    si.reqBuf = w->reqBuf;
    w->setup.callbacks = &g_callbacks;
    safe_asio_setup(&si, &w->setup);
    fflush(stderr); fprintf(stderr, "[asio/sta] setup status=%d nIn=%ld nOut=%ld sr=%.0f buf=%ld\n",
                            w->setup.status, w->setup.nIn, w->setup.nOut,
                            w->setup.actualSr, w->setup.bufSize);

    if (w->setup.status == 0)
        w->drv = drv;
    else
        drv->Release();
}

static ma_result asio_on_device_init(ma_device *pDevice,
                                      const ma_device_config *pConfig,
                                      ma_device_descriptor *pDescPb,
                                      ma_device_descriptor *pDescCap)
{
    /* Determine driver name from capture or playback device ID */
    const char *driverName = nullptr;
    if (pConfig->deviceType == ma_device_type_capture ||
        pConfig->deviceType == ma_device_type_duplex) {
        if (pDescCap->pDeviceID)  driverName = pDescCap->pDeviceID->custom.s;
    }
    if (!driverName && pDescPb && pDescPb->pDeviceID)
        driverName = pDescPb->pDeviceID->custom.s;
    if (!driverName) {
        fflush(stderr); fprintf(stderr, "[asio] no driver name\n");
        return MA_INVALID_ARGS;
    }

    fflush(stderr); fprintf(stderr, "[asio] device_init '%s'\n", driverName);

    /* Run all driver interaction on the dedicated STA thread so COM's STA
     * message dispatch works correctly (required by VoiceMeeter VASIO etc.) */
    AsioOpenWork w = {};
    strncpy(w.driverName, driverName, sizeof(w.driverName) - 1);
    w.sr     = pDescCap ? pDescCap->sampleRate : (pDescPb ? pDescPb->sampleRate : 48000u);
    w.reqBuf = (long)(pDescCap ? pDescCap->periodSizeInFrames
                               : (pDescPb ? pDescPb->periodSizeInFrames : 0u));

    asio_sta_run(asio_open_on_sta, &w);

    if (!w.drv) {
        if (w.init_result == -1 || w.hr_create != S_OK) {
            struct WavRecEngine *logEng = (struct WavRecEngine *)pDevice->pUserData;
            if (logEng) {
                char lb[256];
                snprintf(lb, sizeof(lb),
                    "{\"level\":\"warn\",\"message\":\"ASIO init failed for '%s'\"}",
                    driverName);
                engine_emit(logEng, EVT_LOG, lb);
            }
        }
        return (w.hr_create != S_OK) ? MA_NO_DEVICE : MA_FAILED_TO_INIT_BACKEND;
    }

    /* Copy setup results into the global singleton */
    g_asio.nIn     = w.setup.nIn;
    g_asio.nOut    = w.setup.nOut;
    g_asio.bufSize = w.setup.bufSize;
    memcpy(g_asio.bufInfos, w.setup.bufInfos, sizeof(g_asio.bufInfos));
    memcpy(g_asio.chanInfo, w.setup.chanInfo, sizeof(g_asio.chanInfo));

    g_asio.driver  = w.drv;
    g_asio.pDevice = pDevice;

    /* Fill in miniaudio device descriptors */
    if (pDescCap) {
        pDescCap->format             = ma_format_f32;
        pDescCap->channels           = (ma_uint32)w.setup.nIn;
        pDescCap->sampleRate         = (ma_uint32)w.setup.actualSr;
        pDescCap->periodSizeInFrames = (ma_uint32)w.setup.bufSize;
        pDescCap->periodCount        = 2;
    }
    if (pDescPb) {
        pDescPb->format             = ma_format_f32;
        pDescPb->channels           = (ma_uint32)w.setup.nOut;
        pDescPb->sampleRate         = (ma_uint32)w.setup.actualSr;
        pDescPb->periodSizeInFrames = (ma_uint32)w.setup.bufSize;
        pDescPb->periodCount        = 2;
    }

    return MA_SUCCESS;
}

static void asio_uninit_on_sta(void *)
{
    if (g_asio.driver) {
        g_asio.driver->disposeBuffers();
        g_asio.driver->Release();
        g_asio.driver = nullptr;
    }
}

static ma_result asio_on_device_uninit(ma_device *)
{
    g_asio.running = false;
    asio_sta_run(asio_uninit_on_sta, nullptr);
    g_asio.pDevice = nullptr;
    return MA_SUCCESS;
}

static ma_result asio_on_device_start(ma_device *)
{
    if (!g_asio.driver) return MA_INVALID_OPERATION;
    g_asio.stop_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!g_asio.stop_event) return MA_ERROR;
    if (g_asio.driver->start() != ASE_OK) {
        CloseHandle(g_asio.stop_event);
        g_asio.stop_event = nullptr;
        return MA_FAILED_TO_START_BACKEND_DEVICE;
    }
    g_asio.running = true;
    return MA_SUCCESS;
}

/* Called from miniaudio's worker thread after onDeviceStart.
 * Must block until the device is stopped — ASIO owns its own audio thread
 * via bufferSwitch, so we just wait here for the stop signal. */
static ma_result asio_on_device_data_loop(ma_device *)
{
    if (g_asio.stop_event)
        WaitForSingleObject(g_asio.stop_event, INFINITE);
    return MA_SUCCESS;
}

/* Called by ma_device_stop to unblock the data loop. */
static ma_result asio_on_device_data_loop_wakeup(ma_device *)
{
    if (g_asio.stop_event)
        SetEvent(g_asio.stop_event);
    return MA_SUCCESS;
}

static ma_result asio_on_device_stop(ma_device *)
{
    g_asio.running = false;
    if (g_asio.driver) g_asio.driver->stop();
    if (g_asio.stop_event) {
        CloseHandle(g_asio.stop_event);
        g_asio.stop_event = nullptr;
    }
    return MA_SUCCESS;
}

static ma_result asio_on_context_init(ma_context *pCtx,
                                       const ma_context_config *,
                                       ma_backend_callbacks *pCbs)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    /* Check at least one ASIO driver exists */
    AsioDriverEntry probe[1];
    if (reg_enumerate_drivers(probe, 1) == 0) {
        CoUninitialize();
        return MA_NO_BACKEND; /* causes miniaudio to try next backend */
    }

    /* Start the dedicated STA thread for all driver interactions */
    asio_sta_start();

    pCbs->onContextUninit           = asio_on_context_uninit;
    pCbs->onContextEnumerateDevices = asio_on_enumerate_devices;
    pCbs->onContextGetDeviceInfo    = asio_on_get_device_info;
    pCbs->onDeviceInit              = asio_on_device_init;
    pCbs->onDeviceUninit            = asio_on_device_uninit;
    pCbs->onDeviceStart             = asio_on_device_start;
    pCbs->onDeviceStop              = asio_on_device_stop;
    /* ASIO owns its audio thread via bufferSwitch. The data loop just blocks
     * until ma_device_stop wakes it via onDeviceDataLoopWakeup. */
    pCbs->onDeviceDataLoop          = asio_on_device_data_loop;
    pCbs->onDeviceDataLoopWakeup    = asio_on_device_data_loop_wakeup;

    (void)pCtx;
    return MA_SUCCESS;
}

/* ============================================================================
 * Public C API
 * ========================================================================= */

extern "C" {

void ma_asio_context_config_init(ma_context_config *pConfig)
{
    pConfig->custom.onContextInit = asio_on_context_init;
}

ma_result ma_asio_enumerate_devices(ma_context *pContext,
                                     ma_enum_devices_callback_proc callback,
                                     void *pUserData)
{
    return asio_on_enumerate_devices(pContext, callback, pUserData);
}

} /* extern "C" */

#endif /* WAVREC_HAVE_ASIO */
