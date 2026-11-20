// Sound: the game plays through OpenAL, and the Mac still has it, so every call goes to the Mac's own. Devices and
// contexts are pointers and cross as handles; buffers and sources are numbers and cross as they are. Sample data
// is the game's memory, which stays where it is, so the "static buffer" extension can keep pointing at it.
// The extension functions the game asks for by name (alcGetProcAddress) come back as native function addresses.
// BMAA_TRACE_AL=1 names the calls that set things up.
#include "rt.h"
#include "dr_objc.h"
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void aot_lock(void); void aot_unlock(void);
dr_objc *dr_objc_actual(void);
uint32_t aot_host_fn(void (*fn)(CPU *, uint32_t), uint32_t ctx);
uint32_t guest_call_here(uint32_t addr, int nargs, const uint32_t *args);

#define RET(v) (c->eax = (uint32_t)(v))
#define P(g) ((g) ? G2H(g) : NULL)
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
#define T(...) do { if (tracing()) { fprintf(stderr, "  [al] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ---- the device and its context ----
void I__alcOpenDevice(CPU *c) {
    ALCdevice *d = alcOpenDevice(ARG(0) ? (const char *)G2H(ARG(0)) : NULL);
    fprintf(stderr, "  [al] alcOpenDevice(%s) -> %s\n", ARG(0) ? (const char *)G2H(ARG(0)) : "default", d ? "open" : "FAILED");
    RET(handle(d));
}
void I__alcCloseDevice(CPU *c) { ALCdevice *d = host(ARG(0)); T("alcCloseDevice"); RET(d ? alcCloseDevice(d) : 0); }
void I__alcCreateContext(CPU *c) {
    ALCdevice *d = host(ARG(0));
    ALCcontext *x = d ? alcCreateContext(d, P(ARG(1))) : NULL;
    T("alcCreateContext -> %p", (void *)x);
    RET(handle(x));
}
void I__alcDestroyContext(CPU *c) { ALCcontext *x = host(ARG(0)); T("alcDestroyContext"); if (x) alcDestroyContext(x); }
void I__alcMakeContextCurrent(CPU *c) { T("alcMakeContextCurrent(%08x)", ARG(0)); RET(alcMakeContextCurrent(host(ARG(0)))); }
void I__alcGetCurrentContext(CPU *c) { RET(handle(alcGetCurrentContext())); }
void I__alcGetEnumValue(CPU *c) { RET(alcGetEnumValue(host(ARG(0)), (const char *)G2H(ARG(1)))); }
void I__alcIsExtensionPresent(CPU *c) {
    ALCboolean r = alcIsExtensionPresent(host(ARG(0)), (const char *)G2H(ARG(1)));
    T("alcIsExtensionPresent(%s) -> %d", (const char *)G2H(ARG(1)), r);
    RET(r);
}
void I__alIsExtensionPresent(CPU *c) {
    ALboolean r = alIsExtensionPresent((const char *)G2H(ARG(0)));
    T("alIsExtensionPresent(%s) -> %d", (const char *)G2H(ARG(0)), r);
    RET(r);
}

// ---- the extension functions, by name ----
// Each is the Mac's function, looked up once and called with the game's arguments.
enum { X_BUFFER_STATIC, X_RENDER_CHANNELS, X_MAX_BUSSES, X_NOTIFY, X_ASA_LISTENER, X_ASA_SOURCE, X_N };
static const char *const xname[X_N] = { "alBufferDataStatic", "alMacOSXGetRenderChannelCount", "alcMacOSXMixerMaxiumumBusses",
                                        "alSourceAddNotification", "alcASASetListener", "alcASASetSource" };
static void *xhost[X_N];
static uint32_t xguest[X_N];
struct note { uint32_t fn, user; };
static void notified(ALuint sid, ALuint what, ALvoid *user) {
    struct note *n = user;
    uint32_t a[3] = { sid, what, n->user };
    guest_call_here(n->fn, 3, a);
}
static void ext(CPU *c, uint32_t k) {
    switch (k) {
    case X_BUFFER_STATIC:   // (buffer, format, data, size, frequency)
        ((void (*)(ALint, ALenum, const ALvoid *, ALsizei, ALsizei))xhost[k])(ARG(0), ARG(1), P(ARG(2)), ARG(3), ARG(4));
        break;
    case X_RENDER_CHANNELS: RET(((ALint (*)(void))xhost[k])()); break;
    case X_MAX_BUSSES: ((void (*)(ALint))xhost[k])(ARG(0)); T("alcMacOSXMixerMaxiumumBusses(%d)", ARG(0)); break;
    case X_NOTIFY: {        // (source, which notification, the game's function, its word) -> error
        struct note *n = malloc(sizeof *n);
        n->fn = ARG(2); n->user = ARG(3);
        RET(((ALenum (*)(ALuint, ALuint, void (*)(ALuint, ALuint, ALvoid *), ALvoid *))xhost[k])(ARG(0), ARG(1), notified, n));
        break;
    }
    case X_ASA_LISTENER: RET(((ALenum (*)(ALuint, ALvoid *, ALuint))xhost[k])(ARG(0), P(ARG(1)), ARG(2))); break;
    case X_ASA_SOURCE: RET(((ALenum (*)(ALuint, ALuint, ALvoid *, ALuint))xhost[k])(ARG(0), ARG(1), P(ARG(2)), ARG(3))); break;
    }
}
void I__alcGetProcAddress(CPU *c) {
    const char *name = (const char *)G2H(ARG(1));
    for (uint32_t k = 0; k < X_N; k++) {
        if (strcmp(name, xname[k])) continue;
        if (!xhost[k]) xhost[k] = alcGetProcAddress(host(ARG(0)), xname[k]);
        if (xhost[k] && !xguest[k]) xguest[k] = aot_host_fn(ext, k);
        T("alcGetProcAddress(%s) -> %s", name, xhost[k] ? "the Mac's" : "not there");
        RET(xhost[k] ? xguest[k] : 0);
        return;
    }
    fprintf(stderr, "  [al] alcGetProcAddress(%s): not a function this port knows, answered 0\n", name);
    RET(0);
}

// ---- buffers, sources, the listener ----
void I__alGetError(CPU *c) { RET(alGetError()); }
void I__alGetFloat(CPU *c) { FPUSH((double)alGetFloat(ARG(0))); }
void I__alDistanceModel(CPU *c) { alDistanceModel(ARG(0)); }
void I__alGenBuffers(CPU *c) { alGenBuffers(ARG(0), P(ARG(1))); }
void I__alDeleteBuffers(CPU *c) { alDeleteBuffers(ARG(0), P(ARG(1))); }
void I__alBufferData(CPU *c) { alBufferData(ARG(0), ARG(1), P(ARG(2)), ARG(3), ARG(4)); }
void I__alGenSources(CPU *c) { alGenSources(ARG(0), P(ARG(1))); T("alGenSources(%d)", ARG(0)); }
void I__alDeleteSources(CPU *c) { alDeleteSources(ARG(0), P(ARG(1))); }
void I__alSourcePlay(CPU *c) { alSourcePlay(ARG(0)); T("alSourcePlay(%u)", ARG(0)); }
void I__alSourceStop(CPU *c) { alSourceStop(ARG(0)); }
void I__alSourceQueueBuffers(CPU *c) { alSourceQueueBuffers(ARG(0), ARG(1), P(ARG(2))); }
void I__alSourcef(CPU *c) { alSourcef(ARG(0), ARG(1), ARGF(2)); }
void I__alSourcei(CPU *c) { alSourcei(ARG(0), ARG(1), ARG(2)); }
void I__alSourcefv(CPU *c) { alSourcefv(ARG(0), ARG(1), P(ARG(2))); }
void I__alGetSourcei(CPU *c) { alGetSourcei(ARG(0), ARG(1), P(ARG(2))); }
void I__alListenerfv(CPU *c) { alListenerfv(ARG(0), P(ARG(1))); }

// ---- decoding: the game hands a whole sound file it holds in memory to AudioFile and reads it back as PCM ----
// Its two callbacks (read at a position, size) only copy from that block, {?, type, size, data}; they are answered
// here from the same block rather than called back, since the Mac's buffer is not memory the game can write to.
#include <AudioToolbox/AudioToolbox.h>
struct memfile { uint32_t data, size; };
static OSStatus mem_read(void *u, SInt64 pos, UInt32 want, void *buf, UInt32 *got) {
    struct memfile *m = u;
    if (got) *got = 0;
    if (pos < 0 || pos > m->size) return kAudioFilePositionError;
    UInt32 n = want; if ((SInt64)n > m->size - pos) n = (UInt32)(m->size - pos);
    memcpy(buf, G2H(m->data + (uint32_t)pos), n);
    if (got) *got = n;
    return noErr;
}
static SInt64 mem_size(void *u) { return ((struct memfile *)u)->size; }
// (client data, read, write, size, set size, type hint, AudioFileID *out)
void I__AudioFileOpenWithCallbacks(CPU *c) {
    uint32_t cl = ARG(0);
    static int said;
    if (!cl || !ARG(6)) { RET(kAudio_ParamError); return; }
    struct memfile *m = malloc(sizeof *m);
    m->size = rd32(cl + 8); m->data = rd32(cl + 12);
    AudioFileID f = NULL;
    OSStatus e = AudioFileOpenWithCallbacks(m, mem_read, NULL, mem_size, NULL, ARG(5), &f);
    if (e) free(m);
    if (!said++ || (e && tracing()))
        fprintf(stderr, "  [al] first sound file handed to AudioFile: %u bytes, type hint %08x -> %d\n", m ? rd32(cl + 8) : 0, ARG(5), (int)e);
    wr32(ARG(6), e ? 0 : handle(f));
    RET(e);
}
void I__AudioFileClose(CPU *c) { AudioFileID f = host(ARG(0)); RET(f ? AudioFileClose(f) : kAudio_ParamError); }
void I__ExtAudioFileWrapAudioFileID(CPU *c) {
    ExtAudioFileRef x = NULL;
    AudioFileID f = host(ARG(0));
    OSStatus e = f ? ExtAudioFileWrapAudioFileID(f, (Boolean)ARG(1), &x) : kAudio_ParamError;
    if (ARG(2)) wr32(ARG(2), e ? 0 : handle(x));
    RET(e);
}
void I__ExtAudioFileDispose(CPU *c) { ExtAudioFileRef x = host(ARG(0)); RET(x ? ExtAudioFileDispose(x) : kAudio_ParamError); }
// the properties the game uses are plain numbers (a stream description, a frame count): the same bytes in both worlds
void I__ExtAudioFileGetProperty(CPU *c) {
    ExtAudioFileRef x = host(ARG(0));
    UInt32 n = ARG(2) ? rd32(ARG(2)) : 0;
    OSStatus e = x ? ExtAudioFileGetProperty(x, ARG(1), &n, P(ARG(3))) : kAudio_ParamError;
    if (ARG(2)) wr32(ARG(2), n);
    RET(e);
}
void I__ExtAudioFileSetProperty(CPU *c) {
    ExtAudioFileRef x = host(ARG(0));
    RET(x ? ExtAudioFileSetProperty(x, ARG(1), ARG(2), P(ARG(3))) : kAudio_ParamError);
}
// (file, UInt32 *frames in and out, AudioBufferList *): the game's list holds 32-bit data pointers
void I__ExtAudioFileRead(CPU *c) {
    ExtAudioFileRef x = host(ARG(0));
    uint32_t gl = ARG(2);
    if (!x || !ARG(1) || !gl) { RET(kAudio_ParamError); return; }
    UInt32 nb = rd32(gl), frames = rd32(ARG(1));
    if (nb < 1 || nb > 8) { RET(kAudio_ParamError); return; }
    struct { AudioBufferList l; AudioBuffer more[7]; } h;
    h.l.mNumberBuffers = nb;
    for (UInt32 i = 0; i < nb; i++) {
        h.l.mBuffers[i].mNumberChannels = rd32(gl + 4 + 12 * i);
        h.l.mBuffers[i].mDataByteSize = rd32(gl + 8 + 12 * i);
        h.l.mBuffers[i].mData = P(rd32(gl + 12 + 12 * i));
    }
    OSStatus e = ExtAudioFileRead(x, &frames, &h.l);
    wr32(ARG(1), frames);
    for (UInt32 i = 0; i < nb; i++) wr32(gl + 8 + 12 * i, h.l.mBuffers[i].mDataByteSize);
    RET(e);
}

// ---- which sound devices there are: a second part of the game asks every frame, to notice a change ----
// The default output, the default input and the list of devices are numbers; anything else is refused by name.
#include <CoreAudio/CoreAudio.h>
static int plain_hw(uint32_t sel) { return sel == kAudioHardwarePropertyDefaultOutputDevice || sel == kAudioHardwarePropertyDefaultInputDevice || sel == kAudioHardwarePropertyDevices; }
static void hw_refused(const char *fn, uint32_t sel) {
    static uint32_t seen[8]; static int n;
    for (int i = 0; i < n; i++) if (seen[i] == sel) return;
    if (n < 8) seen[n++] = sel;
    fprintf(stderr, "  [al] %s('%c%c%c%c'): not answered yet\n", fn, sel >> 24, (sel >> 16) & 255, (sel >> 8) & 255, sel & 255);
}
// (selector, UInt32 *size in and out, data out)
void I__AudioHardwareGetProperty(CPU *c) {
    uint32_t sel = ARG(0);
    if (!plain_hw(sel) || !ARG(1) || !ARG(2)) { hw_refused("AudioHardwareGetProperty", sel); RET(kAudioHardwareUnknownPropertyError); return; }
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 n = rd32(ARG(1));
    OSStatus e = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &n, G2H(ARG(2)));
    wr32(ARG(1), n);
    RET(e);
}
// (selector, UInt32 *size out, Boolean *writable out)
void I__AudioHardwareGetPropertyInfo(CPU *c) {
    uint32_t sel = ARG(0);
    if (!plain_hw(sel)) { hw_refused("AudioHardwareGetPropertyInfo", sel); RET(kAudioHardwareUnknownPropertyError); return; }
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 n = 0; Boolean w = 0;
    OSStatus e = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &n);
    if (!e) AudioObjectIsPropertySettable(kAudioObjectSystemObject, &a, &w);
    if (ARG(1)) wr32(ARG(1), n);
    if (ARG(2)) wr8(ARG(2), w);
    RET(e);
}
