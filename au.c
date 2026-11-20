// Sound, the second way: the game's sound library opens the Mac's output unit itself (AudioComponent, AudioUnit) and
// asks the device a few things through the old AudioDevice calls. Every call goes to the Mac's own; units and
// components are pointers and cross as handles. What needs care is the render callback: the Mac calls it on its
// audio thread with its own buffers, and the game's function fills buffers in the game's memory, so each unit keeps a
// buffer list of the game's shape and the samples are copied out after the call.
// The microphone is never opened: turning input on is refused, so no permission prompt appears for a game.
// BMAA_TRACE_AL=1 names the calls.
#include "rt.h"
#include "dr_objc.h"
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void aot_lock(void); void aot_unlock(void);
dr_objc *dr_objc_actual(void);
uint32_t guest_call_here(uint32_t addr, int nargs, const uint32_t *args);
uint32_t aot_malloc(uint32_t n); void aot_free(uint32_t p);

#define RET(v) (c->eax = (uint32_t)(v))
static void *host(uint32_t v) {
    if (!v) return NULL;
    aot_lock(); void *h = dr_objc_object_for(dr_objc_actual(), v); aot_unlock();
    return h;
}
static uint32_t handle(const void *h) {
    if (!h) return 0;
    aot_lock(); uint32_t v = dr_objc_handle_for(dr_objc_actual(), (void *)h); aot_unlock();
    return v;
}
static int tracing(void) { static int v = -1; if (v < 0) v = getenv("BMAA_TRACE_AL") != NULL; return v; }
#define T(...) do { if (tracing()) { fprintf(stderr, "  [au] " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define CC4(v) (int)((v) >> 24 & 255 ? (v) >> 24 & 255 : '.'), (int)((v) >> 16 & 255 ? (v) >> 16 & 255 : '.'), (int)((v) >> 8 & 255 ? (v) >> 8 & 255 : '.'), (int)((v) & 255 ? (v) & 255 : '.')

enum { NUNIT = 8, NBUF = 8 };
static struct unit {
    AudioUnit au;
    uint32_t h;                     // its handle in the game
    uint32_t proc, refcon;          // the game's render function and its context
    uint32_t g_flags, g_ts, g_abl;  // the game's side of the call's arguments
    uint32_t g_buf[NBUF], g_cap[NBUF];
    unsigned long calls;
} units[NUNIT];
static struct unit *unit_of(uint32_t h) {
    for (int i = 0; i < NUNIT; i++) if (units[i].au && units[i].h == h) return &units[i];
    return NULL;
}

static OSStatus render(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io) {
    struct unit *u = ref;
    UInt32 nb = io ? io->mNumberBuffers : 0;
    if (!u->proc || nb > NBUF) {
        for (UInt32 i = 0; i < nb; i++) if (io->mBuffers[i].mData) memset(io->mBuffers[i].mData, 0, io->mBuffers[i].mDataByteSize);
        return noErr;
    }
    if (!u->g_flags) { u->g_flags = aot_malloc(8); u->g_ts = aot_malloc(64); u->g_abl = aot_malloc(4 + 12 * NBUF); }
    wr32(u->g_flags, fl ? *fl : 0);
    if (ts) memcpy(G2H(u->g_ts), ts, 64); else memset(G2H(u->g_ts), 0, 64);
    wr32(u->g_abl, nb);
    for (UInt32 i = 0; i < nb; i++) {
        UInt32 sz = io->mBuffers[i].mDataByteSize;
        if (u->g_cap[i] < sz) { if (u->g_buf[i]) aot_free(u->g_buf[i]); u->g_cap[i] = sz * 2; u->g_buf[i] = aot_malloc(u->g_cap[i]); }
        memset(G2H(u->g_buf[i]), 0, sz);
        wr32(u->g_abl + 4 + 12 * i, io->mBuffers[i].mNumberChannels);
        wr32(u->g_abl + 8 + 12 * i, sz);
        wr32(u->g_abl + 12 + 12 * i, u->g_buf[i]);
    }
    uint32_t a[6] = { u->refcon, u->g_flags, u->g_ts, bus, n, u->g_abl };
    OSStatus e = (OSStatus)guest_call_here(u->proc, 6, a);
    if (fl) *fl = rd32(u->g_flags);
    for (UInt32 i = 0; i < nb; i++) {
        UInt32 sz = rd32(u->g_abl + 8 + 12 * i), gp = rd32(u->g_abl + 12 + 12 * i);
        if (sz > io->mBuffers[i].mDataByteSize) sz = io->mBuffers[i].mDataByteSize;
        if (io->mBuffers[i].mData && gp) memcpy(io->mBuffers[i].mData, G2H(gp), sz);
        io->mBuffers[i].mDataByteSize = sz;
    }
    // has anything but silence come out yet? (the unit's format is 32-bit float here; said once)
    static int heard;
    if (!heard && nb && io->mBuffers[0].mData) {
        const float *f = io->mBuffers[0].mData; float peak = 0;
        for (UInt32 k = 0; k < io->mBuffers[0].mDataByteSize / 4; k++) { float v = f[k] < 0 ? -f[k] : f[k]; if (v > peak) peak = v; }
        if (peak > 0.001f && peak < 8.0f) { heard = 1; fprintf(stderr, "  [au] first block that is not silence: peak %.3f after %lu calls\n", peak, u->calls); }
    }
    if (u->calls++ == 0) fprintf(stderr, "  [au] the game's sound function is being called: %u frames, %u buffer(s) -> %d\n", (unsigned)n, (unsigned)nb, (int)e);
    return e;
}

// ---- components and units ----
void I__AudioComponentFindNext(CPU *c) {
    AudioComponentDescription d; memcpy(&d, G2H(ARG(1)), sizeof d);
    AudioComponent comp = AudioComponentFindNext(host(ARG(0)), &d);
    T("AudioComponentFindNext('%c%c%c%c' '%c%c%c%c') -> %s", CC4(d.componentType), CC4(d.componentSubType), comp ? "found" : "none");
    RET(handle(comp));
}
void I__AudioComponentInstanceNew(CPU *c) {
    AudioUnit au = NULL;
    OSStatus e = AudioComponentInstanceNew(host(ARG(0)), &au);
    uint32_t h = 0;
    if (!e && au) {
        struct unit *u = NULL;
        for (int i = 0; i < NUNIT && !u; i++) if (!units[i].au) u = &units[i];
        if (!u) { AudioComponentInstanceDispose(au); e = -1; }
        else { u->au = au; u->h = h = handle(au); u->proc = u->refcon = 0; u->calls = 0; }
    }
    if (ARG(1)) wr32(ARG(1), h);
    fprintf(stderr, "  [au] the game opens a sound unit -> %d\n", (int)e);
    RET(e);
}
void I__AudioComponentInstanceDispose(CPU *c) {
    struct unit *u = unit_of(ARG(0));
    if (!u) { RET(-1); return; }
    OSStatus e = AudioComponentInstanceDispose(u->au);
    u->au = NULL; u->proc = 0;
    T("AudioComponentInstanceDispose -> %d", (int)e);
    RET(e);
}
// (unit, property, scope, element, data, size)
void I__AudioUnitSetProperty(CPU *c) {
    struct unit *u = unit_of(ARG(0));
    uint32_t prop = ARG(1), scope = ARG(2), elem = ARG(3), data = ARG(4), size = ARG(5);
    if (!u) { RET(-1); return; }
    OSStatus e;
    if (prop == kAudioOutputUnitProperty_EnableIO && scope == kAudioUnitScope_Input && data && rd32(data)) {
        fprintf(stderr, "  [au] the game asks for sound input: refused, the microphone stays closed\n");
        RET(kAudioUnitErr_InvalidPropertyValue); return;
    }
    if (prop == kAudioUnitProperty_SetRenderCallback || prop == kAudioOutputUnitProperty_SetInputCallback) {
        u->proc = data ? rd32(data) : 0; u->refcon = data ? rd32(data + 4) : 0;
        AURenderCallbackStruct cb = { u->proc ? render : NULL, u };
        e = AudioUnitSetProperty(u->au, prop, scope, elem, &cb, sizeof cb);
    } else {
        e = AudioUnitSetProperty(u->au, prop, scope, elem, data ? G2H(data) : NULL, size);
        if (prop == kAudioUnitProperty_StreamFormat && data) {
            AudioStreamBasicDescription f; memcpy(&f, G2H(data), sizeof f);
            fprintf(stderr, "  [au] stream format (scope %u, element %u): %.0f Hz, %u channel(s), %u bits, flags 0x%x -> %d\n",
                    scope, elem, f.mSampleRate, (unsigned)f.mChannelsPerFrame, (unsigned)f.mBitsPerChannel, (unsigned)f.mFormatFlags, (int)e);
        }
    }
    T("AudioUnitSetProperty(%u, scope %u, element %u, %u bytes) -> %d", prop, scope, elem, size, (int)e);
    RET(e);
}
// (unit, property, scope, element, data out, size in and out)
void I__AudioUnitGetProperty(CPU *c) {
    struct unit *u = unit_of(ARG(0));
    if (!u || !ARG(4) || !ARG(5)) { RET(-1); return; }
    UInt32 n = rd32(ARG(5));
    OSStatus e = AudioUnitGetProperty(u->au, ARG(1), ARG(2), ARG(3), G2H(ARG(4)), &n);
    wr32(ARG(5), n);
    T("AudioUnitGetProperty(%u, scope %u, element %u) -> %d, %u bytes", ARG(1), ARG(2), ARG(3), (int)e, (unsigned)n);
    RET(e);
}
void I__AudioUnitInitialize(CPU *c) { struct unit *u = unit_of(ARG(0)); OSStatus e = u ? AudioUnitInitialize(u->au) : -1; fprintf(stderr, "  [au] AudioUnitInitialize -> %d\n", (int)e); RET(e); }
void I__AudioUnitUninitialize(CPU *c) { struct unit *u = unit_of(ARG(0)); OSStatus e = u ? AudioUnitUninitialize(u->au) : -1; T("AudioUnitUninitialize -> %d", (int)e); RET(e); }
void I__AudioOutputUnitStart(CPU *c) { struct unit *u = unit_of(ARG(0)); OSStatus e = u ? AudioOutputUnitStart(u->au) : -1; fprintf(stderr, "  [au] AudioOutputUnitStart -> %d\n", (int)e); RET(e); }
void I__AudioOutputUnitStop(CPU *c) { struct unit *u = unit_of(ARG(0)); OSStatus e = u ? AudioOutputUnitStop(u->au) : -1; T("AudioOutputUnitStop -> %d", (int)e); RET(e); }
// only an input unit pulls with this, and input is refused
void I__AudioUnitRender(CPU *c) { RET(kAudioUnitErr_CannotDoInCurrentContext); }

// ---- the device, through the calls of before 10.6 ----
static AudioObjectPropertyAddress addr_of(uint32_t sel, uint32_t chan, uint32_t input) {
    AudioObjectPropertyAddress a = { sel, input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput, chan };
    return a;
}
static int is_string(uint32_t sel) { return sel == 'lnam' || sel == 'uid ' || sel == 'lmak' || sel == 'lmod'; }
// 'slay' is a buffer list: pointers inside, so the game's is 4 + 12 per buffer and the Mac's 8 + 16
static OSStatus get(AudioObjectID dev, AudioObjectPropertyAddress *a, UInt32 *n, void *out) {
    OSStatus e = AudioObjectGetPropertyData(dev, a, 0, NULL, n, out);
    if (e) { AudioObjectPropertyAddress g = *a; g.mScope = kAudioObjectPropertyScopeGlobal; OSStatus e2 = AudioObjectGetPropertyData(dev, &g, 0, NULL, n, out); if (!e2) e = 0; }
    return e;
}
// (device, channel, isInput, selector, size in and out, data out)
void I__AudioDeviceGetProperty(CPU *c) {
    uint32_t dev = ARG(0), sel = ARG(3), psize = ARG(4), data = ARG(5);
    AudioObjectPropertyAddress a = addr_of(sel, ARG(1), ARG(2) & 255);
    if (!psize || !data) { RET(kAudioHardwareIllegalOperationError); return; }
    OSStatus e;
    if (is_string(sel)) {
        CFStringRef s = NULL; UInt32 n = sizeof s;
        e = get(dev, &a, &n, &s);
        wr32(data, e ? 0 : handle(s)); wr32(psize, 4);
    } else if (sel == 'slay') {
        UInt32 n = 0; a.mScope = (ARG(2) & 255) ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
        e = AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &n);
        AudioBufferList *l = e ? NULL : calloc(1, n ? n : 1);
        if (l) e = AudioObjectGetPropertyData(dev, &a, 0, NULL, &n, l);
        UInt32 have = rd32(psize), nb = l && !e ? l->mNumberBuffers : 0;
        if (have >= 4) wr32(data, nb);
        for (UInt32 i = 0; i < nb && 4 + 12 * (i + 1) <= have; i++) {
            wr32(data + 4 + 12 * i, l->mBuffers[i].mNumberChannels); wr32(data + 8 + 12 * i, l->mBuffers[i].mDataByteSize); wr32(data + 12 + 12 * i, 0);
        }
        wr32(psize, 4 + 12 * nb);
        free(l);
    } else {
        UInt32 n = rd32(psize);
        e = get(dev, &a, &n, G2H(data));
        wr32(psize, n);
    }
    T("AudioDeviceGetProperty(device %u, '%c%c%c%c', %s) -> %d", dev, CC4(sel), (ARG(2) & 255) ? "input" : "output", (int)e);
    RET(e);
}
// (device, channel, isInput, selector, size out, writable out)
void I__AudioDeviceGetPropertyInfo(CPU *c) {
    uint32_t dev = ARG(0), sel = ARG(3);
    AudioObjectPropertyAddress a = addr_of(sel, ARG(1), ARG(2) & 255);
    UInt32 n = 0; Boolean w = 0;
    OSStatus e = AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &n);
    if (e) { a.mScope = kAudioObjectPropertyScopeGlobal; e = AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &n); }
    if (!e) AudioObjectIsPropertySettable(dev, &a, &w);
    if (!e && is_string(sel)) n = 4;
    if (!e && sel == 'slay') n = n >= 8 ? 4 + 12 * ((n - 8) / 16) : 4;
    if (ARG(4)) wr32(ARG(4), n);
    if (ARG(5)) wr8(ARG(5), w);
    T("AudioDeviceGetPropertyInfo(device %u, '%c%c%c%c') -> %d, %u bytes", dev, CC4(sel), (int)e, (unsigned)n);
    RET(e);
}
// (device, when, channel, isInput, selector, size, data)
void I__AudioDeviceSetProperty(CPU *c) {
    uint32_t dev = ARG(0), sel = ARG(4), size = ARG(5), data = ARG(6);
    AudioObjectPropertyAddress a = addr_of(sel, ARG(2), ARG(3) & 255);
    OSStatus e = data ? AudioObjectSetPropertyData(dev, &a, 0, NULL, size, G2H(data)) : kAudioHardwareIllegalOperationError;
    if (e && data) { a.mScope = kAudioObjectPropertyScopeGlobal; e = AudioObjectSetPropertyData(dev, &a, 0, NULL, size, G2H(data)); }
    T("AudioDeviceSetProperty(device %u, '%c%c%c%c', %u bytes) -> %d", dev, CC4(sel), size, (int)e);
    RET(e);
}
