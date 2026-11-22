#pragma once

class QWindow;

// Windows only (a no-op elsewhere): gives a Qt::FramelessWindowHint window back
// the native "resizable, maximizable" window styles (WS_THICKFRAME,
// WS_MAXIMIZEBOX, ...) while still drawing no native frame.
//
// Without those styles Windows Snap and PowerToys FancyZones treat the window
// as a popup and refuse to snap it when it is dragged by our own title bar.
void installSnapFrame(QWindow *window);
