#pragma once

// Preserves stamina tick fractional remainders for accurate sprint drain at high FPS
bool installStaminaFix();

// Blighttown swamp poison builds up at the 30 FPS speed (above 30 FPS; no game code is patched)
void installSwampPoisonFix();
// once per frame, with the step the engine got this frame
void updateSwampPoisonFix(float step);
