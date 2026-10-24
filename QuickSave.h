#pragma once

#include <d3d9.h>

// Quick save and quick load (see QuickSave.cpp). Settings quickSave, quickSaveKey and quickLoadKey in DSfix.ini; off by
// default.

// Called once at start-up, after the settings are read: checks this game build and reads the keys.
void quickSaveInit();

// Called by RSManager right before every Present: reads the keys, follows a running quick save or quick load and draws
// their messages.
void quickSaveFrame(IDirect3DDevice9* dev);

// Called when the device releases its resources (Reset, Alt+Tab, shutdown).
void quickSaveRelease();
