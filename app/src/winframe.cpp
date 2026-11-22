#include "winframe.h"

#include <QWindow>

#ifdef Q_OS_WIN
#include <QAbstractNativeEventFilter>
#include <QGuiApplication>
#include <QPointer>
#include <qt_windows.h>
#include <shellapi.h>

namespace {

constexpr LONG_PTR kSnapStyles = WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU;

void applyStyles(HWND hwnd)
{
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & kSnapStyles) == kSnapStyles)
        return;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style | kSnapStyles);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// The rectangle a maximized window should fill: the monitor's work area (the
// taskbar stays visible), minus one pixel on the edge of an auto-hidden
// taskbar so that it can still pop up.
bool maximizedRect(HWND hwnd, RECT *work, RECT *monitor)
{
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
        return false;
    *work = mi.rcWork;
    *monitor = mi.rcMonitor;
    if (EqualRect(work, monitor)) {
        APPBARDATA abd{};
        abd.cbSize = sizeof(abd);
        abd.rc = mi.rcMonitor; // the monitor to ask about
        for (UINT edge : {ABE_BOTTOM, ABE_TOP, ABE_LEFT, ABE_RIGHT}) {
            abd.uEdge = edge;
            if (SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &abd)) {
                if (edge == ABE_BOTTOM)
                    work->bottom -= 1;
                else if (edge == ABE_TOP)
                    work->top += 1;
                else if (edge == ABE_LEFT)
                    work->left += 1;
                else
                    work->right -= 1;
                break;
            }
        }
    }
    return true;
}

class SnapFrameFilter : public QAbstractNativeEventFilter
{
public:
    SnapFrameFilter(HWND hwnd, QWindow *window) : m_hwnd(hwnd), m_window(window) {}

    bool nativeEventFilter(const QByteArray &, void *message, qintptr *result) override
    {
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg->hwnd != m_hwnd)
            return false;

        if (msg->message == WM_NCCALCSIZE && msg->wParam) {
            // The whole window is client area: no native frame or caption.
            *result = 0;
            return true;
        }

        RECT work, monitor;
        if (msg->message == WM_GETMINMAXINFO) {
            // A resizable window is normally maximized with its frame hanging
            // off the screen edges. Maximize to exactly the work area instead.
            // Handled fully here, so apply the window's min/max size too.
            auto *mmi = reinterpret_cast<MINMAXINFO *>(msg->lParam);
            if (maximizedRect(m_hwnd, &work, &monitor)) {
                mmi->ptMaxPosition.x = work.left - monitor.left;
                mmi->ptMaxPosition.y = work.top - monitor.top;
                mmi->ptMaxSize.x = work.right - work.left;
                mmi->ptMaxSize.y = work.bottom - work.top;
            }
            if (m_window) {
                const qreal dpr = m_window->devicePixelRatio();
                const QSize minSize = m_window->minimumSize() * dpr;
                const QSize maxSize = m_window->maximumSize();
                mmi->ptMinTrackSize.x = qMax<LONG>(mmi->ptMinTrackSize.x, minSize.width());
                mmi->ptMinTrackSize.y = qMax<LONG>(mmi->ptMinTrackSize.y, minSize.height());
                if (maxSize.width() < 16777215) // QWINDOWSIZE_MAX: no maximum
                    mmi->ptMaxTrackSize.x = LONG(maxSize.width() * dpr);
                if (maxSize.height() < 16777215)
                    mmi->ptMaxTrackSize.y = LONG(maxSize.height() * dpr);
            }
            *result = 0;
            return true;
        }

        if (msg->message == WM_WINDOWPOSCHANGING && IsZoomed(m_hwnd) &&
            !(m_window && m_window->visibility() == QWindow::FullScreen)) {
            // Right after maximizing, Qt resizes a frameless window to cover the
            // whole monitor, hiding the taskbar (it looks like full screen).
            // While maximized, keep the window on the work area.
            auto *wp = reinterpret_cast<WINDOWPOS *>(msg->lParam);
            if (!(wp->flags & SWP_NOSIZE) && maximizedRect(m_hwnd, &work, &monitor)) {
                wp->x = work.left;
                wp->y = work.top;
                wp->cx = work.right - work.left;
                wp->cy = work.bottom - work.top;
                wp->flags &= ~UINT(SWP_NOMOVE);
            }
            return false;
        }
        return false;
    }

private:
    HWND m_hwnd;
    QPointer<QWindow> m_window;
};

} // namespace

void installSnapFrame(QWindow *window)
{
    if (!window || qEnvironmentVariableIsSet("SNP_NO_SNAP_FRAME"))
        return;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    qApp->installNativeEventFilter(new SnapFrameFilter(hwnd, window));
    applyStyles(hwnd);
    // Qt rewrites the style when entering/leaving full screen; put ours back.
    QObject::connect(window, &QWindow::visibilityChanged, window, [hwnd](QWindow::Visibility v) {
        if (v != QWindow::FullScreen && v != QWindow::Hidden)
            applyStyles(hwnd);
        // Coming back from full screen, Qt restores a maximized window at the
        // full monitor size (while it still counts as full screen, so the
        // clamp in the filter doesn't apply); fit it to the work area again.
        RECT work, monitor;
        if (v == QWindow::Maximized && maximizedRect(hwnd, &work, &monitor))
            SetWindowPos(hwnd, nullptr, work.left, work.top, work.right - work.left, work.bottom - work.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    });
}

#else

void installSnapFrame(QWindow *) {}

#endif
