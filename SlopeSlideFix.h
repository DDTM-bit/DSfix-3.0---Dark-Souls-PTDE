#pragma once
#include <windows.h>

// Steep slope slides start reliably at any frame rate (see SlopeSlideFix.cpp).
// No setting: installed from FPS.cpp whenever unlockFPS is 1 and the FPS limit is above 30.
bool installSlopeSlideFix();
