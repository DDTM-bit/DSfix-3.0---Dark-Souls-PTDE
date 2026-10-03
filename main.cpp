
#define _CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES 1
#include <windows.h>
#include <fstream>
#include <ostream>
#include <iostream>
#include <fstream>
#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <sys/timeb.h>
#include <strsafe.h>

#include "main.h"
#include "d3d9.h"
#include "d3dutil.h"
#include "Settings.h"
#include "KeyActions.h"
#include "Detouring.h"
#include "SaveManager.h"
#include "FPS.h"
#include <cstdint>
#include <mutex>
#include "RenderstateManager.h"
#include <vector>
#include <intrin.h>


// globals
tDirect3DCreate9 oDirect3DCreate9 = Direct3DCreate9;
tDirectInput8Create oDirectInput8Create;
std::ofstream ofile;	
char dlldir[320];
std::mutex logMutex; // DSfix 3.0: Global mutex to prevent thread-racing crashes

// DSfix 3.0: Integrated Bonfire Input Softlock Fix (Original logic by SeanPesce & Nullby7e)
DWORD WINAPI BonfireGlitchDetectionThread(LPVOID lpParam) {
    Sleep(1000); // Wait for the game to initialize
    uintptr_t baseAddr = (uintptr_t)GetModuleHandle(NULL);
    DWORD first_detected = 0;

    while (true) {
        Sleep(200);
        bool isSitting = false;

        __try {
            // 1. Read player character status (Human = 0, Hollow = 8)
            uintptr_t statusPtr = *(uintptr_t*)(baseAddr + 0xF7E204);
            int status = *(int*)(statusPtr + 0xA28);

            if (status == 0 || status == 8) {
                // 2. Resolve the nested pointer to the character's current animation ID
                uintptr_t animPtr = *(uintptr_t*)(baseAddr + 0xEE29E8);
                animPtr = *(uintptr_t*)(animPtr + 0x0);
                uint32_t current_anim = *(uint32_t*)(animPtr + 0xFC);

                // Check if sitting animation is playing
                isSitting = (current_anim == 7701 || current_anim == 7711 || current_anim == 7721);

                // 3. Read the Bonfire UI menu flags
                uintptr_t menuPtr = *(uintptr_t*)(baseAddr + 0xF786D0);

                uint8_t bonfire_menu = *(uint8_t*)(menuPtr + 0x40);
                uint8_t repair_menu = *(uint8_t*)(menuPtr + 0x4C);
                uint8_t level_menu = *(uint8_t*)(menuPtr + 0x78);
                uint8_t bottomless_menu = *(uint8_t*)(menuPtr + 0x84);
                uint8_t attune_menu = *(uint8_t*)(menuPtr + 0x80);
                uint8_t reinforce_menu = *(uint8_t*)(menuPtr + 0x50);
                uint8_t warp_menu = *(uint8_t*)(menuPtr + 0xAC);
                uint8_t dialog_menu = *(uint8_t*)(menuPtr + 0x60);

                // 4. Check if we are softlocked (Sitting animation + NO UI Menus open)
                if (isSitting && !bonfire_menu && !repair_menu && !bottomless_menu && !reinforce_menu &&
                    !level_menu && !attune_menu && !dialog_menu && !warp_menu)
                {
                    if (first_detected == 0) {
                        first_detected = GetTickCount();
                    }
                    else if ((GetTickCount() - first_detected) >= 1000) {
                        // 5. Break the softlock!
                        *(uint32_t*)(animPtr + 0xFC) = 0;
                        SDLOG(0, "DSfix 3.0: Bonfire input softlock detected and neutralized.\n");
                    }
                }
                else {
                    first_detected = 0;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // If any pointer in the chain is invalid, catch the access violation silently.
            first_detected = 0;
            isSitting = false;
        }

        // Pass the sitting state to RSManager to disable SSAO
        RSManager::get().setBonfireDisableSSAO(isSitting);
    }
    return 0;
}

typedef BOOL(WINAPI* PFN_SETPROCESSDPIAWARENESSCONTEXT)(HANDLE);

static void ApplyModernDPIAwareness() {
	HMODULE hUser32 = GetModuleHandleA("user32.dll");
	if (hUser32) {
		PFN_SETPROCESSDPIAWARENESSCONTEXT SetDpiContext =
			(PFN_SETPROCESSDPIAWARENESSCONTEXT)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
		if (SetDpiContext) {
			// -4 corresponds to DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
			SetDpiContext((HANDLE)-4);
			return;
		}
	}
	// Fallback for older Windows builds
	SetProcessDPIAware();
}

#include <vector>

void OptimizeCPUExecution() {
    // 1. Remove HIGH_PRIORITY_CLASS completely to prevent driver starvation.
    // If you absolutely must elevate priority, use ABOVE_NORMAL_PRIORITY_CLASS, 
    // but NORMAL_PRIORITY_CLASS is best for DirectX 9 games.

    // 2. Remove the manual P-Core affinity lock. Let the Windows 11 Thread Director 
    // natively handle routing heavy threads to P-Cores and audio/net to E-Cores.

    // 3. Keep the EcoQoS (Power Throttling) disable to ensure the OS doesn't 
    // park cores while the game is running.
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        typedef BOOL(WINAPI* PFN_SETPROCESSINFORMATION)(HANDLE, DWORD, LPVOID, DWORD);
        PFN_SETPROCESSINFORMATION SetProcInfo =
            (PFN_SETPROCESSINFORMATION)GetProcAddress(hKernel32, "SetProcessInformation");

        if (SetProcInfo) {
            struct {
                ULONG Version;
                ULONG ControlMask;
                ULONG StateMask;
            } powerThrottling;

            powerThrottling.Version = 1;     // PROCESS_POWER_THROTTLING_CURRENT_VERSION
            powerThrottling.ControlMask = 1; // PROCESS_POWER_THROTTLING_EXECUTION_SPEED
            powerThrottling.StateMask = 0;   // 0 disables throttling (enables max turbo)

            // 4 corresponds to ProcessPowerThrottling in PROCESS_INFORMATION_CLASS
            if (SetProcInfo(GetCurrentProcess(), 4, &powerThrottling, sizeof(powerThrottling))) {
                SDLOG(0, "DSfix 3.0: EcoQoS / Power Throttling successfully disabled.\n");
            }
        }
    }
}

HMODULE g_hModule = NULL;
bool g_dsfixInitialized = false;

// 1. Deferred setup for dangerous OS-level functions ONLY
void InitializeDSfix() {
    if (g_dsfixInitialized) return;
    g_dsfixInitialized = true;

    // Launch the Bonfire Softlock Monitor safely outside loader lock
    CreateThread(NULL, 0, BonfireGlitchDetectionThread, NULL, 0, NULL);

    // load original dinput8.dll safely outside loader lock
    HMODULE hMod;
    if (Settings::get().getDinput8dllWrapper().empty() || (Settings::get().getDinput8dllWrapper().find("none") == 0)) {
        char syspath[320];
        GetSystemDirectory(syspath, 320);
        strcat_s(syspath, "\\dinput8.dll");
        hMod = LoadLibrary(syspath);
    }
    else {
        sdlog(0, "Loading dinput wrapper %s\n", Settings::get().getDinput8dllWrapper().c_str());
        hMod = LoadLibrary(Settings::get().getDinput8dllWrapper().c_str());
    }
    if (!hMod) {
        sdlog("Could not load original dinput8.dll\nABORTING.\n");
        errorExit((LPTSTR)"Loading of specified dinput wrapper");
    }
    oDirectInput8Create = (tDirectInput8Create)GetProcAddress(hMod, "DirectInput8Create");
}

// 2. Immediate setup for timing-critical Engine functions
bool WINAPI DllMain(HMODULE hDll, DWORD dwReason, PVOID pvReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hDll);
        g_hModule = hDll;

        ApplyModernDPIAwareness();
        OptimizeCPUExecution();

        TCHAR fileName[512];
        GetModuleFileName(NULL, fileName, 512);
        GetModuleFileName(hDll, dlldir, 512);
        for (int i = strlen(dlldir); i > 0; i--) { if (dlldir[i] == '\\') { dlldir[i + 1] = 0; break; } }

        ofile.open(GetDirectoryFile("DSfix.log"), std::ios::out);
        sdlogtime();
        SDLOG(0, "===== start DSfix %s = fn: %s\n", VERSION, fileName);

        // MUST load settings immediately so D3D creates buffers at your custom resolution
        Settings::get().load();
        Settings::get().report();

        KeyActions::get().load();
        KeyActions::get().report();
        SaveManager::get().init();

        // MUST execute immediately so DSfix intercepts the DirectX graphics engine
        earlyDetour();

        // MUST execute immediately before game threads start to avoid logic deadlocks
        initFPSTimer();
        if (Settings::get().getUnlockFPS()) applyFPSPatch();

        return true;
    }
    else if (dwReason == DLL_PROCESS_DETACH) {
        Settings::get().shutdown();
        endDetour();
        if (ofile) { ofile.close(); }
    }
    return true;
}

char* GetDirectoryFile(const char* filename) {
    thread_local static char path[320];
    strcpy_s(path, dlldir);
    strcat_s(path, filename);
    return path;
}

void __cdecl sdlogtime() {
	char timebuf[26];
    time_t ltime;
    struct tm gmt;
	time(&ltime);
    _gmtime64_s(&gmt, &ltime);
    asctime_s(timebuf, 26, &gmt);
	timebuf[24] = '\0'; // remove newline
	SDLOG(0, "===== %s =====\n", timebuf);
}

void __cdecl sdlog(const char *fmt, ...) {
	if(ofile.good()) {
		if(!fmt) { return; }

        std::lock_guard<std::mutex> lock(logMutex); // Locks the file so threads queue up politely

		va_list va_alist;
		char logbuf[9999] = {0};

		va_start (va_alist, fmt);
		_vsnprintf_s(logbuf+strlen(logbuf), sizeof(logbuf) - strlen(logbuf), _TRUNCATE, fmt, va_alist);
		va_end (va_alist);

		ofile << logbuf;
		ofile.flush();
	}
}

void errorExit(LPTSTR lpszFunction) { 
    // Retrieve the system error message for the last-error code
    LPVOID lpMsgBuf;
    LPVOID lpDisplayBuf;
    DWORD dw = GetLastError(); 

    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR) &lpMsgBuf, 0, NULL );

    // Display the error message and exit the process
    lpDisplayBuf = (LPVOID)LocalAlloc(LMEM_ZEROINIT, (lstrlen((LPCTSTR)lpMsgBuf) + lstrlen((LPCTSTR)lpszFunction) + 40) * sizeof(TCHAR)); 
    StringCchPrintf((LPTSTR)lpDisplayBuf, LocalSize(lpDisplayBuf) / sizeof(TCHAR), TEXT("%s failed with error %d: %s"), lpszFunction, dw, lpMsgBuf); 
    MessageBox(NULL, (LPCTSTR)lpDisplayBuf, TEXT("Error"), MB_OK); 

    LocalFree(lpMsgBuf);
    LocalFree(lpDisplayBuf);
    ExitProcess(dw); 
}

bool fileExists(const char *filename) {
  return std::ifstream(filename).good();
}

void createDirectory(const char *fileName) {
	CreateDirectory(GetDirectoryFile(fileName), nullptr);
	DWORD error = GetLastError();
	if (error && error != ERROR_ALREADY_EXISTS) {
		SDLOG(0, "Failed to create %s: %s\n", fileName, formatMessage(error));
	}
}

bool writeFile(const char *filename, const char *data, size_t length) {
	std::ofstream file(filename, std::ios::out | std::ios::binary);
	if (!file) {
		SDLOG(0, "Failed to open %s: %s\n", filename, strError(errno));
		return false;
	}
	file.write(data, length);
	if (!file) {
		SDLOG(0, "Failed to write to %s: %s\n", filename, strError(errno));
		return false;
	}
	file.close();
	if (!file) {
		SDLOG(0, "Failed to close %s: %s\n", filename, strError(errno));
		return false;
	}
	return true;
}

std::string formatMessage(DWORD messageId) {
	char *buffer = nullptr;
	size_t length = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, messageId, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)&buffer, 0, nullptr);
	std::string result(buffer, length);
	LocalFree(buffer);
	return result;
}

std::string strError(int err) {
	std::string result(4096, '\0');
	strerror_s(&result[0], result.length(), err);
	return result;
}
