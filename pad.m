// Gamepads. The game finds pads through IOHIDManager on a thread of its own and knows each model from a list in its
// resources (InputDevices/*.plist: vendor, product, and which HID page:usage is which button). That list ends in
// 2013; a DualSense, a Switch Pro or a current Xbox pad is not in it, and their HID reports are not ones it could
// read. So no HID device is shown to the game as it is. Every pad the Mac's GameController framework knows is shown
// as the model the game knows best, a wired Xbox 360 pad (1118:654, XBox360Wired.plist), with that model's elements:
//   buttons page 9: 1 A, 2 B, 3 X, 4 Y, 5 LB, 6 RB, 7 left stick, 8 right stick, 9 start, 10 back, 11 guide,
//                   12 up, 13 down, 14 left, 15 right
//   axes page 1: 0x30 left X, 0x31 left Y, 0x32 left trigger, 0x33 right X, 0x34 right Y, 0x35 right trigger
// Sticks are -32768..32767 with down positive (BMAA_PAD_Y_UP=1 for up positive), triggers 0..255.
// The game's HID thread ends in CFRunLoopRun; that call is answered here by the loop that reads the pads and calls
// the game's own callbacks (device arrived, device left, value changed) on that thread, as the Mac would.
// BMAA_NO_PADS=1 leaves all of it unanswered, as before this file. BMAA_TRACE_PAD=1 says what it sees.
#import <GameController/GameController.h>
#import <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "rt.h"
#include "dr_objc.h"

void aot_lock(void); void aot_unlock(void);
dr_objc *dr_objc_actual(void);
uint32_t aot_malloc(uint32_t n);
uint32_t guest_call_here(uint32_t addr, int nargs, const uint32_t *args);
void unimpl_import(CPU *c, const char *name);

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
#define PAD_NUM 0x4D554E50u   /* marks a number of ours in the game's memory */
static int off(void) { static int v = -1; if (v < 0) v = getenv("BMAA_NO_PADS") != NULL; return v; }
static int tracing(void) { static int v = -1; if (v < 0) v = getenv("BMAA_TRACE_PAD") != NULL; return v; }

enum { NBUTTON = 15, NAXIS = 6, NELEM = NBUTTON + NAXIS, NPAD = 4 };
static struct pad {
    void *gc;                 // the GCController (retained), NULL while the slot is free
    CFTypeRef dev;            // what stands for the device: an object of its own, only ever compared and looked up
    uint32_t devh;            // its handle in the game
    uint32_t elem[NELEM];     // the game's side of each element: { device, page, usage, cookie }
    uint32_t value[NELEM];    // and of its value: { element, number, time low, time high }
    int32_t last[NELEM];
    CFMutableDictionaryRef props;
} pads[NPAD];
static uint32_t mgr;                               // the one manager, a word of the game's memory
static uint32_t cb_match[2], cb_gone[2], cb_value[2];   // the game's function and its context
static uint32_t hid_loop;                          // the run loop the game scheduled the manager on
static pthread_t hid_thread;
static volatile int stop;

static void num(CFMutableDictionaryRef d, CFStringRef k, int32_t v) {
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    CFDictionarySetValue(d, k, n); CFRelease(n);
}
static void pad_setup(struct pad *p, int slot) {
    if (!p->dev) {
        p->dev = CFDataCreateMutable(NULL, 0);
        p->devh = handle(p->dev);
        p->props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        num(p->props, CFSTR("VendorID"), 1118); num(p->props, CFSTR("ProductID"), 654);
        num(p->props, CFSTR("VersionNumber"), 0x114); num(p->props, CFSTR("LocationID"), 0x14100000 + slot);
        num(p->props, CFSTR("PrimaryUsagePage"), 1); num(p->props, CFSTR("PrimaryUsage"), 5);
        num(p->props, CFSTR("DeviceUsagePage"), 1); num(p->props, CFSTR("DeviceUsage"), 5);
        CFDictionarySetValue(p->props, CFSTR("Product"), CFSTR("Controller"));
        CFDictionarySetValue(p->props, CFSTR("Manufacturer"), CFSTR("©Microsoft Corporation"));
        CFDictionarySetValue(p->props, CFSTR("Transport"), CFSTR("USB"));
        for (int e = 0; e < NELEM; e++) {
            p->elem[e] = aot_malloc(16); p->value[e] = aot_malloc(16);
            wr32(p->elem[e], p->devh);
            wr32(p->elem[e] + 4, e < NBUTTON ? 9 : 1);
            wr32(p->elem[e] + 8, e < NBUTTON ? (uint32_t)e + 1 : 0x30 + (uint32_t)(e - NBUTTON));
            wr32(p->elem[e] + 12, (uint32_t)e + 1);
            wr32(p->value[e], p->elem[e]);
        }
    }
    for (int e = 0; e < NELEM; e++) p->last[e] = 0;
}
static int32_t stick(float v) { int32_t r = (int32_t)(v * 32767.0f); return r < -32768 ? -32768 : r > 32767 ? 32767 : r; }
// For testing with nobody at the pad (BMAA_PAD_FILE=path): a line "element value milliseconds" written to that file
// holds that element of every connected pad at that value for that long; the file is removed once read.
static int inj_elem = -1; static int32_t inj_value; static uint64_t inj_until;
static void inject_poll(void) {
    static const char *path; static int asked;
    if (!asked) { asked = 1; path = getenv("BMAA_PAD_FILE"); }
    if (!path) return;
    FILE *f = fopen(path, "r"); if (!f) return;
    int e = -1, v = 0, ms = 0;
    if (fscanf(f, "%d %d %d", &e, &v, &ms) == 3 && e >= 0 && e < NELEM) {
        inj_elem = e; inj_value = v; inj_until = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + (uint64_t)ms * 1000000u;
        fprintf(stderr, "  [pad] test: element %d held at %d for %d ms\n", e, v, ms);
        // what the game holds about its pads at this moment (its manager object is the callbacks' context)
        uint32_t m = cb_value[1];
        fprintf(stderr, "  [pad] test: the game's input-off flag is %u\n", rd32(0x37c3a9c) & 255);
        if (m) {   // its table of known pads, by (product << 16 | vendor): how many, and is ours in it
            uint32_t n = rd32(m + 0x5c), want = 0x028e045e, found = 0, shown = 0;
            fprintf(stderr, "  [pad] test: the game knows %u pad definitions;", rd32(m + 0x68));
            uint32_t st[64]; int sp = 0;
            while ((n > 0x1000 || sp) && shown < 400) {
                while (n > 0x1000 && sp < 64) { st[sp++] = n; n = rd32(n + 8); }
                if (!sp) break;
                n = st[--sp];
                if (rd32(n + 0x10) == want) found = 1;
                if (shown++ < 12) fprintf(stderr, " %x", rd32(n + 0x10));
                n = rd32(n + 0xc);
            }
            fprintf(stderr, " ... ours %s\n", found ? "is there" : "is NOT there");
        }
        for (int i = 0; m && i < 32; i++) {
            uint32_t d = rd32(m + 0x84 + 4 * i); if (!d) continue;
            fprintf(stderr, "  [pad] test: game slot %d: device 0x%x id 0x%x connected %u, %u configuration(s)\n",
                    i, rd32(d), rd32(d + 0xc), rd32(d + 0x70) & 255, rd32(d + 0xb0));
            uint32_t n = rd32(d + 0xa8);                       // the leftmost node of its map of configurations
            if (rd32(d + 0xb0) && n > 0x1000) {
                fprintf(stderr, "  [pad] test:   first node:");
                for (int k = 4; k < 30; k++) fprintf(stderr, " %x", rd32(n + 4 * k));
                fputc('\n', stderr);
            }
        }
    }
    fclose(f); remove(path);
}
static void inject(int32_t now[NELEM]) {
    if (inj_elem >= 0 && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < inj_until) now[inj_elem] = inj_value;
}
static void pad_read(struct pad *p, int32_t now[NELEM]) {
    static int y_up = -1; if (y_up < 0) y_up = getenv("BMAA_PAD_Y_UP") != NULL;
    memset(now, 0, NELEM * sizeof now[0]);
    if (p->gc == (void *)kCFNull) { inject(now); return; }   // the test pad: nobody holds it
    GCController *gc = (__bridge GCController *)p->gc;
    GCExtendedGamepad *g = gc.extendedGamepad;
    if (!g) return;
    now[0] = g.buttonA.pressed; now[1] = g.buttonB.pressed; now[2] = g.buttonX.pressed; now[3] = g.buttonY.pressed;
    now[4] = g.leftShoulder.pressed; now[5] = g.rightShoulder.pressed;
    now[6] = g.leftThumbstickButton.pressed; now[7] = g.rightThumbstickButton.pressed;
    now[8] = g.buttonMenu.pressed; now[9] = g.buttonOptions.pressed; now[10] = g.buttonHome.pressed;
    now[11] = g.dpad.up.pressed; now[12] = g.dpad.down.pressed; now[13] = g.dpad.left.pressed; now[14] = g.dpad.right.pressed;
    float ly = g.leftThumbstick.yAxis.value, ry = g.rightThumbstick.yAxis.value;
    now[15] = stick(g.leftThumbstick.xAxis.value);  now[16] = stick(y_up ? ly : -ly);
    now[17] = (int32_t)(g.leftTrigger.value * 255.0f + 0.5f);
    now[18] = stick(g.rightThumbstick.xAxis.value); now[19] = stick(y_up ? ry : -ry);
    now[20] = (int32_t)(g.rightTrigger.value * 255.0f + 0.5f);
    inject(now);
}
static void call4(const uint32_t cb[2], uint32_t what) {
    if (!cb[0]) return;
    uint32_t a[4] = { cb[1], 0, mgr, what };
    guest_call_here(cb[0], 4, a);
}
// Which pads there are now; a new one takes a free slot. Returns the slots that came and went as bit masks.
static void pads_scan(unsigned *came, unsigned *went) {
    *came = *went = 0;
    NSArray<GCController *> *all = [GCController controllers];
    for (int s = 0; s < NPAD; s++) {
        if (!pads[s].gc || pads[s].gc == (void *)kCFNull) continue;
        GCController *gc = (__bridge GCController *)pads[s].gc;
        if ([all indexOfObjectIdenticalTo:gc] == NSNotFound) { CFRelease(pads[s].gc); pads[s].gc = NULL; *went |= 1u << s; }
    }
    // with BMAA_PAD_FILE, a pad nobody holds sits in the last slot, so the game can be driven with no controller here
    if (getenv("BMAA_PAD_FILE") && !pads[NPAD - 1].gc) {
        pads[NPAD - 1].gc = (void *)kCFNull; pad_setup(&pads[NPAD - 1], NPAD - 1); *came |= 1u << (NPAD - 1);
        fprintf(stderr, "  [pad] test pad in slot %d\n", NPAD);
    }
    for (GCController *gc in all) {
        if (!gc.extendedGamepad) continue;
        int have = 0, freeslot = -1;
        for (int s = 0; s < NPAD; s++) {
            if (pads[s].gc == (__bridge void *)gc) have = 1;
            else if (!pads[s].gc && freeslot < 0 && !(*went & (1u << s))) freeslot = s;
        }
        if (have || freeslot < 0) continue;
        pads[freeslot].gc = (void *)CFBridgingRetain(gc);
        pad_setup(&pads[freeslot], freeslot);
        *came |= 1u << freeslot;
        fprintf(stderr, "  [pad] %s is pad %d, shown to the game as a wired Xbox 360 pad\n", gc.vendorName.UTF8String ?: "a controller", freeslot + 1);
    }
}

// ---- the manager ----
void I__IOHIDManagerCreate(CPU *c) {
    if (off()) { unimpl_import(c, "_IOHIDManagerCreate"); return; }
    if (!mgr) mgr = aot_malloc(16);
    RET(mgr);
}
void I__IOHIDManagerSetDeviceMatchingMultiple(CPU *c) { if (off()) unimpl_import(c, "_IOHIDManagerSetDeviceMatchingMultiple"); }
void I__IOHIDManagerRegisterDeviceMatchingCallback(CPU *c) { if (off()) { unimpl_import(c, "_IOHIDManagerRegisterDeviceMatchingCallback"); return; } cb_match[0] = ARG(1); cb_match[1] = ARG(2); }
void I__IOHIDManagerRegisterDeviceRemovalCallback(CPU *c) { if (off()) { unimpl_import(c, "_IOHIDManagerRegisterDeviceRemovalCallback"); return; } cb_gone[0] = ARG(1); cb_gone[1] = ARG(2); }
void I__IOHIDManagerRegisterInputValueCallback(CPU *c) { if (off()) { unimpl_import(c, "_IOHIDManagerRegisterInputValueCallback"); return; } cb_value[0] = ARG(1); cb_value[1] = ARG(2); }
void I__IOHIDManagerScheduleWithRunLoop(CPU *c) {
    if (off()) { unimpl_import(c, "_IOHIDManagerScheduleWithRunLoop"); return; }
    hid_loop = ARG(1); hid_thread = pthread_self(); stop = 0;
}
void I__IOHIDManagerUnscheduleFromRunLoop(CPU *c) { if (off()) unimpl_import(c, "_IOHIDManagerUnscheduleFromRunLoop"); }
void I__IOHIDManagerOpen(CPU *c) { if (off()) { unimpl_import(c, "_IOHIDManagerOpen"); return; } RET(0); }
void I__IOHIDManagerClose(CPU *c) { if (off()) { unimpl_import(c, "_IOHIDManagerClose"); return; } stop = 1; RET(0); }
// The pads there are at this moment: always "none" (no set, as IOKit answers then). Every pad is announced through
// the matching callback from the loop instead, the one way seen to work: a pad handed over in this set was never
// asked for its properties by the game.
void I__IOHIDManagerCopyDevices(CPU *c) {
    if (off()) { unimpl_import(c, "_IOHIDManagerCopyDevices"); return; }
    RET(0);
}
// The set's values as the game's handles. (The bridge hands a host pointer through untouched here.)
void I__CFSetGetValues(CPU *c) {
    CFSetRef set = host(ARG(0));
    if (!set || !ARG(1)) return;
    CFIndex n = CFSetGetCount(set);
    const void **v = calloc((size_t)n + 1, sizeof *v);
    CFSetGetValues(set, v);
    for (CFIndex i = 0; i < n; i++) wr32(ARG(1) + 4 * (uint32_t)i, handle(v[i]));
    free(v);
}
// (device, key) -> a number or a string, not retained for the caller
void I__IOHIDDeviceGetProperty(CPU *c) {
    if (off()) { unimpl_import(c, "_IOHIDDeviceGetProperty"); return; }
    char key[64] = ""; uint32_t k = ARG(1);
    if (k && k < DR_OBJC_HANDLE_BASE) {          // a constant string of the game's: { isa, flags, bytes, length }
        uint32_t bytes = rd32(k + 8), len = rd32(k + 12);
        if (bytes && len < sizeof key) { memcpy(key, G2H(bytes), len); key[len] = 0; }
    } else if (k) CFStringGetCString(host(k), key, sizeof key, kCFStringEncodingUTF8);
    RET(0);
    for (int s = 0; s < NPAD; s++) {
        if (pads[s].devh != ARG(0) || !pads[s].props) continue;
        CFStringRef ks = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
        const void *v = ks ? CFDictionaryGetValue(pads[s].props, ks) : NULL;
        if (ks) CFRelease(ks);
        if (tracing()) fprintf(stderr, "  [pad] the game asks pad %d for \"%s\": %s\n", s + 1, key, v ? "answered" : "nothing");
        // A number goes out as a word pair of the game's memory that only the two shims below read. As a handle it
        // did not come back: small CFNumbers are tagged pointers, and the game then read vendor and product as
        // nothing ("CFGetTypeID with a NULL object") and never took the pad.
        if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
            int32_t n = 0; CFNumberGetValue(v, kCFNumberSInt32Type, &n);
            uint32_t w = aot_malloc(8); wr32(w, PAD_NUM); wr32(w + 4, (uint32_t)n);   // a few per pad, never freed
            RET(w);
        } else RET(handle(v));
    }
}
static int pad_num(uint32_t v) { return v >= 0x1000 && v < DR_OBJC_HANDLE_BASE && !(v & 3) && rd32(v) == PAD_NUM; }
void I__CFGetTypeID(CPU *c) {
    if (pad_num(ARG(0))) { RET(CFNumberGetTypeID()); return; }
    unimpl_import(c, "_CFGetTypeID");
}
// (number, type, value out)
void I__CFNumberGetValue(CPU *c) {
    if (!pad_num(ARG(0))) { unimpl_import(c, "_CFNumberGetValue"); return; }
    uint32_t t = ARG(1), v = rd32(ARG(0) + 4), out = ARG(2);
    if (t == 1 || t == 7) wr8(out, (uint8_t)v);
    else if (t == 2 || t == 8) wr16(out, (uint16_t)v);
    else if (t == 4 || t == 11) { wr32(out, v); wr32(out + 4, (int32_t)v < 0 ? 0xFFFFFFFFu : 0); }
    else wr32(out, v);
    RET(1);
}

// ---- elements and values: words of the game's memory, written by pad_setup and the loop below ----
void I__IOHIDValueGetElement(CPU *c) { RET(ARG(0) ? rd32(ARG(0)) : 0); }
void I__IOHIDValueGetIntegerValue(CPU *c) { RET(ARG(0) ? rd32(ARG(0) + 4) : 0); }
void I__IOHIDValueGetTimeStamp(CPU *c) { c->eax = ARG(0) ? rd32(ARG(0) + 8) : 0; c->edx = ARG(0) ? rd32(ARG(0) + 12) : 0; }
void I__IOHIDElementGetDevice(CPU *c) { RET(ARG(0) ? rd32(ARG(0)) : 0); }
void I__IOHIDElementGetUsagePage(CPU *c) { RET(ARG(0) ? rd32(ARG(0) + 4) : 0); }
void I__IOHIDElementGetUsage(CPU *c) { RET(ARG(0) ? rd32(ARG(0) + 8) : 0); }
void I__IOHIDElementGetCookie(CPU *c) { RET(ARG(0) ? rd32(ARG(0) + 12) : 0); }

// ---- the loop: the game's HID thread sits here ----
void I__CFRunLoopRun(CPU *c) {
    if (off() || !mgr || !pthread_equal(pthread_self(), hid_thread)) { unimpl_import(c, "_CFRunLoopRun"); return; }
    fprintf(stderr, "  [pad] the game's gamepad thread is listening\n");
    for (unsigned tick = 0; !stop; tick++) {
        @autoreleasepool {
            if (tick % 25 == 0) inject_poll();
            if (tick % 125 == 0) {               // twice a second: who came, who left
                unsigned came, went; pads_scan(&came, &went);
                for (int s = 0; s < NPAD; s++) {
                    if (went & (1u << s)) { fprintf(stderr, "  [pad] pad %d left\n", s + 1); call4(cb_gone, pads[s].devh); }
                    if (came & (1u << s)) call4(cb_match, pads[s].devh);
                }
            }
            for (int s = 0; s < NPAD; s++) {
                struct pad *p = &pads[s];
                if (!p->gc) continue;
                int32_t now[NELEM]; pad_read(p, now);
                uint64_t t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                for (int e = 0; e < NELEM; e++) {
                    if (now[e] == p->last[e]) continue;
                    p->last[e] = now[e];
                    wr32(p->value[e] + 4, (uint32_t)now[e]); wr32(p->value[e] + 8, (uint32_t)t); wr32(p->value[e] + 12, (uint32_t)(t >> 32));
                    if (tracing() && e < NBUTTON) fprintf(stderr, "  [pad] pad %d button %d %s\n", s + 1, e + 1, now[e] ? "down" : "up");
                    call4(cb_value, p->value[e]);
                }
            }
        }
        usleep(4000);
    }
}
// the game stops that loop by its run loop; any other run loop is the bridge's business
void I__CFRunLoopStop(CPU *c) {
    if (!off() && hid_loop && ARG(0) == hid_loop) { stop = 1; return; }
    unimpl_import(c, "_CFRunLoopStop");
}

// CFNumberCreate(allocator, type, value pointer): the game builds its matching dictionaries with it, and the value is in
// the game's memory, which the general way does not follow (it answered NULL, and the dictionary then refused the nil).
// The game is 32-bit, so its long, CFIndex and NSInteger are 32 bits and its CGFloat is a float.
void I__CFNumberCreate(CPU *c) {
    uint32_t type = ARG(1), p = ARG(2);
    CFNumberRef n = NULL;
    if (p) switch (type) {
    case 1: case 7: { int8_t v = (int8_t)rd32(p); n = CFNumberCreate(NULL, kCFNumberSInt8Type, &v); break; }
    case 2: case 8: { int16_t v = (int16_t)rd32(p); n = CFNumberCreate(NULL, kCFNumberSInt16Type, &v); break; }
    case 3: case 9: case 10: case 14: case 15: { int32_t v = (int32_t)rd32(p); n = CFNumberCreate(NULL, kCFNumberSInt32Type, &v); break; }
    case 4: case 11: { int64_t v = (int64_t)((uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32); n = CFNumberCreate(NULL, kCFNumberSInt64Type, &v); break; }
    case 5: case 12: case 16: { uint32_t b = rd32(p); float v; memcpy(&v, &b, 4); n = CFNumberCreate(NULL, kCFNumberFloat32Type, &v); break; }
    case 6: case 13: { uint64_t b = (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; double v; memcpy(&v, &b, 8); n = CFNumberCreate(NULL, kCFNumberFloat64Type, &v); break; }
    default: fprintf(stderr, "  [cf] CFNumberCreate: type %u not known, refused\n", type);
    }
    RET(n ? handle(n) : 0);
}

// CFDictionaryGetKeysAndValues(dictionary, keys out, values out): the game lists a pad definition's entries with it
// (Resources/InputDevices/*.plist), and the two arrays are the game's memory, which the general way does not write:
// every pad then got a configuration with no controls in it, and no button did anything.
void I__CFDictionaryGetKeysAndValues(CPU *c) {
    CFDictionaryRef d = host(ARG(0));
    if (!d) return;
    CFIndex n = CFDictionaryGetCount(d);
    const void **k = calloc((size_t)n + 1, sizeof *k), **v = calloc((size_t)n + 1, sizeof *v);
    CFDictionaryGetKeysAndValues(d, k, v);
    for (CFIndex i = 0; i < n; i++) {
        if (ARG(1)) wr32(ARG(1) + 4 * (uint32_t)i, handle(k[i]));
        if (ARG(2)) wr32(ARG(2) + 4 * (uint32_t)i, handle(v[i]));
    }
    free(k); free(v);
}

// CFDictionaryGetCountOfKey(dictionary, key): how the game asks a pad definition "is there a ButtonA?". It had no
// answer and so said 0 to every name: the configuration came out with no controls at all.
void I__CFDictionaryGetCountOfKey(CPU *c) {
    CFDictionaryRef d = host(ARG(0));
    uint32_t k = ARG(1);
    RET(0);
    if (getenv("BMAA_TRACE_PAD")) fprintf(stderr, "  [pad] CountOfKey(0x%x -> %p, 0x%x)\n", ARG(0), (void *)d, k);
    if (!d || !k) return;
    if (k >= DR_OBJC_HANDLE_BASE) { CFTypeRef key = host(k); if (key) RET(CFDictionaryGetCountOfKey(d, key)); return; }
    uint32_t bytes = rd32(k + 8), len = rd32(k + 12);        // a constant string of the game's: { isa, flags, bytes, length }
    if (!bytes || len > 4096) return;
    CFStringRef key = CFStringCreateWithBytes(NULL, G2H(bytes), len, kCFStringEncodingUTF8, false);
    if (key) { RET(CFDictionaryGetCountOfKey(d, key)); CFRelease(key); }
}

// The game also watches the USB bus for the old Xbox 360 driver's devices coming and going (IOKit notifications on
// the pad thread's run loop). Pads reach it through GameController here, so that watch has nothing to see: the port
// is a word of the game's memory, it has no run-loop source, and a notification registers with an empty iterator.
// (Through the general way the port came back as nothing and CFRunLoopAddSource fell over a NULL source.)
static uint32_t note_port;
void I__IONotificationPortCreate(CPU *c) {
    if (off()) { unimpl_import(c, "_IONotificationPortCreate"); return; }
    if (!note_port) note_port = aot_malloc(16);
    RET(note_port);
}
void I__IONotificationPortGetRunLoopSource(CPU *c) {
    if (off()) { unimpl_import(c, "_IONotificationPortGetRunLoopSource"); return; }
    RET(0);
}
// (port, type, matching dictionary (consumed), callback, context, iterator out)
void I__IOServiceAddMatchingNotification(CPU *c) {
    if (off()) { unimpl_import(c, "_IOServiceAddMatchingNotification"); return; }
    if (ARG(5)) wr32(ARG(5), 0);
    RET(0);
}
void I__CFRunLoopAddSource(CPU *c) {
    if (!ARG(1)) return;                                   // no source: nothing to add
    unimpl_import(c, "_CFRunLoopAddSource");
}
void I__IOIteratorNext(CPU *c) {
    if (!ARG(0)) { RET(0); return; }                       // the empty iterator above
    unimpl_import(c, "_IOIteratorNext");
}
