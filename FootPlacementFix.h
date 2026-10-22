#pragma once
#include <windows.h>

// Foot placement on stairs and slopes (see FootPlacementFix.cpp).
// No setting: installed from FPS.cpp whenever unlockFPS is 1 and the FPS limit is above 30.
bool installFootPlacementFix();

// Called once per frame with the step DSfix just wrote into the engine (seconds).
// Does nothing if the fix was not installed.
void updateFootPlacementFix(float deltaSeconds);
