// CoreText fonts and the CoreGraphics text and path calls that take or return floating point and structs, answered
// natively. The bridge's table of signatures passes integers and handles; these take CGFloat (a float in the game,
// a double here), CGRect and CGAffineTransform by value, and three of them return a struct through a hidden pointer
// that the callee pops. Left to the bridge they answered nothing and left the game's stack one slot off: the engine
// measures a string with CTFontGetBoundingRectsForGlyphs as it creates its window, and the frame that called it
// then popped its registers from the wrong slots (two calls through garbage pointers, and the engine thread gone).
#include "dr_bridge.h"
#include "dr_objc.h"
#include "rt.h"
#include <CoreText/CoreText.h>
#include <CoreGraphics/CoreGraphics.h>

void aot_lock(void);
void aot_unlock(void);
dr_objc *dr_objc_actual(void);

#define RET(v) (c->eax = (uint32_t)(v))

// A host object for what the game holds: a handle of the bridge's, or one of its own constants with a host twin.
static void *host(uint32_t v) {
    if (!v) return NULL;
    dr_objc *o = dr_objc_actual();
    aot_lock();
    void *h = dr_objc_object_for(o, v);
    if (!h && v < 0xE0000000u) h = dr_objc_host_twin(o, v);
    aot_unlock();
    return h;
}
static uint32_t handle(const void *h) {
    if (!h) return 0;
    aot_lock();
    uint32_t v = dr_objc_handle_for(dr_objc_actual(), (void *)h);
    aot_unlock();
    return v;
}
static float F(CPU *c, int i) { return rdf(c->esp + 4 + 4 * i); }
static void rect_out(uint32_t g, CGRect r) {
    wrf(g, (float)r.origin.x); wrf(g + 4, (float)r.origin.y); wrf(g + 8, (float)r.size.width); wrf(g + 12, (float)r.size.height);
}
// A CGAffineTransform of the game's: six floats. NULL stays NULL.
static CGAffineTransform *matrix(uint32_t g, CGAffineTransform *m) {
    if (!g) return NULL;
    *m = CGAffineTransformMake(rdf(g), rdf(g + 4), rdf(g + 8), rdf(g + 12), rdf(g + 16), rdf(g + 20));
    return m;
}

// ---- fonts ----
void I__CTFontCreateWithName(CPU *c) {
    CGAffineTransform m;
    RET(handle(CTFontCreateWithName(host(ARG(0)), F(c, 1), matrix(ARG(2), &m))));
}
void I__CTFontCreateUIFontForLanguage(CPU *c) { RET(handle(CTFontCreateUIFontForLanguage(ARG(0), F(c, 1), host(ARG(2))))); }
void I__CTFontCreateCopyWithSymbolicTraits(CPU *c) {
    CGAffineTransform m;
    RET(handle(CTFontCreateCopyWithSymbolicTraits(host(ARG(0)), F(c, 1), matrix(ARG(2), &m), ARG(3), ARG(4))));
}
void I__CTFontGetSize(CPU *c)    { FPUSH(CTFontGetSize(host(ARG(0)))); }
void I__CTFontGetAscent(CPU *c)  { FPUSH(CTFontGetAscent(host(ARG(0)))); }
void I__CTFontGetDescent(CPU *c) { FPUSH(CTFontGetDescent(host(ARG(0)))); }
void I__CTFontGetLeading(CPU *c) { FPUSH(CTFontGetLeading(host(ARG(0)))); }
void I__CTFontGetGlyphsForCharacters(CPU *c) {
    RET(CTFontGetGlyphsForCharacters(host(ARG(0)), G2H(ARG(1)), G2H(ARG(2)), (CFIndex)(int32_t)ARG(3)) ? 1 : 0);
}
// CGRect f(font): the hidden pointer comes first and the callee pops it.
void I__CTFontGetBoundingBox(CPU *c) {
    uint32_t out = ARG(0);
    rect_out(out, CTFontGetBoundingBox(host(ARG(1))));
    RET(out); c->esp += 4;
}
// CGRect f(font, orientation, glyphs, CGRect *each, count)
void I__CTFontGetBoundingRectsForGlyphs(CPU *c) {
    uint32_t out = ARG(0), each = ARG(4);
    CFIndex n = (CFIndex)(int32_t)ARG(5);
    CGRect *tmp = each && n > 0 ? calloc((size_t)n, sizeof *tmp) : NULL;
    CGRect all = CTFontGetBoundingRectsForGlyphs(host(ARG(1)), ARG(2), G2H(ARG(3)), tmp, n);
    for (CFIndex k = 0; tmp && k < n; k++) rect_out(each + 16u * (uint32_t)k, tmp[k]);
    free(tmp);
    rect_out(out, all);
    RET(out); c->esp += 4;
}
// double f(font, orientation, glyphs, CGSize *each, count)
void I__CTFontGetAdvancesForGlyphs(CPU *c) {
    uint32_t each = ARG(3);
    CFIndex n = (CFIndex)(int32_t)ARG(4);
    CGSize *tmp = each && n > 0 ? calloc((size_t)n, sizeof *tmp) : NULL;
    double all = CTFontGetAdvancesForGlyphs(host(ARG(0)), ARG(1), G2H(ARG(2)), tmp, n);
    for (CFIndex k = 0; tmp && k < n; k++) { wrf(each + 8u * (uint32_t)k, (float)tmp[k].width); wrf(each + 8u * (uint32_t)k + 4, (float)tmp[k].height); }
    free(tmp);
    FPUSH(all);
}
void I__CTFontCopyGraphicsFont(CPU *c) {
    CTFontDescriptorRef d = NULL;
    RET(handle(CTFontCopyGraphicsFont(host(ARG(0)), ARG(1) ? &d : NULL)));
    if (ARG(1)) wr32(ARG(1), handle(d));
}
void I__CTFontCreatePathForGlyph(CPU *c) {
    CGAffineTransform m;
    RET(handle(CTFontCreatePathForGlyph(host(ARG(0)), (CGGlyph)ARG(1), matrix(ARG(2), &m))));
}
void I__CTLineGetImageBounds(CPU *c) {
    uint32_t out = ARG(0);
    rect_out(out, CTLineGetImageBounds(host(ARG(1)), host(ARG(2))));
    RET(out); c->esp += 4;
}

// ---- CoreGraphics text and paths ----
void I__CGAffineTransformMakeScale(CPU *c) {
    uint32_t out = ARG(0);
    CGAffineTransform m = CGAffineTransformMakeScale(F(c, 1), F(c, 2));
    wrf(out, (float)m.a); wrf(out + 4, (float)m.b); wrf(out + 8, (float)m.c); wrf(out + 12, (float)m.d); wrf(out + 16, (float)m.tx); wrf(out + 20, (float)m.ty);
    RET(out); c->esp += 4;
}
void I__CGContextSetTextPosition(CPU *c) { CGContextRef x = host(ARG(0)); if (x) CGContextSetTextPosition(x, F(c, 1), F(c, 2)); }
// CGPoint: eight bytes, back in eax:edx.
void I__CGContextGetTextPosition(CPU *c) {
    CGContextRef x = host(ARG(0));
    CGPoint p = x ? CGContextGetTextPosition(x) : CGPointZero;
    float fx = (float)p.x, fy = (float)p.y;
    memcpy(&c->eax, &fx, 4); memcpy(&c->edx, &fy, 4);
}
void I__CGContextSetTextMatrix(CPU *c) {
    CGContextRef x = host(ARG(0));
    if (x) CGContextSetTextMatrix(x, CGAffineTransformMake(F(c, 1), F(c, 2), F(c, 3), F(c, 4), F(c, 5), F(c, 6)));
}
void I__CGPathMoveToPoint(CPU *c)    { CGAffineTransform m; CGMutablePathRef p = host(ARG(0)); if (p) CGPathMoveToPoint(p, matrix(ARG(1), &m), F(c, 2), F(c, 3)); }
void I__CGPathAddLineToPoint(CPU *c) { CGAffineTransform m; CGMutablePathRef p = host(ARG(0)); if (p) CGPathAddLineToPoint(p, matrix(ARG(1), &m), F(c, 2), F(c, 3)); }
void I__CGPathAddArcToPoint(CPU *c) {
    CGAffineTransform m; CGMutablePathRef p = host(ARG(0));
    if (p) CGPathAddArcToPoint(p, matrix(ARG(1), &m), F(c, 2), F(c, 3), F(c, 4), F(c, 5), F(c, 6));
}

// ---- CFUUID ----
// CFUUIDGetUUIDBytes returns sixteen bytes by value (hidden pointer, popped by the callee); the bridge's table
// refused CFUUIDCreate for its NULL allocator. The engine makes a GUID this way for every package it creates.
void I__CFUUIDCreate(CPU *c) { RET(handle(CFUUIDCreate(NULL))); }
void I__CFUUIDGetUUIDBytes(CPU *c) {
    uint32_t out = ARG(0);
    CFUUIDRef u = host(ARG(1));
    CFUUIDBytes b = {0};
    if (u) b = CFUUIDGetUUIDBytes(u);
    memcpy(G2H(out), &b, 16);
    RET(out); c->esp += 4;
}

// ---- CoreFoundation calls the bridge's generic route cannot take (a struct argument, a pointer to fill) ----
// (CFMutableStringRef, find, replace, CFRange by value: two slots, options) -> how many replaced
void I__CFStringFindAndReplace(CPU *c) {
    CFMutableStringRef s = host(ARG(0)); CFStringRef f = host(ARG(1)), r = host(ARG(2));
    if (!s || !f || !r) { c->eax = 0; return; }
    c->eax = (uint32_t)CFStringFindAndReplace(s, f, r, CFRangeMake((int32_t)ARG(3), (int32_t)ARG(4)), ARG(5));
}
// (allocator, CFDataRef xml, options, CFStringRef *error) -> the property list
void I__CFPropertyListCreateFromXMLData(CPU *c) {
    CFDataRef d = host(ARG(1));
    CFPropertyListRef p = d ? CFPropertyListCreateWithData(NULL, d, ARG(2), NULL, NULL) : NULL;
    if (ARG(3)) wr32(ARG(3), 0);
    c->eax = handle(p);
}
