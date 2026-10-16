
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
#include "IntroSkip.h"
#include "GameParams.h"
#include "DurabilityBars.h"
#include <cstdint>
#include <mutex>
#include "RenderstateManager.h"
#include "WindowManager.h"
#include <vector>
#include <intrin.h>


// globals
tDirect3DCreate9 oDirect3DCreate9 = Direct3DCreate9;
tDirectInput8Create oDirectInput8Create;
std::ofstream ofile;	
char dlldir[320];
std::mutex logMutex; // DSfix 3.0: Global mutex to prevent thread-racing crashes

// DSfix 3.0: Integrated Bonfire Input Softlock Fix (Original logic by SeanPesce & Nullby7e)
// Every pointer of the chain is checked with VirtualQuery before it is read. The chain
// holds stale values while characters are rebuilt (map loads, joining another world): before, each read through such a
// value raised a hidden access violation (caught by __try, so no crash, but every 200 ms during loads, and seen by any
// crash reporter or debugger in the process). Now an unreadable pointer simply counts as "not sitting", exactly what the
// __except path did. Everything else is unchanged.
namespace {
    bool bonfireReadable(uintptr_t address, size_t size) {
        MEMORY_BASIC_INFORMATION info = {};
        if (address < 0x10000 || !VirtualQuery((LPCVOID)address, &info, sizeof(info))) return false;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
        const uintptr_t end = (uintptr_t)info.BaseAddress + info.RegionSize;
        return address + size <= end;
    }

    bool bonfireRead32(uintptr_t address, uint32_t* out) {
        if (!bonfireReadable(address, 4)) return false;
        __try {
            *out = *(const uint32_t*)address;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool bonfireRead8(uintptr_t address, uint8_t* out) {
        if (!bonfireReadable(address, 1)) return false;
        __try {
            *out = *(const uint8_t*)address;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Reads the state the watchdog needs. Returns 0 when some pointer of the chain is not readable right now, 1 when the
    // character is neither Human nor Hollow (nothing is checked, as before), 2 when everything was read.
    // sitting: one of the three bonfire sit animations is playing
    // menuOpen: any of the bonfire menus or a dialog is open; animAddress: where the animation id lives
    int bonfireState(uintptr_t baseAddr, bool* sitting, bool* menuOpen, uintptr_t* animAddress) {
        *sitting = false;
        *menuOpen = false;
        *animAddress = 0;

        // 1. Player character status (Human = 0, Hollow = 8)
        uint32_t statusPtr = 0, status = 0;
        if (!bonfireRead32(baseAddr + 0xF7E204, &statusPtr) || !bonfireRead32(statusPtr + 0xA28, &status)) return 0;
        if (status != 0 && status != 8) return 1;

        // 2. The nested pointer to the character's current animation ID
        uint32_t animPtr = 0, animObj = 0, currentAnim = 0;
        if (!bonfireRead32(baseAddr + 0xEE29E8, &animPtr) || !bonfireRead32(animPtr + 0x0, &animObj) ||
            !bonfireRead32(animObj + 0xFC, &currentAnim)) return 0;
        *sitting = (currentAnim == 7701 || currentAnim == 7711 || currentAnim == 7721);
        *animAddress = (uintptr_t)animObj + 0xFC;

        // 3. The bonfire UI menu flags: bonfire, repair, level up, bottomless box, attune, reinforce, warp, dialog
        uint32_t menuPtr = 0;
        if (!bonfireRead32(baseAddr + 0xF786D0, &menuPtr)) return 0;
        static const uint32_t kMenuFlags[] = { 0x40, 0x4C, 0x78, 0x84, 0x80, 0x50, 0xAC, 0x60 };
        for (uint32_t offset : kMenuFlags) {
            uint8_t open = 0;
            if (!bonfireRead8(menuPtr + offset, &open)) return 0;
            if (open) *menuOpen = true;
        }
        return 2;
    }
}

DWORD WINAPI BonfireGlitchDetectionThread(LPVOID lpParam) {
    Sleep(1000); // Wait for the game to initialize
    uintptr_t baseAddr = (uintptr_t)GetModuleHandle(NULL);
    DWORD first_detected = 0;

    while (true) {
        Sleep(200);
        bool isSitting = false, menuOpen = false;
        uintptr_t animAddress = 0;

        const int state = bonfireState(baseAddr, &isSitting, &menuOpen, &animAddress);
        if (state == 0) {
            // A pointer in the chain is not readable right now (same as the old __except path)
            first_detected = 0;
            isSitting = false;
        }
        else if (state == 1) {
            // neither Human nor Hollow: nothing is checked (as before)
        }
        // 4. Check if we are softlocked (Sitting animation + NO UI Menus open)
        else if (isSitting && !menuOpen) {
            if (first_detected == 0) {
                first_detected = GetTickCount();
            }
            else if ((GetTickCount() - first_detected) >= 1000) {
                // 5. Break the softlock!
                __try {
                    *(uint32_t*)animAddress = 0;
                    SDLOG(0, "DSfix 3.0: Bonfire input softlock detected and neutralized.\n");
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    first_detected = 0;
                }
            }
        }
        else {
            first_detected = 0;
        }

        // Pass the sitting state to RSManager to disable SSAO
        RSManager::get().setBonfireDisableSSAO(isSitting);
    }
    return 0;
}

typedef BOOL(WINAPI* PFN_SETPROCESSDPIAWARENESSCONTEXT)(HANDLE);
typedef HANDLE(WINAPI* PFN_GETTHREADDPIAWARENESSCONTEXT)(void);
typedef int(WINAPI* PFN_GETAWARENESSFROMDPIAWARENESSCONTEXT)(HANDLE);

// DSfix 3.0: these run in DllMain before DSfix.log is open, so their results are stored and written to the log
// once the log and the settings are loaded (see DllMain).
static char g_dpiStatus[256] = "not applied";
static char g_cpuStatus[128] = "not applied";

static void ApplyModernDPIAwareness() {
	HMODULE hUser32 = GetModuleHandleA("user32.dll");
	if (hUser32) {
		PFN_SETPROCESSDPIAWARENESSCONTEXT SetDpiContext =
			(PFN_SETPROCESSDPIAWARENESSCONTEXT)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
		if (SetDpiContext) {
			// -4 corresponds to DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
			if (SetDpiContext((HANDLE)-4)) {
				strcpy_s(g_dpiStatus, "per-monitor DPI aware (v2) - Windows will not scale the game window");
				return;
			}
			const DWORD err = GetLastError();
			// Report what Windows actually uses instead (0 = unaware, 1 = system aware, 2 = per-monitor aware)
			int effective = -1;
			PFN_GETTHREADDPIAWARENESSCONTEXT GetCtx = (PFN_GETTHREADDPIAWARENESSCONTEXT)GetProcAddress(hUser32, "GetThreadDpiAwarenessContext");
			PFN_GETAWARENESSFROMDPIAWARENESSCONTEXT GetAw = (PFN_GETAWARENESSFROMDPIAWARENESSCONTEXT)GetProcAddress(hUser32, "GetAwarenessFromDpiAwarenessContext");
			if (GetCtx && GetAw) effective = GetAw(GetCtx());
			const char* effText = effective == 2 ? "per-monitor aware (OK)" : (effective == 1 ? "system aware" : (effective == 0 ? "UNAWARE - Windows may scale the game window" : "unknown"));
			sprintf_s(g_dpiStatus, "per-monitor v2 request refused (error %lu%s), effective: %s", err,
				err == ERROR_ACCESS_DENIED ? ", already fixed by the exe manifest or the compatibility setting 'Override high DPI scaling behavior'" : "",
				effText);
			return;
		}
	}
	// Fallback for older Windows builds
	if (SetProcessDPIAware()) strcpy_s(g_dpiStatus, "system DPI aware (legacy fallback, older Windows)");
	else sprintf_s(g_dpiStatus, "legacy SetProcessDPIAware failed (error %lu)", GetLastError());
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
                strcpy_s(g_cpuStatus, "EcoQoS / power throttling disabled");
            } else {
                sprintf_s(g_cpuStatus, "EcoQoS opt-out refused (error %lu)", GetLastError());
            }
        } else {
            strcpy_s(g_cpuStatus, "EcoQoS not available on this Windows version (nothing to disable)");
        }
    }
}

// DSfix 3.0: menu mouse mapping (only with enableHudMod). The game reads the cursor for its menus with GetCursorPos
// (found with a probe: no GetMessagePos / raw input). While a menu window is shrunk, remapMenuCursor (RenderstateManager.cpp)
// moves the reported position back to where that spot of the menu was before the shrink, so hover and clicks hit what
// is drawn under the cursor. With no shrunk window open the real position is passed through unchanged.
void remapMenuCursor(POINT* p); // RenderstateManager.cpp
namespace {
    typedef BOOL (WINAPI* GetCursorPos_t)(LPPOINT);
    GetCursorPos_t oGetCursorPos = NULL;
    BOOL WINAPI hkGetCursorPos(LPPOINT p) {
        const BOOL ok = oGetCursorPos(p);
        if (ok) remapMenuCursor(p);
        return ok;
    }

    // Replaces the game's import-table entry of dll!fn. Returns false if the game does not import it by name.
    bool patchGameImport(const char* dll, const char* fn, void* hook, void** orig) {
        BYTE* base = (BYTE*)GetModuleHandle(NULL);
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return false;
        for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
            if (_stricmp((const char*)(base + d->Name), dll) != 0) continue;
            IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
            void** slots = (void**)(base + d->FirstThunk);
            for (int i = 0; names[i].u1.AddressOfData; ++i) {
                if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
                IMAGE_IMPORT_BY_NAME* byName = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
                if (strcmp((const char*)byName->Name, fn) != 0) continue;
                DWORD old;
                if (!VirtualProtect(&slots[i], sizeof(void*), PAGE_READWRITE, &old)) return false;
                *orig = slots[i];
                slots[i] = hook;
                VirtualProtect(&slots[i], sizeof(void*), old, &old);
                return true;
            }
        }
        return false;
    }

    void installMenuMouseMapping() {
        const bool ok = patchGameImport("user32.dll", "GetCursorPos", (void*)hkGetCursorPos, (void**)&oGetCursorPos);
        SDLOG(0, "Menu mouse mapping: %s\n", ok ? "GetCursorPos hooked" : "GetCursorPos not imported by the game, not active");
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

    if (Settings::get().getEnableHudMod()) installMenuMouseMapping();

    // disableItemAutoEquip / durabilityBars: finds the game's item and weapon tables once they are loaded (own thread)
    startGameParamsThread();

    // load original dinput8.dll safely outside loader lock
    HMODULE hMod;
    if (Settings::get().getDinput8dllWrapper().empty() || (Settings::get().getDinput8dllWrapper().find("none") == 0)) {
        char syspath[320];
        GetSystemDirectory(syspath, 320);
        strcat_s(syspath, "\\dinput8.dll");
        hMod = LoadLibrary(syspath);
    }
    else {
        SDLOG(0, "Loading dinput wrapper %s\n", Settings::get().getDinput8dllWrapper().c_str());
        hMod = LoadLibrary(Settings::get().getDinput8dllWrapper().c_str());
    }
    if (!hMod) {
        sdlog("Could not load original dinput8.dll\nABORTING.\n");
        errorExit((LPTSTR)"Loading of specified dinput wrapper");
    }
    oDirectInput8Create = (tDirectInput8Create)GetProcAddress(hMod, "DirectInput8Create");
    SDLOG(0, " - dinput8 loaded at %p, DirectInput8Create %s\n", (void*)hMod, oDirectInput8Create ? "found" : "MISSING");
}

// 2. Immediate setup for timing-critical Engine functions
bool WINAPI DllMain(HMODULE hDll, DWORD dwReason, PVOID pvReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hDll);
        g_hModule = hDll;
        WindowManager::rememberClipCursor(); // before any other mod is loaded (see WindowManager.cpp)

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
        SDLOG(0, "DPI awareness: %s\n", g_dpiStatus);
        SDLOG(0, "CPU power management: %s\n", g_cpuStatus);

        KeyActions::get().load();
        KeyActions::get().report();
        SaveManager::get().init();

        // MUST execute immediately so DSfix intercepts the DirectX graphics engine
        earlyDetour();

        // MUST execute immediately before game threads start to avoid logic deadlocks
        initFPSTimer();
        if (Settings::get().getUnlockFPS()) applyFPSPatch();

        // skipIntro (with or without unlockFPS): the title screen's own steps skip the autosave notice and the logos
        if (Settings::get().getSkipIntro()) installIntroSkip();

        // durabilityBars: notes the durability of the equipped weapons when the game reads it
        if (Settings::get().getDurabilityBars()) installDurabilityBars();

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


