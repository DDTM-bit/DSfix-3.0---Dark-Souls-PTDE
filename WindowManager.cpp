
#include "WindowManager.h"
#include "main.h"
#include "Settings.h"
#include <cstring>

WindowManager WindowManager::instance;

// DSfix 2.x called ClipCursor on every frame (ClipCursor(NULL) when the cursor is not captured). Input mods such as
// Dark Souls Input Customizer hook ClipCursor and use that call as their per-frame update: switching between camera
// and menu cursor, hiding and capturing the cursor. DSfix 3.0 only calls ClipCursor when it clips the cursor itself
// (so other programs' clipping is left alone), which starved those mods: mouse camera dead, left click always the
// attack button. So when another mod has hooked ClipCursor (its first bytes differ from the ones remembered at
// startup), DSfix calls it on every frame again, exactly like 2.x. Without such a mod nothing changes.
namespace {
	BYTE  g_clipOrig[8] = {};
	BYTE* g_clipAddr = NULL;
	bool  g_clipHookLogged = false;

	bool clipCursorHookedByMod() {
		if (!g_clipAddr) return false;
		const bool hooked = memcmp(g_clipAddr, g_clipOrig, sizeof(g_clipOrig)) != 0;
		if (hooked && !g_clipHookLogged) {
			g_clipHookLogged = true;
			SDLOG(0, "WindowManager: ClipCursor is hooked by another mod (e.g. Dark Souls Input Customizer) - calling it every frame as DSfix 2.x did\n");
		}
		return hooked;
	}
}

void WindowManager::rememberClipCursor() {
	HMODULE u32 = ::GetModuleHandleA("user32.dll");
	if (!u32) u32 = ::LoadLibraryA("user32.dll");
	BYTE* p = u32 ? (BYTE*)::GetProcAddress(u32, "ClipCursor") : NULL;
	if (!p) return;
	memcpy(g_clipOrig, p, sizeof(g_clipOrig));
	g_clipAddr = p;
}

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
	else if (clipCursorHookedByMod()) {
		::ClipCursor(NULL); // feeds the input mod's per-frame update (see above)
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


