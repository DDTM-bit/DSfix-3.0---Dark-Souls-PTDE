#pragma once
#include <windows.h>

// Installs in-place memory operand patches for gravity, friction, and air damping
bool installPhysicsFixes();

// Updates the scaled variables once per frame from the render/timing loop
void updatePhysicsDeltas(float deltaSeconds);
extern float g_PhysicsDeltaTime;