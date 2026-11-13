// Dark Souls FPS fix by Clement Barnier (Nwks)
///////////////////////////////////////////////

#pragma once

void initFPSTimer();
void applyFPSPatch();

// returns time since startup in milliseconds
double getElapsedTime(void);

// unlockFPS 1: the step the engine got this frame (seconds, 0.001 to 0.125), written by the frame-step hook.
// Stays 1/30 with unlockFPS 0 (nothing reads it then).
extern float g_engineStep;
