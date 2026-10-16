#include "IntroSkip.h"
#include "Settings.h" // SDLOG reads the log level when RELEASE_VER is not defined
#include "main.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

// Start-up autosave notice and logos.
//
// The title screen (TitleStep_forWin32, its step functions in the table at 0x1378FE8):
//  * Step 0 (0xEE6AC0) opens the autosave notice (system message 45000, "This game uses an autosave function...") from
//    0xEE6B2D on, and step 1 (0xEE62C0) keeps it for 10 s or until a button. With skipIntro the notice is never opened:
//    the step jumps over that block, marks the notice's time as run out and moves on; step 1 then goes straight to the
//    save data check.
//  * Step 37 (0xEE5900) sets MenuMan+0x10C to 1, which opens the logo dialog (logo_BNG, logo_from, logo_havok); step 38
//    waits until the dialog's last logo sets it back to 0. With skipIntro that write is left out: the dialog never opens
//    and step 38 goes straight on. The console title step (0xEE92A0) has the same pair and is patched too.
// Only these start-up steps change. The flaming autosave icon shown while the game saves during play is not touched.
//
// Before, DSfix's skipIntro sped up the game's clock while it saw the Bandai Namco logo, through a hook on
// QueryPerformanceCounter that stayed in place for the whole session (and took a stack trace on every call).

namespace {
    // mov dword ptr [eax+10Ch], edx (edx = 1, MenuMan in eax): "show the logos". The add [ecx+8], edx after it moves the
    // title step on and stays.
    const DWORD kLogoOpenSites[2] = { 0x00EE5919, 0x00EE92B9 };
    const BYTE kLogoOpen[6] = { 0x89, 0x90, 0x0C, 0x01, 0x00, 0x00 };
    const BYTE kNops[6] = { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 }; // nop word ptr [eax+eax+0] (one 6-byte nop)

    // mov eax, [013786D0h] at the start of the notice block in title step 0
    const DWORD kNoticeOpenSite = 0x00EE6B2D;
    const BYTE kNoticeOpen[5] = { 0xA1, 0xD0, 0x86, 0x37, 0x01 };
    DWORD g_noticeResume = 0x00EE6B68; // add [esi+8], edi  (the step's own increment)

    __declspec(naked) void skip_notice() {
        __asm {
            mov byte ptr[esi + 0x5C], 1   // step 1: the notice's time has run out
            mov edi, 1                    // the step's increment
            jmp dword ptr[g_noticeResume]
        }
    }

    bool patch(DWORD va, const BYTE* expect, const BYTE* repl, size_t n) {
        BYTE* p = (BYTE*)va;
        __try {
            if (memcmp(p, expect, n) != 0) return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        DWORD oldProtect;
        if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
        memcpy(p, repl, n);
        FlushInstructionCache(GetCurrentProcess(), p, n);
        VirtualProtect(p, n, oldProtect, &oldProtect);
        return true;
    }

    bool patchJump(DWORD va, const BYTE* expect, size_t n, const void* target) {
        BYTE code[5] = { 0xE9, 0, 0, 0, 0 };
        const int32_t rel = (int32_t)((DWORD)target - (va + 5));
        memcpy(code + 1, &rel, 4);
        return n == sizeof(code) && patch(va, expect, code, n);
    }
}

bool installIntroSkip() {
    if ((DWORD)GetModuleHandle(NULL) != 0x00400000) {
        SDLOG(0, "skipIntro: game image is not at its expected base, intro not skipped\n");
        return false;
    }
    int logos = 0;
    for (DWORD site : kLogoOpenSites) {
        if (patch(site, kLogoOpen, kNops, sizeof(kLogoOpen))) ++logos;
    }
    const bool notice = patchJump(kNoticeOpenSite, kNoticeOpen, sizeof(kNoticeOpen), (const void*)&skip_notice);
    if (logos == 0 && !notice) {
        SDLOG(0, "skipIntro: the title screen's steps do not match this build; the notice and the logos are shown\n");
        return false;
    }
    SDLOG(0, "skipIntro: autosave notice %s; start-up logos skipped (%d of 2 title steps patched)\n",
        notice ? "skipped" : "shown (its step did not match this build)", logos);
    return true;
}
