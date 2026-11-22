#include "winframe.h"

#include <QPointer>
#include <QVariant>
#include <QWindow>

#import <AppKit/AppKit.h>

#include <cmath>

namespace {

QPointer<QWindow> g_window;

void applyTitleBar(QWindow *window)
{
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    NSWindow *w = view.window;
    if (!w)
        return;
    // Standard frame and traffic-light buttons, but a transparent title bar
    // with our toolbar drawn underneath it (like Safari / Finder). Main.qml
    // leaves room for the buttons on the toolbar's left.
    w.titlebarAppearsTransparent = YES;
    w.titleVisibility = NSWindowTitleHidden;
    w.styleMask |= NSWindowStyleMaskFullSizeContentView;
}

// --- swipe between pages ------------------------------------------------------------
// Two-finger horizontal swipes (tracked like Safari, with
// -[NSEvent trackSwipeEventWithOptions:...]) and three-finger swipes, as set in
// System Settings > Trackpad > "Swipe between pages". Main.qml decides where a
// swipe may navigate (canSwipe: not over the video, history available) and
// does it (swipeNavigate). dir: 1 = back, -1 = forward.

bool canSwipe(NSEvent *event, int dir)
{
    if (!g_window)
        return false;
    NSView *view = reinterpret_cast<NSView *>(g_window->winId());
    const NSPoint p = [view convertPoint:event.locationInWindow fromView:nil];
    const double y = view.isFlipped ? p.y : view.bounds.size.height - p.y;
    QVariant ok;
    QMetaObject::invokeMethod(g_window.data(), "canSwipe", Q_RETURN_ARG(QVariant, ok), Q_ARG(QVariant, double(p.x)),
                              Q_ARG(QVariant, y), Q_ARG(QVariant, dir));
    return ok.toBool();
}

void navigate(int dir)
{
    if (g_window)
        QMetaObject::invokeMethod(g_window.data(), "swipeNavigate", Q_ARG(QVariant, dir));
}

bool g_candidate = false; // a two-finger scroll began; not yet known whether it is a swipe

NSEvent *handleEvent(NSEvent *event)
{
    if (!g_window)
        return event;
    NSView *view = reinterpret_cast<NSView *>(g_window->winId());
    if (event.window != view.window)
        return event;

    if (event.type == NSEventTypeSwipe) { // three-finger swipe
        const int dir = event.deltaX > 0 ? 1 : event.deltaX < 0 ? -1 : 0;
        if (dir != 0 && canSwipe(event, dir)) {
            navigate(dir);
            return nil;
        }
        return event;
    }

    // two-finger scroll: becomes a swipe when its first movement is horizontal
    if (event.type != NSEventTypeScrollWheel || event.momentumPhase != NSEventPhaseNone ||
        !NSEvent.isSwipeTrackingFromScrollEventsEnabled)
        return event;
    if (event.phase == NSEventPhaseBegan) {
        g_candidate = true;
        return event;
    }
    if (event.phase != NSEventPhaseChanged || !g_candidate)
        return event;
    const CGFloat dx = event.scrollingDeltaX, dy = event.scrollingDeltaY;
    if (dx == 0 && dy == 0)
        return event;
    g_candidate = false;
    if (std::fabs(dx) <= std::fabs(dy))
        return event; // vertical: an ordinary scroll
    const bool back = canSwipe(event, 1), forward = canSwipe(event, -1);
    if (!back && !forward)
        return event;

    __block bool ended = false;
    __block bool navigated = false;
    [event trackSwipeEventWithOptions:NSEventSwipeTrackingLockDirection | NSEventSwipeTrackingClampGestureAmount
             dampenAmountThresholdMin:(forward ? -1 : 0)
                                  max:(back ? 1 : 0)
                         usingHandler:^(CGFloat amount, NSEventPhase phase, BOOL complete, BOOL *) {
                             if (phase == NSEventPhaseEnded)
                                 ended = true;
                             else if (phase == NSEventPhaseCancelled)
                                 ended = false;
                             // positive amount = content moving right = back
                             if (complete && ended && !navigated && amount != 0) {
                                 navigated = true;
                                 navigate(amount > 0 ? 1 : -1);
                             }
                         }];
    return nil;
}

void installSwipeNavigation(QWindow *window)
{
    static id monitor = nil;
    g_window = window;
    if (monitor)
        return;
    monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel | NSEventMaskSwipe
                                                    handler:^NSEvent *(NSEvent *event) { return handleEvent(event); }];
#if !__has_feature(objc_arc)
    [monitor retain];
#endif
}

} // namespace

void installWindowFrame(QWindow *window)
{
    if (!window)
        return;
    applyTitleBar(window);
    // Qt recomputes the style mask on some state changes (e.g. leaving full
    // screen); put the full-size content view back each time.
    QObject::connect(window, &QWindow::windowStateChanged, window, [window] { applyTitleBar(window); });
    QObject::connect(window, &QWindow::visibleChanged, window, [window](bool visible) {
        if (visible)
            applyTitleBar(window);
    });
    installSwipeNavigation(window);
}
