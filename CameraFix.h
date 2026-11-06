#pragma once

// Makes the follow camera's lock-on panning and smoothing framerate-independent
bool installCameraFix();

// cameraDistance: the camera's distance as a multiplier of the game's (1.0 = vanilla, nothing patched); lock-on too
// with cameraDistanceLockOn 1.
// Installed from main.cpp, with or without unlockFPS. toggleCameraDistance switches between the setting and vanilla.
bool installCameraDistance();
void toggleCameraDistance();
