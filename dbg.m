// kill -USR2 <pid>: the process's windows and their view trees (class, frame, hidden), printed from the main queue.
// For looking at what the game put on screen without being in front of it.
#import <AppKit/AppKit.h>
#include <signal.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <stdio.h>

@interface NSView (DRDebug)
- (NSString *)_subtreeDescription;
@end

void dbg_init(void) {
    signal(SIGUSR2, SIG_IGN);
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR2, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(src, ^{
        // DBG_CLICK=x,y: a left click at that point of the launcher window's content (points, from its top left),
        // delivered to the application as the system would, inside this process (nothing is posted to the desktop)
        if (getenv("DBG_CLICK")) {
            double x = 0, y = 0; sscanf(getenv("DBG_CLICK"), "%lf,%lf", &x, &y);
            for (NSWindow *w in [NSApp windows]) {
                if (![w.title containsString:@"Options"]) continue;
                {   // who answers the events of this window
                    for (Class k = object_getClass(w); k; k = class_getSuperclass(k)) {
                        Method m = class_getInstanceMethod(k, @selector(sendEvent:));
                        Dl_info di; IMP imp = m ? method_getImplementation(m) : NULL;
                        if (imp && dladdr((void *)imp, &di))
                            fprintf(stderr, "  [click] %s -sendEvent: is %s\n", class_getName(k), di.dli_sname ?: "?");
                        if (!strcmp(class_getName(k), "NSWindow")) break;
                    }
                }
                // a first click on a window that is not key only makes it key (a web view does not take the first
                // mouse): make it key first, as a person's first click would have
                if (!w.isKeyWindow) [w makeKeyWindow];
                NSPoint pt = NSMakePoint(x, w.contentView.bounds.size.height - y);
                for (int k = 0; k < 2; k++) {
                    NSEvent *e = [NSEvent mouseEventWithType:k ? NSEventTypeLeftMouseUp : NSEventTypeLeftMouseDown location:pt
                                               modifierFlags:0 timestamp:[[NSProcessInfo processInfo] systemUptime]
                                                windowNumber:w.windowNumber context:nil eventNumber:k clickCount:1 pressure:k ? 0 : 1];
                    fprintf(stderr, "  [click] %s at %.0f,%.0f in \"%s\"\n", k ? "up" : "down", pt.x, pt.y, w.title.UTF8String);
                    [NSApp sendEvent:e];
                }
            }
        }
        // DBG_GAME_CLICK=x,y: once the launcher is gone, a left click at that point (points from the top left) of every
        // other visible window with a view, again inside this process only
        if (getenv("DBG_GAME_CLICK")) {
            double x = 0, y = 0; sscanf(getenv("DBG_GAME_CLICK"), "%lf,%lf", &x, &y);
            BOOL launcher = NO;
            for (NSWindow *w in [NSApp windows]) if (w.isVisible && [w.title containsString:@"Options"]) launcher = YES;
            for (NSWindow *w in [NSApp windows]) {
                if (launcher || !w.isVisible || !w.contentView || [w isKindOfClass:[NSPanel class]]) continue;
                if (!w.isKeyWindow) [w makeKeyWindow];
                NSPoint pt = NSMakePoint(x, w.contentView.bounds.size.height - y);
                for (int k = 0; k < 2; k++) {
                    NSEvent *e = [NSEvent mouseEventWithType:k ? NSEventTypeLeftMouseUp : NSEventTypeLeftMouseDown location:pt
                                               modifierFlags:0 timestamp:[[NSProcessInfo processInfo] systemUptime]
                                                windowNumber:w.windowNumber context:nil eventNumber:k clickCount:1 pressure:k ? 0 : 1];
                    fprintf(stderr, "  [click] %s at %.0f,%.0f in \"%s\" (%s)\n", k ? "up" : "down", pt.x, pt.y, w.title.UTF8String, object_getClassName(w));
                    [NSApp sendEvent:e];
                }
            }
        }
        for (NSWindow *w in [NSApp windows]) {
            fprintf(stderr, "  [views] window \"%s\" %s frame %s visible %d key %d alpha %.2f opaque %d\n", w.title.UTF8String,
                    object_getClassName(w), NSStringFromRect(w.frame).UTF8String, w.isVisible, w.isKeyWindow, w.alphaValue, w.isOpaque);
            NSView *cv = w.contentView;
            if ([cv respondsToSelector:@selector(_subtreeDescription)] && getenv("DBG_VIEWS"))
                fprintf(stderr, "%s\n", [cv _subtreeDescription].UTF8String);
            // a web view in it: what its page says about itself
            NSMutableArray *todo = [NSMutableArray arrayWithObject:cv ?: (NSView *)NSNull.null];
            while (todo.count) {
                NSView *v = todo.lastObject; [todo removeLastObject];
                if (![v isKindOfClass:[NSView class]]) continue;
                [todo addObjectsFromArray:v.subviews];
                if (![v respondsToSelector:@selector(stringByEvaluatingJavaScriptFromString:)]) continue;
                static NSString *const ask[] = {
                    @"document.readyState", @"document.title", @"location.href",
                    @"document.getElementsByTagName('*').length",
                    @"getComputedStyle(document.body).backgroundColor + ' / display ' + getComputedStyle(document.body).display + ' / visibility ' + getComputedStyle(document.body).visibility + ' / opacity ' + getComputedStyle(document.body).opacity",
                    @"document.body.innerText.replace(/\\s+/g,' ').slice(0,600)",
                    @"Array.prototype.slice.call(document.body.children).map(function(e){var s=getComputedStyle(e);return e.tagName+'#'+e.id+'.'+e.className+' '+e.offsetWidth+'x'+e.offsetHeight+' disp='+s.display+' vis='+s.visibility+' op='+s.opacity}).join(' | ')",
                };
                // DBG_JS: something to run in the page first (to look past an animation a locked screen holds still)
                static int js_done;   // the script once: a later report must not press the button again
                if (getenv("DBG_JS") && !js_done++)
                    fprintf(stderr, "  [page] DBG_JS => %s\n",
                            [[(id)v performSelector:@selector(stringByEvaluatingJavaScriptFromString:)
                                         withObject:[NSString stringWithUTF8String:getenv("DBG_JS")]] description].UTF8String);
                for (unsigned k = 0; k < sizeof ask / sizeof ask[0]; k++)
                    fprintf(stderr, "  [page] %s => %s\n", ask[k].UTF8String,
                            [[(id)v performSelector:@selector(stringByEvaluatingJavaScriptFromString:) withObject:ask[k]] description].UTF8String);
            }
        }
    });
    dispatch_resume(src);
    // BMAA_AUTOPLAY=1: press the launcher's PLAY as soon as its page has the button (once), so the game starts without
    // a click on the options window. Looked for once a second from the main queue.
    if (getenv("BMAA_AUTOPLAY")) {
        static dispatch_source_t t;
        t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
        dispatch_source_set_event_handler(t, ^{
            for (NSWindow *w in [NSApp windows]) {
                NSMutableArray *todo = [NSMutableArray arrayWithObject:w.contentView ?: (NSView *)NSNull.null];
                while (todo.count) {
                    NSView *v = todo.lastObject; [todo removeLastObject];
                    if (![v isKindOfClass:[NSView class]]) continue;
                    [todo addObjectsFromArray:v.subviews];
                    if (![v respondsToSelector:@selector(stringByEvaluatingJavaScriptFromString:)]) continue;
                    NSString *r = [(id)v performSelector:@selector(stringByEvaluatingJavaScriptFromString:) withObject:
                        @"(function(){var b=document.getElementById('main-playButton-0'); if(!b||document.readyState!='complete') return '';"
                         "window.__n=(window.__n||0)+1; if(window.__n%5!=4) return 'ok';"
                         "var o=document.getElementById('loadingOverlay'); if(o) o.style.display='none'; b.click(); return 'ok';})()"];
                    // the page takes the press only once its own scripts have settled: wait three seconds after it is
                    // first seen ready, and press again every five while the launcher is still up
                    static int seen;
                    if (![r isEqual:@"ok"] || !w.isVisible) continue;
                    if (seen++ % 5 == 3) fprintf(stderr, "  [page] PLAY pressed for the user (BMAA_AUTOPLAY)\n");
                    return;
                }
            }
        });
        dispatch_resume(t);
    }
}
