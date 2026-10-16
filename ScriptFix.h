#pragma once

// Game script memory. Installed with the FPS fixes (unlockFPS 1).
// The enemy AI scripts and a second script state are cleaned up (Lua garbage collection) on a countdown that runs on the
// engine's time step. At a high frame rate many times as much garbage piled up between clean-ups, and in a crowded area
// the game could run out of script memory and close itself. Every countdown now takes at least 1/30 s per frame.
bool installScriptFix();
