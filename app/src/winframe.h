#pragma once

class QWindow;

// Platform window frame for the main window, whose toolbar doubles as the
// title bar.
//
// Windows (winframe.cpp): gives the Qt::FramelessWindowHint window back the
// native "resizable, maximizable" window styles (WS_THICKFRAME,
// WS_MAXIMIZEBOX, ...) while still drawing no native frame. Without those
// styles Windows Snap and PowerToys FancyZones treat the window as a popup
// and refuse to snap it when it is dragged by our own title bar.
//
// macOS (winframe_mac.mm): keeps the standard frame and traffic-light
// buttons, with a transparent title bar over the toolbar.
//
// Elsewhere: a no-op.
void installWindowFrame(QWindow *window);
