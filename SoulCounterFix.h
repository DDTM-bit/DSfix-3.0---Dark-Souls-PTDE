#pragma once
#include <windows.h>

// The soul counter rolls up at the 30 FPS speed at any frame rate (see SoulCounterFix.cpp).
// No setting: installed from FPS.cpp whenever unlockFPS is 1 and the FPS limit is above 30.
bool installSoulCounterFix();
