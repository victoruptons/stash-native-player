#include "winframe.h"

#include <QWindow>

#import <AppKit/AppKit.h>

namespace {

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
}
