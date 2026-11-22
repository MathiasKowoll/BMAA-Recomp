// A frame counter on screen (BMAA_FPS=1): the game draws through OpenGL, so the Metal performance HUD has no layer to
// attach to and never shows. This is a small panel of our own at the top right: frames per second and the frame time,
// counted where the game presents a frame (CGLFlushDrawable) and refreshed twice a second from the main queue.
// The same moment hides the bridge's own empty window ("DecompRecomp - ..."), which the game never draws in.
#import <AppKit/AppKit.h>
#include <stdatomic.h>
#include <time.h>

static atomic_uint frames;
static NSPanel *panel; static NSTextField *label;

// The bridge opens that window when the game's nib brings none, well before the first frame: for the half minute
// the game takes to load it sat on screen, empty. It is put away the moment it shows.
void bmaa_mouse_apply(void);   // display.c: the pointer is held only while this application is in front
__attribute__((constructor)) static void hide_bridge_window(void) {
    for (NSNotificationName name in @[NSApplicationDidBecomeActiveNotification, NSApplicationDidResignActiveNotification])
        [[NSNotificationCenter defaultCenter] addObserverForName:name object:nil queue:nil
                                                      usingBlock:^(NSNotification *n) { (void)n; bmaa_mouse_apply(); }];
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidUpdateNotification object:nil queue:nil
        usingBlock:^(NSNotification *n) {
            NSWindow *w = n.object;
            if (w.visible && [w.title hasPrefix:@"DecompRecomp"]) [w orderOut:nil];
        }];
}
static void setup(void) {
    for (NSWindow *w in [NSApp windows]) if ([w.title hasPrefix:@"DecompRecomp"]) [w orderOut:nil];
    const char *on = getenv("BMAA_FPS"); if (!on || !*on || *on == (char)48) return;
    NSRect s = [NSScreen mainScreen].frame;
    panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(NSMaxX(s) - 190, NSMaxY(s) - 70, 170, 30)
                                       styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                         backing:NSBackingStoreBuffered defer:NO];
    panel.opaque = NO; panel.backgroundColor = [NSColor colorWithWhite:0 alpha:0.55]; panel.hasShadow = NO;
    panel.ignoresMouseEvents = YES; panel.level = NSScreenSaverWindowLevel; panel.hidesOnDeactivate = NO;
    panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary |
                               NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;
    label = [NSTextField labelWithString:@"-- fps"];
    label.frame = NSMakeRect(8, 4, 154, 22); label.textColor = [NSColor greenColor];
    label.font = [NSFont monospacedDigitSystemFontOfSize:15 weight:NSFontWeightSemibold];
    [panel.contentView addSubview:label];
    [panel orderFrontRegardless];
    static dispatch_source_t t;
    t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), NSEC_PER_SEC / 2, NSEC_PER_MSEC * 20);
    __block uint64_t last = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    dispatch_source_set_event_handler(t, ^{
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        unsigned n = atomic_exchange(&frames, 0);
        double dt = (double)(now - last) / 1e9; last = now;
        double fps = dt > 0 ? n / dt : 0;
        label.stringValue = n ? [NSString stringWithFormat:@"%.0f fps  %.1f ms", fps, 1000.0 / fps] : @"0 fps";
        if (!panel.isVisible && NSApp.isActive) [panel orderFrontRegardless];
    });
    dispatch_resume(t);
}
// called by the frame-present shim, on whatever thread the game presents from
void fps_frame(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{ dispatch_async(dispatch_get_main_queue(), ^{ setup(); }); });
    atomic_fetch_add(&frames, 1);
}
