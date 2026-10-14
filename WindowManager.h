
#pragma once

#include <Windows.h>

class WindowManager {
	static WindowManager instance;
	
	bool captureCursor, cursorVisible;
	bool cursorClipped; // true while DSfix holds a ClipCursor rectangle

	bool borderlessFullscreen;
	RECT prevWindowRect;
	long prevStyle, prevExStyle;

public:
	static WindowManager& get() {
		return instance;
	}

	WindowManager() : captureCursor(false), cursorVisible(true), cursorClipped(false), borderlessFullscreen(false) { }
	void applyCursorCapture();
	void toggleCursorCapture();
	void toggleCursorVisibility();
	void toggleBorderlessFullscreen();
	void resize(unsigned clientW, unsigned clientH);

	// DSfix 3.0: remembers the start of user32!ClipCursor before other mods are loaded (called from DllMain)
	static void rememberClipCursor();
};


