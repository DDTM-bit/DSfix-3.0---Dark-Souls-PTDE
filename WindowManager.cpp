#include "WindowManager.h"

WindowManager WindowManager::instance;

void WindowManager::applyCursorCapture() {
	// DSfix 3.0: only hold the cursor while the game window is actually in front. After Alt+Tab the cursor is released
	// (it used to be clipped to an undefined rectangle because the window lookup failed and was not checked), and
	// ClipCursor(NULL) is only called when DSfix itself clipped, so other programs' cursor clipping is left alone.
	if(captureCursor) {
		HWND fg = ::GetForegroundWindow();
		DWORD fgPid = 0;
		if (fg) ::GetWindowThreadProcessId(fg, &fgPid);
		if (fg && fgPid == ::GetCurrentProcessId()) {
			HWND hwnd = ::GetActiveWindow();
			if (!hwnd) hwnd = fg;
			RECT clientrect;
			if (::GetClientRect(hwnd, &clientrect) && clientrect.right > clientrect.left && clientrect.bottom > clientrect.top) {
				POINT topLeft = { clientrect.left, clientrect.top };
				POINT bottomRight = { clientrect.right, clientrect.bottom };
				if (::ClientToScreen(hwnd, &topLeft) && ::ClientToScreen(hwnd, &bottomRight)) {
					RECT screenRect = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
					if (::ClipCursor(&screenRect)) {
						cursorClipped = true;
						return;
					}
				}
			}
		}
	}
	if (cursorClipped) {
		::ClipCursor(NULL);
		cursorClipped = false;
	}
}

void WindowManager::toggleCursorCapture() {
	captureCursor = !captureCursor;
}

void WindowManager::toggleCursorVisibility() {	
	cursorVisible = !cursorVisible;
	::ShowCursor(cursorVisible);
}

void WindowManager::toggleBorderlessFullscreen() {
	borderlessFullscreen = !borderlessFullscreen;
	HWND hwnd = ::GetActiveWindow();
	if(borderlessFullscreen) {
		// store previous rect
		::GetClientRect(hwnd, &prevWindowRect);
		// set styles
		LONG lStyle = ::GetWindowLong(hwnd, GWL_STYLE);
		prevStyle = lStyle;
		lStyle &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE | WS_SYSMENU);
		::SetWindowLong(hwnd, GWL_STYLE, lStyle);
		LONG lExStyle = ::GetWindowLong(hwnd, GWL_EXSTYLE);
		prevExStyle = lExStyle;
		lExStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
		::SetWindowLong(hwnd, GWL_EXSTYLE, lExStyle);
		// adjust size & position
		HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO info;
		info.cbSize = sizeof(MONITORINFO);
		GetMonitorInfo(monitor, &info);
		int monitorWidth = info.rcMonitor.right - info.rcMonitor.left;
		int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;
		::SetWindowPos(hwnd, NULL, info.rcMonitor.left, info.rcMonitor.top, monitorWidth, monitorHeight, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
	} else {
		// restore previous window
		::SetWindowLong(hwnd, GWL_STYLE, prevStyle);
		::SetWindowLong(hwnd, GWL_EXSTYLE, prevExStyle);
		RECT desiredRect = prevWindowRect;
		::AdjustWindowRect(&desiredRect, prevStyle, false);
		int wWidth = desiredRect.right - desiredRect.left, wHeight = desiredRect.bottom - desiredRect.top;
		::SetWindowPos(hwnd, NULL, prevWindowRect.left, prevWindowRect.top, wWidth, wHeight, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
	}
}

void WindowManager::resize(unsigned clientW, unsigned clientH) {
	HWND hwnd = ::GetActiveWindow();
	// Store current window rect
	::GetClientRect(hwnd, &prevWindowRect);
	// Get monitor size
	HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO info;
	info.cbSize = sizeof(MONITORINFO);
	GetMonitorInfo(monitor, &info);
	int monitorWidth = info.rcMonitor.right - info.rcMonitor.left;
	int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;

	// How much do we overlap or are smaller than the actual screen size
	int widthDiff = monitorWidth - (clientW ? clientW : prevWindowRect.right);
	int heightDiff = monitorHeight - (clientH ? clientH : prevWindowRect.bottom);

 	RECT desiredRect;
	desiredRect.left = widthDiff / 2;
	desiredRect.top = heightDiff / 2;
	desiredRect.right = monitorWidth - (widthDiff / 2);
	desiredRect.bottom = monitorHeight - (heightDiff / 2);
 	LONG lStyle = ::GetWindowLong(hwnd, GWL_STYLE);
 	::AdjustWindowRect(&desiredRect, lStyle, false);
	::SetWindowPos(hwnd, NULL, desiredRect.left, desiredRect.top, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
}
