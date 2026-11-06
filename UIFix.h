#pragma once

// Normalizes HUD gauge animations and loading screen swirl speeds
// loadingSwirl: also patch the loading screen swirl (FPS.cpp: only when the FPS limit is above 30).
bool installUIFix(bool loadingSwirl);