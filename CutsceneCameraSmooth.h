#pragma once
#include <windows.h>

// The cutscene camera moves on every frame above 30 FPS (see CutsceneCameraSmooth.cpp).
// No setting: installed from FPS.cpp whenever unlockFPS is 1 and the FPS limit is above 30.
bool installCutsceneCameraSmooth();
