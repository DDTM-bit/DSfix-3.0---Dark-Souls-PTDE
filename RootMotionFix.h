#pragma once
#include <windows.h>

// Animation root motion at any frame rate (see RootMotionFix.cpp).
// No setting: installed from FPS.cpp whenever unlockFPS is 1 and the FPS limit is above 30.
bool installRootMotionFix();

// Called once per frame with the step DSfix just wrote into the engine (seconds).
// Does nothing if the fix was not installed.
void updateRootMotionFix(float deltaSeconds);
