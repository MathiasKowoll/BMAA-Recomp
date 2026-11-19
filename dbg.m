// kill -USR2 <pid>: the process's windows and their view trees (class, frame, hidden), printed from the main queue.
// For looking at what the game put on screen without being in front of it.
#import <AppKit/AppKit.h>
#include <signal.h>
#include <stdio.h>

@interface NSView (DRDebug)
- (NSString *)_subtreeDescription;
@end

void dbg_init(void) {
    signal(SIGUSR2, SIG_IGN);
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR2, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(src, ^{
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
                if (getenv("DBG_JS"))
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
}
