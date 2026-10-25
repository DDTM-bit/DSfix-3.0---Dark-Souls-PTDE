// Dark Souls FPS fix by Clement Barnier (Nwks)
///////////////////////////////////////////////

#include "FPS.h"

#include <windows.h>
//#include <cmath>
#include "Settings.h"
#include "main.h"
#include "Detouring.h"
#include "RenderstateManager.h"
#include "memory.h"
#include "PhysicsFixes.h"
#include "FootPlacementFix.h"
#include "RootMotionFix.h"
#include "SlopeSlideFix.h"
#include "CutsceneCameraSmooth.h"
#include "SoulCounterFix.h"
#include "LadderSnapFix.h"
#include "StaminaFix.h"
#include "CameraFix.h"
#include "TurnFix.h"
#include "SfxFix.h"
#include "GhostFix.h"
#include "UIFix.h"
#include "AspectFix.h"
#include "ScriptFix.h"

extern bool g_Force30FPS;

#ifndef WITHOUT_GFWL_LIB
void enableGFWLCompatibility(void);
#endif

// Globals
static DWORD OriginalBase = 0x0400000;
static DWORD ImageBase = NULL;

// Hook Globals
double lastRenderTime;
static LARGE_INTEGER timerFreq;
static LARGE_INTEGER counterAtStart;

// Hook Parameters
//-----------------------------------
// Dark Souls executable timestamp
DWORD EXE_TIMESTAMP = 0x557F2FD0; // current Steam build (the addresses below; any other build uses pattern matching)

// Time-step value address
DWORD ADDR_TS = 0x011E7E90;  // Steam Beta 2.0 was 0x011E4D70, 1.0.0 0x012497F0, 1.0.1 0x012498E0
LPCSTR TS_PATTERN = "0080264400009444000058420000C0428988083D0000A044";
DWORD TS_OFFSET = 0x00000010;

// Presentation interval address
DWORD ADDR_PRESINT = 0x00FFB68E; // Steam Beta 2.0 was 0x00FFA15E, 1.0.0 0x010275AE, 1.0.1 0x0102788E
LPCSTR PRESINT_PATTERN = "FF15xxxxxxxx83C408C78648020000020000005EC20800";
DWORD PRESINT_OFFSET = 0x0000000F;

// getDrawThreadMsgCommand address in HGCommandDispatcher loop
DWORD ADDR_GETCMD =	0x00BAC4ED; // Steam Beta 2.0 was 0x00BAC3DD, 1.0.0 0x00BD601D, 1.0.1 0x00BD60ED
LPCSTR GETCMD_PATTERN = "6A018BCDE8xxxxxxxx8BF08BCEE8xxxxxxxx83F805";
DWORD GETCMD_OFFSET = 0x0000000D;

//----------------------------------------------------------------------------------------
// Support Functions
//----------------------------------------------------------------------------------------

// Misc
//------------------------------------
DWORD getAbsoluteAddress(DWORD offset) {
	if (ImageBase)
		return ImageBase + offset;
	else
		return NULL;
}

DWORD convertAddress(DWORD Address) {
	return getAbsoluteAddress(Address - OriginalBase);
}

// Memory
//------------------------------------
// Writes the engine's time step and returns the value written (seconds).
float updateAnimationStepTime(float stepTime, float minFPS, float maxFPS) {
	float FPS = 1.0f/(stepTime/1000);

	if (FPS < minFPS)
		FPS = minFPS;
	else if (FPS > maxFPS)
		FPS = maxFPS;
	
	float cappedStep = 1/(float)FPS;
	if(RSManager::get().isPaused()) cappedStep = 0.000000000000000001f;

	// The step lives in the game's read-only constants. Its page is made writable once (it used to be unlocked and
	// locked again around every write, two system calls per frame); then the step is a plain 4-byte store.
	static float* stepAddress = nullptr;
	if (!stepAddress) {
		float* address = (float*)convertAddress(ADDR_TS);
		DWORD oldProtect;
		if (address && VirtualProtect(address, sizeof(float), PAGE_READWRITE, &oldProtect)) stepAddress = address;
	}
	if (stepAddress) {
		*(volatile float*)stepAddress = cappedStep;
	}
	else {
		DWORD data = *(DWORD*)&cappedStep;
		writeToAddress(&data, convertAddress(ADDR_TS), sizeof(data));
	}
	return cappedStep;
}

// Timer
double getElapsedTime(void) {
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (double)( (c.QuadPart - counterAtStart.QuadPart) * 1000.0 / (double)timerFreq.QuadPart );
}

//----------------------------------------------------------------------------------------
// Hook functions
//----------------------------------------------------------------------------------------

void _stdcall updateFramerate(unsigned int cmd) {
	// If rendering was performed, update animation step-time
	if ((cmd == 2) || (cmd == 5)) {
		double maxFPS = g_Force30FPS ? 30.0 : (double)Settings::get().getCurrentFPSLimit();
		double minFPS = 10.0f;

		double currentTime = getElapsedTime();
		double deltaTime = currentTime - lastRenderTime;
		lastRenderTime = currentTime;

		// Update step-time using the actual elapsed render time without artificial snapping
		const float writtenStep = updateAnimationStepTime((float)deltaTime, (float)minFPS, (float)maxFPS);

		// DSfix 3.0: physics, ghosts and the ladder fix use exactly the step the engine just received,
		// so they cannot drift from game time on hitches, frame-limit jitter or pause. During pause the step is ~0,
		// which updatePhysicsDeltas clamps to its 1 ms minimum (all per-frame factors are then ~neutral).
		updatePhysicsDeltas(writtenStep);
		// Foot placement and root motion use the same step; each does nothing unless it was installed (FPS limit above 30).
		updateFootPlacementFix(writtenStep);
		updateRootMotionFix(writtenStep);
	}
}

// Hook
__declspec(naked) void getDrawThreadMsgCommand(void) {
	__asm {
		MOV EAX, [ECX+0Ch] // Put msgCmd in EAX (Return value)
		PUSHAD
		PUSH EAX
		CALL updateFramerate // Call updateFramerate(msgCmd)
		POPAD
		RETN
	}
}

//----------------------------------------------------------------------------------------
// Game Patches
//----------------------------------------------------------------------------------------
void applyFPSPatch() {

	SDLOG(0, "Starting FPS unlock...\n");
#ifndef WITHOUT_GFWL_LIB
	SDLOG(0, "Applying GFWL compatibility\n");
	enableGFWLCompatibility();
#endif

	// Get image info
	MODULEINFO moduleInfo;
	PIMAGE_DOS_HEADER dosHeader;
	PIMAGE_NT_HEADERS ntHeader;
    IMAGE_FILE_HEADER header;

	if(GetModuleInformation(GetCurrentProcess(), GetModuleHandle(NULL), &moduleInfo, sizeof(moduleInfo)))
	{
		ImageBase = (DWORD)moduleInfo.lpBaseOfDll;
		SDLOG(0, "ImageBase at 0x%08X\n", ImageBase);

		dosHeader = (PIMAGE_DOS_HEADER)ImageBase;
		ntHeader = (PIMAGE_NT_HEADERS)((DWORD)(dosHeader) + (dosHeader->e_lfanew));
		header = ntHeader->FileHeader;
		DWORD TimeStamp = header.TimeDateStamp;
				SDLOG(0, "Executable timestamp: 0x%08X, config: 0x%08X\n", TimeStamp, EXE_TIMESTAMP);

		// Perform pattern matching if timestamp differs
		if (TimeStamp != EXE_TIMESTAMP) {
			SDLOG(0, "Trying pattern matching...\n");

			DWORD address;
			address = GetMemoryAddressFromPattern(NULL, TS_PATTERN, TS_OFFSET);
			if(address != NULL) {
				SDLOG(0, "ADDR_TS found at 0x%08X\n", address);
				ADDR_TS = address;
			}
			else {
				SDLOG(0, "Could not match ADDR_TS pattern, FPS not unlocked\n");
				return;
			}
			address = GetMemoryAddressFromPattern(NULL, PRESINT_PATTERN, PRESINT_OFFSET);
			if(address != NULL) {
				SDLOG(0, "ADDR_PRESINT found at 0x%08X\n", address);
				ADDR_PRESINT = address;
			}
			else {
				SDLOG(0, "Could not match ADDR_PRESINT pattern, FPS not unlocked\n");
				return;
			}
			address = GetMemoryAddressFromPattern(NULL, GETCMD_PATTERN, GETCMD_OFFSET);
			if(address != NULL) {
				SDLOG(0, "ADDR_GETCMD found at 0x%08X\n", address);
				ADDR_GETCMD = address;
			}
			else {
				SDLOG(0, "Could not match ADDR_GETCMD pattern, FPS not unlocked\n");
				return;
			}
			SDLOG(0, "Pattern matching successful\n");
		}
		else
			SDLOG(0, "Using configured addresses\n");
	}
	else
	{
		SDLOG(0, "GetModuleInformation failed, FPS not unlocked\n");
		return;
	}

	// Binary patches
	//--------------------------------------------------------------
	installPhysicsFixes();
	// Foot placement on stairs and slopes: always on above 30 FPS (no setting). At 30 it would change nothing.
	if (Settings::get().getCurrentFPSLimit() > 30) installFootPlacementFix();
	else SDLOG(0, "FootPlacementFix: FPS limit is 30, nothing patched\n");
	// Animation root motion (slope slides, steering): always on above 30 FPS (no setting).
	if (Settings::get().getCurrentFPSLimit() > 30) installRootMotionFix();
	else SDLOG(0, "RootMotionFix: FPS limit is 30, nothing patched\n");
	// Slope slides start reliably (slide contact kept for 1/30 s): always on above 30 FPS (no setting).
	if (Settings::get().getCurrentFPSLimit() > 30) installSlopeSlideFix();
	else SDLOG(0, "SlopeSlideFix: FPS limit is 30, nothing patched\n");
	// Cutscene camera moves on every frame (blended between its 1/30 s keys): always on above 30 FPS (no setting).
	if (Settings::get().getCurrentFPSLimit() > 30) installCutsceneCameraSmooth();
	else SDLOG(0, "CutsceneCameraSmooth: FPS limit is 30, nothing patched\n");
	// Soul counter rolls up at the 30 FPS speed: always on above 30 FPS (no setting).
	if (Settings::get().getCurrentFPSLimit() > 30) installSoulCounterFix();
	else SDLOG(0, "SoulCounterFix: FPS limit is 30, nothing patched\n");
	installLadderSnapFix();
	installStaminaFix();
	installCameraFix();
	installTurnFix();
	installSfxFix();
	installGhostFix();
	installUIFix();
	installAspectFix();
	installScriptFix();
	
	DWORD address;
	DWORD data;

	// Override D3D Presentation Interval
	address = convertAddress(ADDR_PRESINT);
	data = 5; //Set to immediate
	writeToAddress(&data, address, sizeof(data));

	// Detour call to getDrawThreadMsgCommand
	address = convertAddress(ADDR_GETCMD);
	DetourApply((BYTE*)address, (BYTE*)getDrawThreadMsgCommand, 5, CALLOP);

	// --- NEW: Disable Present Catch-up Loop (VRR stutter fix) ---
	DWORD kRepeatJump = 0x00BACE90;
	BYTE* jumpSite = (BYTE*)kRepeatJump;
	if (jumpSite[0] == 0x7E && jumpSite[1] == 0x36) { // Check for 'jle 0xbacec8'
		DWORD oldProtect;
		if (VirtualProtect(jumpSite, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			jumpSite[0] = 0xEB; // Change 'jle' to 'jmp' to skip the loop
			VirtualProtect(jumpSite, 1, oldProtect, &oldProtect);
			FlushInstructionCache(GetCurrentProcess(), jumpSite, 1);
			SDLOG(0, "FPS: Present catch-up loop disabled.\n");
		}
	}
	else if (jumpSite[0] == 0xEB && jumpSite[1] == 0x36) {
		SDLOG(0, "FPS: Present catch-up loop was already disabled.\n");
	}
	else {
		SDLOG(0, "FPS: catch-up loop jump at 0x%08X is %02X %02X, not the expected 7E 36 - not patched.\n", kRepeatJump, jumpSite[0], jumpSite[1]);
	}

	SDLOG(0, "FPS unlocked\n");
}

void initFPSTimer() {
	// Init counter for frame-rate calculations
	lastRenderTime = 0.0f;
	QueryPerformanceFrequency(&timerFreq);
	QueryPerformanceCounter(&counterAtStart);
}
