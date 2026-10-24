#include "QuickSave.h"
#include "Settings.h"
#include "main.h" // SDLOG
#include "TitleMark.h" // titleScreenShowing
#include <windows.h>
#include <d3dx9.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <algorithm>
#pragma comment(lib, "advapi32.lib")

int hudFramesSinceSeen(); // RenderstateManager.cpp: frames since the in-game HUD was last drawn

// =====================================================================================================================
// LOOK OF THE QUICK SAVE / QUICK LOAD MESSAGES - everything that decides how they look is in this block. Sizes and
// places are parts of the screen (height for sizes, width / height for the place), so they look the same at every
// resolution. Colours are D3DCOLOR_ARGB(alpha, red, green, blue), 0..255.
// =====================================================================================================================
namespace QuickSaveLook {
    const char kSavedText[]     = "Quick Saved";
    const char kFailedText[]    = "Quick Save failed";
    const char kBlockedText[]   = "Quick Save not available now";
    const char kFontName[]      = "Palatino Linotype";          // any font installed with Windows
    const int kFontWeight       = FW_SEMIBOLD;         // FW_NORMAL, FW_SEMIBOLD or FW_BOLD
    const float kHeight         = 0.027f;              // text height (about 35 pixels at 1080p)
    const float kLeft           = 0.78f;               // left edge of the text: right of the longest health bar
    const float kTop            = 0.085f;              // top edge of the text: the row of the health bar
    const float kRightMargin    = 0.09f;               // a longer text moves left to end at least this far from the edge
    const D3DCOLOR kColor       = D3DCOLOR_ARGB(240, 114, 163, 109); // green
    const D3DCOLOR kFailColor   = D3DCOLOR_ARGB(240, 225, 105, 95);  // red: a quick save or load did not happen
    const D3DCOLOR kShadow      = D3DCOLOR_ARGB(170, 0, 0, 0);       // soft dark shadow, readable on bright spots
    const float kShadowOffset   = 0.0018f;             // shadow distance (at least 1 pixel)
    const float kFadeInSeconds  = 0.25f;
    const float kHoldSeconds    = 1.6f;
    const float kFadeOutSeconds = 0.9f;
    const float kGameIconSeconds = 1.0f;               // how long the game's own save icon (the flame) is shown
    // quick load
    const char kLoadAskText[]     = "Press again to Quick Load";
    const char kLoadingText[]     = "Quick Loading...";
    const char kLoadReadyText[]   = "Quick Load ready - choose Load Game";
    const char kLoadedText[]      = "Quick Loaded";
    const char kLoadFailedText[]  = "Quick Load failed";
    const char kLoadNoSaveText[]  = "No Quick Save for this character";
    const char kLoadBlockedText[] = "Quick Load not available now";
    const D3DCOLOR kAskColor      = D3DCOLOR_ARGB(240, 235, 190, 90);  // amber, for the question
    const float kConfirmSeconds   = 3.0f;              // time to press the quick load key the second time
    const float kReadyHoldSeconds = 5.0f;              // how long "Quick Load ready" stays on the title screen
}
// =====================================================================================================================

// Quick save.
//
// The quickSaveKey makes the game save the current character exactly as it does on its own (the same request its event
// scripts make with "SaveRequest"), waits until the game has written its save file, then copies that character - and
// only that character - into QuickSave\QS_DRAKS0005 next to the save file. Every quick save overwrites that file.
// Nothing in the game's save file is changed.
//
// THE GAME'S SAVE (checked in DARKSOULS.exe)
//   GameMan = [0x013784A0]
//   +0xA70  index of the character slot in play (0..9; -1 at the title screen)
//   +0xB0F  "Profile Save" requested: the character slot (USER_DATA00n)
//   +0xB10  "Option Save" requested: the menu / options data (USER_DATA010, it holds the load menu's slot summaries)
//   +0xB11  passed to the option save ("power on/off" in the game's debug text)
//   +0xB1C  a save is being written (set by the save functions 0xEBAB20 / 0xEBA9D0, cleared at 0xEBB74E when the
//           write has finished)
//   +0xB40  saving allowed (the event scripts can switch it off)
//   The script command "SaveRequest" (0x00D5EE90) sets B10, B0F and B11 when B40 is set; the game's update
//   (0x00EFB040) then starts the profile save, then the option save. Quick save sets the same three bytes.
//
// THE SAVE FILE (DRAKS0005.sl2, a BND4 container with 11 entries, not encrypted)
//   USER_DATA000..009  one character each: 0x60014 bytes = MD5 of the rest (16 bytes) + the character data
//   USER_DATA010       menu data, also with an MD5 first: +0x28 = 10 bytes "slot used", +0x38 + slot * 0x170 = the
//                      load menu's summary of each slot (name first, the same name as at +0x100 of the slot entry)
//
// QuickSave\QS_DRAKS0005 holds a small header, the slot's summary record and the slot entry exactly as the game wrote
// them (with their MD5), so a quick load can put that one character back without touching the other nine.
//
// THE GAME'S SAVE ICON (the flame in the corner): FrpgMenuDlgSaveIcon (0x00C82BD0) shows it while
// [[0x013786D0]+0x248] is 1. The game sets that only while a save is being written (0x00EFBC70), which for a quick
// save is about 0.15 s, too short to see. So it is held at 1 for kGameIconSeconds from the request, while the game
// is not writing a save of its own; nothing else in the game reads it.
//
// Allowed only while playing: the HUD is drawn, a character is loaded, it is alive, the game allows saving and no
// other player is in the world (the game's session state [[0x0137D82C]+0x90]: 3 = host, 4 = joining, 6 = client).
//
// Quick load (quickLoadKey, pressed twice within kConfirmSeconds; the same rules as quick save, plus the ones of the
// game's own "Quit Game"):
//   1. Checks that QuickSave\QS_DRAKS0005 is valid and belongs to the character in play (same slot, same name).
//   2. Quits to the title screen exactly as the system menu's "Quit Game" does when you answer yes (0x00FA5918):
//        MenuMan = [0x013786D0], SystemMan = [0x01378700]
//        if [SystemMan+8] != 0, [MenuMan+0x6C] == 0 and GameMan+0xB0C == 0: [MenuMan+0x244] = 1 and the save request
//          (0x00EB9510 with 0: B10 = B0F = 1, B11 = 0, only when B40 is set)
//        [MenuMan+0x238] = 1   "quit to the title": the in-game update (0x00EFC041 / 0x00EFC18E) waits until the save
//                              requests are done, clears it and leaves the game; so the game saves as it always does.
//      The system menu greys out "Quit Game" when GameMan+0xB4F is 0 or more than one player is in the world
//      ([[GameMan+0xBEC]+0x10] > 1, 0x00FA61B5); quick load refuses then too.
//   3. On the title screen, when the game has finished saving: the save file is kept as it is in
//      QuickSave\BeforeQuickLoad<yyMMddHHmmss>_DRAKS0005.sl2.bak (the 20 newest are kept; the backup feature never
//      sees them, it only looks at the save folder itself), then the slot entry and its load menu summary are put back
//      from the quick save, the slot is marked used and the menu data's MD5 is computed again. The other nine slots
//      and the rest of the menu data stay exactly as the game wrote them. The game reads a character from the file when
//      it is loaded, so "Load Game" then starts the quick save.
//   4. The automatic load: DSfix then does on the title screen what you would do - "Load Game", then the character:
//      - The title menu (FrpgMenuDlgTitle) is found through its per-frame update: its vtable slot 16 (0x011C2914, the
//        update 0x00C77D90) is pointed at a small stub that notes the dialog and jumps on to the game's update.
//        On "PRESS START BUTTON" (+0x10C is 0) DSfix does what pressing a button there does (0x00C77770):
//        +0x10C = 1, +0xF4 = 0, and the menu shows its items.
//        When the menu shows its items (+0x10C), is not closing (+0x110, +0x114 are 0) and offers "Load Game" (+0x118),
//        DSfix does what choosing "Load Game" does (0x00C77680): +0x100 = 3, +0xF4 = 0, +0x114 = 2 ("close"),
//        +0x108 = 3. The menu then fades out by itself and sets the title flow [0x0137868C] = 3 (0x00C77B65), and the
//        title step starts the game's LoadGameStep, exactly as when you choose it.
//      - LoadGameStep opens the character list: MenuMan+0x90 = 3, then 4 while it waits for you, +0x434 = 1 (load).
//        The list (FrpgMenuDlgProfileSelectList, found the same way through its update 0x00FAA8C0, vtable slot
//        0x011C4414) shows 4 rows of the 10 slots: every frame it sets MenuMan+0x180 = its row (+0x100, 0..3) + its
//        scroll (+0x108) (0x00FAA929), and the dialog copies that into the chosen slot MenuMan+0x438 (0x00C6E9AF).
//        DSfix sets the row (and the scroll if the slot is not in view; the list holds +0x104 slots), checks that the
//        list took it (+0x438 = the slot), then does what picking it does (0x00C1D375): +0x448 = 1,
//        [0x0137868A] = 1, +0x90 = 6. The list closes and LoadGameStep (0x00EEEA30) loads slot +0x438, the same way
//        as when you pick it. When the HUD shows again, "Quick Loaded" appears.
//      Every part waits for the state it expects and has a time limit; if anything is not as expected, DSfix stops
//      and you choose Load Game yourself ("Quick Load ready - choose Load Game"). Measured with the game: the list
//      states 3 -> 4 -> 6 (picked) or 6 with +0x448 = -1 (back) -> 0, then the load (save state 2).
//   If anything does not match, nothing is changed.

// The title menu's update stub (see "the automatic load" above): notes the dialog, then runs the game's update.
volatile DWORD g_QsTitleDlg = 0;            // the title menu dialog last updated
volatile LONG g_QsTitleDlgTick = 0;         // counts its updates
volatile DWORD g_QsTitleDlgUpdate = 0x00C77D90;

__declspec(naked) void qsTitleDlgUpdateStub() {
    __asm {
        mov dword ptr[g_QsTitleDlg], ecx
        lock inc dword ptr[g_QsTitleDlgTick]
        jmp dword ptr[g_QsTitleDlgUpdate]
    }
}

// The same for the character list.
volatile DWORD g_QsListDlg = 0;
volatile LONG g_QsListDlgTick = 0;
volatile DWORD g_QsListDlgUpdate = 0x00FAA8C0;

__declspec(naked) void qsListDlgUpdateStub() {
    __asm {
        mov dword ptr[g_QsListDlg], ecx
        lock inc dword ptr[g_QsListDlgTick]
        jmp dword ptr[g_QsListDlgUpdate]
    }
}

namespace {
    // ---- game addresses (this build only; checked at start-up) ----
    const DWORD kGameMan = 0x013784A0;
    const DWORD kWorldChrMan = 0x0137D644;
    const DWORD kNetSession = 0x0137D82C;
    const DWORD kSaveIconMan = 0x013786D0;   // [+0x248] = 1 shows the game's save icon
    const DWORD kMenuMan = 0x013786D0;       // the same object: [+0x238] = quit to the title, [+0x244] = close the menus
    const DWORD kSystemMan = 0x01378700;

    struct Signature { DWORD addr; const char* hex; };
    const Signature kSignatures[] = {
        { 0x00D5EE90, "a1a084370180b8400b0000007414b1018888100b000088880f0b00008888110b0000" }, // SaveRequest
        { 0x00EBAB46, "83ffff75068bb8700a000083" },                                             // profile save: slot +0xA70
        { 0x00D5FB70, "a144d6370185c074078b403c85c0750483c8ffc38b4070c3" },                     // local player [WorldChrMan+0x3C]
        { 0x00D5E2E0, "8b0d2cd8370133c083b990000000060f94c0c3" },                               // session state +0x90
        { 0x00EFB040, "51a1a08437018a880f0b00008a90100b0000538a98110b0000885c240484c9750484d2743a83b81c0b000000753184c9" },
        { 0x00C82C2B, "a1d0863701" "8b8048020000" "83f801" },                                 // save icon reads +0x248
    };
    // the automatic load (if these do not match, quick load stops at the title screen and you choose Load Game)
    const Signature kAutoSignatures[] = {
        // "PRESS START BUTTON" pressed -> +0x10C = 1, +0xF4 = 0
        { 0x00C77770, "516a0268ffc99a3bc6860c01000001e85cf0faff0f57c0f30f1186f400000059c3" },
        // the title menu's choice "Load Game" -> +0x108 = 3, +0x114 = 2
        { 0x00C77680, "518b86000100000f57c0f30f1186f4000000c786140100000200000085c0740a83f801741f83f80275156a026a64c7860801000002000000e823f1faff59c383f80375156a026a64c7860801000003000000e809f1faff59c3" },
        // the title menu after its fade: MenuMan+0x6C = 0, title flow = +0x108
        { 0x00C77B65, "8b8e100100008b86140100003bc8753f3ad3747083f90275238b8e080100008b15d0863701895a6c890d8c863701899e14010000899e100100005bc3" },
        // the character list copies its cursor into the chosen slot
        { 0x00C6E9AF, "a1d08637018b88800100008bd6898838040000" },
        // the list's cursor = row + scroll, every frame
        { 0x00FAA929, "8b87000100000387080100008b0dd0863701898180010000" },
        // the character list's "picked"
        { 0x00C1D375, "a1d08637016a026a64c7809000000006000000c7804804000001000000c6058a86370101" },
        // LoadGameStep STEP_SelectProfile: loads slot MenuMan+0x438 when +0x448 is 1
        { 0x00EEEA3A, "8b35d086370183be9000000000578bf9897c240c740d83be34040000010f8484000000d9ee51b808082e01d91c2433c9e8616ad5ff8b864804000083f8017547a1008737018b40308bb63804000085c0745583fe097750807c3010010f94c084c074448b0da08437018b3d04e2370189b1700a0000e84c19d0ff8b54240cc7420806000000" },
    };
    const DWORD kTitleDlgVtable = 0x011C28D4;
    const DWORD kTitleDlgUpdateSlot = kTitleDlgVtable + 16 * 4;
    const DWORD kTitleDlgUpdate = 0x00C77D90;
    const DWORD kListDlgVtable = 0x011C43D4;
    const DWORD kListDlgUpdateSlot = kListDlgVtable + 16 * 4;
    const DWORD kListDlgUpdate = 0x00FAA8C0;
    const DWORD kTitleFlow = 0x0137868C;
    const DWORD kListPicked = 0x0137868A;

    // quick load only (if these do not match, only quick load is off)
    const Signature kLoadSignatures[] = {
        // "Quit Game" answered yes: save request, then MenuMan+0x238 = 1
        { 0x00FA5928, "8b0d008737013959087421395e6c751ca1a084370138980c0b0000750f53899644020000e8bf3bf1ff83c404899638020000899ec4000000" },
        // the save request it calls (0x00EB9510)
        { 0x00EB9510, "a1a084370180b8400b0000007418b1018888100b000088880f0b00008a4c24048888110b0000c3" },
        // the in-game update reads MenuMan+0x238 ...
        { 0x00EFC038, "8b3dd08637010f94c383bf3802000000" },
        // ... waits for the save requests, clears it and leaves the game
        { 0x00EFC18E, "80be0f0b000000752080be100b000000751783be140b0000ff750ec7873802000000000000c6416101" },
        // the system menu greys out "Quit Game": more than one player, or GameMan+0xB4F == 0
        { 0x00FA61B5, "a1a0843701c686f4000000008b90ec0b0000b901000000394a107e06888ef400000080b84f0b0000007506888ef4000000" },
    };

    // ---- save file layout ----
    const DWORD kEntryCount = 11;
    const DWORD kEntrySize = 0x60014;
    const DWORD kMenuEntry = 10;
    const DWORD kUsedFlags = 0x28;
    const DWORD kSummaryBase = 0x38;
    const DWORD kSummarySize = 0x170;
    const DWORD kSlotNameOffset = 0x100;

#pragma pack(push, 1)
    struct QsHeader {
        char magic[8];          // "DSFIXQS1"
        DWORD version;          // 1
        DWORD slot;             // 0..9
        DWORD summarySize;      // kSummarySize
        DWORD entrySize;        // kEntrySize
        FILETIME savedAt;       // UTC
        wchar_t name[17];       // character name (for the log and for the quick load check)
        wchar_t pad;
        DWORD reserved[4];
    };
#pragma pack(pop)

    bool g_enabled = false;
    int g_key = 0;
    bool g_keyWasDown = false;

    enum State { ST_IDLE, ST_WAIT_SAVE, ST_COPY };
    State g_state = ST_IDLE;
    ULONGLONG g_startMs = 0;
    int g_slot = -1;
    bool g_sawBusy = false;

    struct Candidate { std::string path; FILETIME time; bool existed; };
    std::vector<Candidate> g_candidates;

    // worker result (written by the copy thread, read by the render thread)
    volatile LONG g_workDone = 0;      // 0 = running, 1 = success, 2 = failed
    char g_workMsg[256] = "";
    struct Job { std::string savePath; int slot; };

    // message on screen
    const char* g_msgText = NULL;
    D3DCOLOR g_msgColor = 0;
    float g_msgHold = 0.0f;
    ULONGLONG g_msgStartMs = 0;

    // quick load
    bool g_loadEnabled = false;
    int g_loadKey = 0;
    bool g_loadKeyWasDown = false;
    enum LoadState { QL_IDLE, QL_ASKED, QL_QUITTING, QL_WAIT_TITLE, QL_RESTORE,
                     QL_AUTO_MENU, QL_AUTO_FLOW, QL_AUTO_LIST, QL_AUTO_PICK, QL_AUTO_LOAD, QL_AUTO_PLAY };
    bool g_autoEnabled = false;
    ULONGLONG g_autoStableMs = 0;       // since when the state the automatic load waits for is there
    LONG g_titleDlgLastTick = 0;
    LONG g_listDlgLastTick = 0;
    LoadState g_qlState = QL_IDLE;
    ULONGLONG g_qlMs = 0;               // when the current quick load state began
    ULONGLONG g_qlReadySinceMs = 0;     // since when the title screen is showing and the game is not saving
    std::string g_qlSavePath;
    int g_qlSlot = -1;
    std::wstring g_qlName;
    std::vector<BYTE> g_qlData;         // the quick save file
    bool g_qlBackedUp = false;
    int g_qlTries = 0;

    ID3DXFont* g_font = NULL;
    ID3DXSprite* g_sprite = NULL;
    int g_fontHeight = 0;
    bool g_drawFailLogged = false;
    ULONGLONG g_iconUntilMs = 0;        // the game's save icon is held on until then
    bool g_iconOn = false;

    // ---- small helpers ----
    bool readMem(DWORD addr, void* out, size_t n) {
        __try { memcpy(out, (const void*)addr, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool readDword(DWORD addr, DWORD& v) { return readMem(addr, &v, 4); }
    bool readByte(DWORD addr, BYTE& v) { return readMem(addr, &v, 1); }
    bool writeDword(DWORD addr, DWORD v) {
        __try { *(volatile DWORD*)addr = v; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool writeByte(DWORD addr, BYTE v) {
        __try { *(volatile BYTE*)addr = v; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool hexMatches(DWORD addr, const char* hex) {
        const size_t n = strlen(hex) / 2;
        std::vector<BYTE> want(n), have(n);
        for (size_t i = 0; i < n; ++i) {
            unsigned v = 0;
            sscanf_s(hex + i * 2, "%2x", &v);
            want[i] = (BYTE)v;
        }
        if (!readMem(addr, &have[0], n)) return false;
        return memcmp(&want[0], &have[0], n) == 0;
    }

    int parseKey(std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t')) s.pop_back();
        while (!s.empty() && (s[0] == ' ' || s[0] == '\t')) s.erase(0, 1);
#define KEY(_name, _val) if (s == #_name) return _val;
#include "Keys.def"
#undef KEY
        return 0;
    }

    bool md5(const BYTE* data, DWORD len, BYTE out[16]) {
        HCRYPTPROV prov = 0;
        HCRYPTHASH hash = 0;
        bool ok = false;
        if (CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
            if (CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
                DWORD n = 16;
                ok = CryptHashData(hash, data, len, 0) && CryptGetHashParam(hash, HP_HASHVAL, out, &n, 0) && n == 16;
                CryptDestroyHash(hash);
            }
            CryptReleaseContext(prov, 0);
        }
        return ok;
    }

    bool fileTime(const std::string& path, FILETIME& t) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a)) return false;
        t = a.ftLastWriteTime;
        return true;
    }

    // Every place DRAKS0005.sl2 can be: the custom save folder from DSfix.ini, else Documents\NBGI\DarkSouls and its
    // sub-folders. The one the game writes during the quick save is the one in use.
    std::vector<std::string> candidatePaths() {
        std::vector<std::string> out;
        const std::string& custom = Settings::get().getCustomSaveFolder();
        if (custom != "none" && !custom.empty()) {
            out.push_back(custom + "\\DRAKS0005.sl2");
            return out;
        }
        char docs[MAX_PATH];
        if (FAILED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docs))) return out;
        const std::string base = std::string(docs) + "\\NBGI\\DarkSouls";
        out.push_back(base + "\\DRAKS0005.sl2");
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((base + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
                    out.push_back(base + "\\" + fd.cFileName + "\\DRAKS0005.sl2");
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        return out;
    }

    bool readWholeFile(const std::string& path, std::vector<BYTE>& data, DWORD& err) {
        err = 0;
        HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) { err = GetLastError(); return false; }
        LARGE_INTEGER size;
        bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 64 * 1024 * 1024;
        if (ok) {
            data.resize((size_t)size.QuadPart);
            DWORD got = 0;
            ok = ReadFile(f, &data[0], (DWORD)data.size(), &got, NULL) && got == data.size();
        }
        if (!ok) err = GetLastError();
        CloseHandle(f);
        return ok;
    }

    bool writeFileReplace(const std::string& path, const std::vector<BYTE>& data, char* why, size_t whyLen) {
        const std::string tmp = path + ".tmp";
        HANDLE f = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) { sprintf_s(why, whyLen, "could not create %s (error %lu)", tmp.c_str(), GetLastError()); return false; }
        DWORD put = 0;
        const bool ok = WriteFile(f, &data[0], (DWORD)data.size(), &put, NULL) && put == data.size() && FlushFileBuffers(f);
        CloseHandle(f);
        if (!ok) { DeleteFileA(tmp.c_str()); sprintf_s(why, whyLen, "could not write %s", tmp.c_str()); return false; }
        if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            sprintf_s(why, whyLen, "could not replace %s (error %lu)", path.c_str(), GetLastError());
            DeleteFileA(tmp.c_str());
            return false;
        }
        return true;
    }

    // Checks the BND4 layout of a save file and finds its 11 entries.
    bool parseSave(const std::vector<BYTE>& d, DWORD off[kEntryCount], char* why, size_t whyLen) {
        if (d.size() < 0x40 + kEntryCount * 0x20 || memcmp(&d[0], "BND4", 4) != 0) { sprintf_s(why, whyLen, "not a BND4 save file"); return false; }
        DWORD count = 0, entryHeader = 0;
        memcpy(&count, &d[0x0C], 4);
        memcpy(&entryHeader, &d[0x20], 4);
        if (count != kEntryCount || entryHeader != 0x20) { sprintf_s(why, whyLen, "unexpected layout (%lu entries, header %lu)", count, entryHeader); return false; }
        for (DWORD i = 0; i < kEntryCount; ++i) {
            DWORD size = 0;
            memcpy(&size, &d[0x40 + i * 0x20 + 8], 4);
            memcpy(&off[i], &d[0x40 + i * 0x20 + 16], 4);
            if (size != kEntrySize || (size_t)off[i] + kEntrySize > d.size()) { sprintf_s(why, whyLen, "entry %lu has an unexpected size or place", i); return false; }
        }
        return true;
    }

    // Takes the slot out of the save file and writes the quick save file. 'why' explains a failure.
    bool extractSlot(const std::vector<BYTE>& d, int slot, std::vector<BYTE>& qs, char* why, size_t whyLen, std::wstring& name) {
        DWORD off[kEntryCount];
        if (!parseSave(d, off, why, whyLen)) return false;
        const BYTE* slotEntry = &d[off[slot]];
        const BYTE* menu = &d[off[kMenuEntry]];
        BYTE h[16];
        if (!md5(slotEntry + 16, kEntrySize - 16, h) || memcmp(h, slotEntry, 16) != 0) { sprintf_s(why, whyLen, "slot %d checksum does not match (file still being written?)", slot); return false; }
        if (!md5(menu + 16, kEntrySize - 16, h) || memcmp(h, menu, 16) != 0) { sprintf_s(why, whyLen, "menu data checksum does not match (file still being written?)"); return false; }
        if (menu[kUsedFlags + slot] == 0) { sprintf_s(why, whyLen, "slot %d is marked empty in the save file", slot); return false; }
        const BYTE* summary = menu + kSummaryBase + slot * kSummarySize;
        // the summary record starts with the character name, the same as in the slot entry: proves the layout
        wchar_t sumName[17] = {}, slotName[17] = {};
        memcpy(sumName, summary, 32);
        memcpy(slotName, slotEntry + kSlotNameOffset, 32);
        if (!sumName[0] || wcsncmp(sumName, slotName, 16) != 0) { sprintf_s(why, whyLen, "slot %d: the name in the load menu data does not match the character", slot); return false; }
        name = sumName;

        QsHeader hd = {};
        memcpy(hd.magic, "DSFIXQS1", 8);
        hd.version = 1;
        hd.slot = (DWORD)slot;
        hd.summarySize = kSummarySize;
        hd.entrySize = kEntrySize;
        GetSystemTimeAsFileTime(&hd.savedAt);
        memcpy(hd.name, sumName, sizeof(hd.name));
        qs.resize(sizeof(hd) + kSummarySize + kEntrySize);
        memcpy(&qs[0], &hd, sizeof(hd));
        memcpy(&qs[sizeof(hd)], summary, kSummarySize);
        memcpy(&qs[sizeof(hd) + kSummarySize], slotEntry, kEntrySize);
        return true;
    }

    DWORD WINAPI copyThread(LPVOID param) {
        Job* job = (Job*)param;
        char why[256] = "";
        bool ok = false;
        std::wstring name;
        // The game may still hold the file for a moment; a few tries, 0.2 s apart.
        for (int attempt = 0; attempt < 10 && !ok; ++attempt) {
            if (attempt) Sleep(200);
            std::vector<BYTE> data;
            DWORD err = 0;
            if (!readWholeFile(job->savePath, data, err)) { sprintf_s(why, "could not read %s (error %lu)", job->savePath.c_str(), err); continue; }
            std::vector<BYTE> qs;
            if (!extractSlot(data, job->slot, qs, why, sizeof(why), name)) continue;
            const size_t cut = job->savePath.find_last_of('\\');
            const std::string dir = job->savePath.substr(0, cut) + "\\QuickSave";
            CreateDirectoryA(dir.c_str(), NULL);
            if (!writeFileReplace(dir + "\\QS_DRAKS0005", qs, why, sizeof(why))) continue;
            char nm[64] = "";
            WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, nm, sizeof(nm), NULL, NULL);
            sprintf_s(why, "slot %d (\"%s\") copied to %s\\QS_DRAKS0005", job->slot, nm, dir.c_str());
            ok = true;
        }
        strcpy_s(g_workMsg, why);
        delete job;
        InterlockedExchange(&g_workDone, ok ? 1 : 2);
        return 0;
    }

    void showMessage(const char* text, D3DCOLOR color, float hold = QuickSaveLook::kHoldSeconds) {
        g_msgText = text;
        g_msgColor = color;
        g_msgHold = hold;
        g_msgStartMs = GetTickCount64();
    }

    // Why a quick save is not possible right now, or NULL when it is. 'inGame' tells whether to show a message at all.
    const char* blockedReason(int& slot, bool& inGame) {
        inGame = hudFramesSinceSeen() <= 5;
        if (!inGame) return "not in game (no HUD drawn: title screen, loading screen or cutscene)";
        DWORD gm = 0, wcm = 0, player = 0, s = 0, net = 0, netState = 0, hp = 0;
        BYTE allowed = 0, req0 = 0, req1 = 0, busy = 0;
        if (!readDword(kGameMan, gm) || !gm) return "the game manager is not there";
        if (!readDword(gm + 0xA70, s) || (int)s < 0 || s > 9) return "no character slot is loaded";
        if (!readByte(gm + 0xB40, allowed) || !allowed) return "the game does not allow saving right now";
        readByte(gm + 0xB0F, req0); readByte(gm + 0xB10, req1); readByte(gm + 0xB1C, busy);
        if (req0 || req1 || busy) return "the game is already saving";
        if (!readDword(kWorldChrMan, wcm) || !wcm || !readDword(wcm + 0x3C, player) || !player) return "no player character";
        if (!readDword(player + 0x2D4, hp) || (int)hp <= 0) return "the character is dead";
        if (readDword(kNetSession, net) && net && readDword(net + 0x90, netState) && netState >= 3 && netState <= 6)
            return "another player is in the world (or joining)";
        slot = (int)s;
        return NULL;
    }

    void startQuickSave() {
        int slot = -1;
        bool inGame = false;
        const char* why = blockedReason(slot, inGame);
        if (why) {
            SDLOG(0, "QuickSave: not now - %s\n", why);
            if (inGame) showMessage(QuickSaveLook::kBlockedText, QuickSaveLook::kFailColor);
            return;
        }
        g_candidates.clear();
        const std::vector<std::string> paths = candidatePaths();
        for (size_t i = 0; i < paths.size(); ++i) {
            Candidate c;
            c.path = paths[i];
            c.existed = fileTime(c.path, c.time);
            if (!c.existed) memset(&c.time, 0, sizeof(c.time));
            g_candidates.push_back(c);
        }
        DWORD gm = 0, net = 0, netState = 0xFFFFFFFF;
        readDword(kGameMan, gm);
        if (readDword(kNetSession, net) && net) readDword(net + 0x90, netState);
        // the same three bytes the game's own "SaveRequest" script command sets
        if (!gm || !writeByte(gm + 0xB10, 1) || !writeByte(gm + 0xB0F, 1) || !writeByte(gm + 0xB11, 1)) {
            SDLOG(0, "QuickSave: could not request the save\n");
            showMessage(QuickSaveLook::kFailedText, QuickSaveLook::kFailColor);
            return;
        }
        g_slot = slot;
        g_startMs = GetTickCount64();
        g_iconUntilMs = g_startMs + (ULONGLONG)(QuickSaveLook::kGameIconSeconds * 1000.0f);
        g_sawBusy = false;
        g_state = ST_WAIT_SAVE;
        SDLOG(0, "QuickSave: save requested for slot %d (session state %ld, %u possible save files)\n", slot, (long)netState, (unsigned)g_candidates.size());
    }

    void followQuickSave() {
        const ULONGLONG now = GetTickCount64();
        if (g_state == ST_WAIT_SAVE) {
            DWORD gm = 0;
            BYTE r0 = 1, r1 = 1, busy = 1;
            if (readDword(kGameMan, gm) && gm) { readByte(gm + 0xB0F, r0); readByte(gm + 0xB10, r1); readByte(gm + 0xB1C, busy); }
            if (busy) g_sawBusy = true;
            if (!r0 && !r1 && !busy && now - g_startMs > 150) {
                // the game has finished its save: which file did it write?
                for (size_t i = 0; i < g_candidates.size(); ++i) {
                    FILETIME t;
                    if (!fileTime(g_candidates[i].path, t)) continue;
                    if (!g_candidates[i].existed || CompareFileTime(&t, &g_candidates[i].time) != 0) {
                        Job* job = new Job;
                        job->savePath = g_candidates[i].path;
                        job->slot = g_slot;
                        g_workDone = 0;
                        HANDLE th = CreateThread(NULL, 0, copyThread, job, 0, NULL);
                        if (!th) {
                            delete job;
                            SDLOG(0, "QuickSave: could not start the copy\n");
                            showMessage(QuickSaveLook::kFailedText, QuickSaveLook::kFailColor);
                            g_state = ST_IDLE;
                            return;
                        }
                        CloseHandle(th);
                        SDLOG(0, "QuickSave: the game saved after %.2f s (%s), copying slot %d from %s\n",
                            (now - g_startMs) / 1000.0, g_sawBusy ? "write seen" : "write not seen", g_slot, job->savePath.c_str());
                        g_state = ST_COPY;
                        return;
                    }
                }
            }
            if (now - g_startMs > 15000) {
                SDLOG(0, "QuickSave: failed - the game did not write its save file within 15 s (requests %d %d, writing %d)\n", r0, r1, busy);
                showMessage(QuickSaveLook::kFailedText, QuickSaveLook::kFailColor);
                g_state = ST_IDLE;
            }
        }
        else if (g_state == ST_COPY) {
            const LONG done = g_workDone;
            if (done == 0) return;
            SDLOG(0, "QuickSave: %s - %s\n", done == 1 ? "done" : "failed", g_workMsg);
            if (done == 1) showMessage(QuickSaveLook::kSavedText, QuickSaveLook::kColor);
            else showMessage(QuickSaveLook::kFailedText, QuickSaveLook::kFailColor);
            g_state = ST_IDLE;
        }
    }

    // ================================================================================================================
    // QUICK LOAD
    // ================================================================================================================

    bool sameName(const wchar_t* a, const wchar_t* b) { return a[0] && wcsncmp(a, b, 16) == 0; }

    // Checks a quick save file: layout, the slot entry's MD5, and the same name in the header, the summary and the slot.
    bool validQuickSave(const std::vector<BYTE>& qs, QsHeader& hd, char* why, size_t whyLen) {
        if (qs.size() != sizeof(QsHeader) + kSummarySize + kEntrySize) { sprintf_s(why, whyLen, "the quick save file has the wrong size (%u bytes)", (unsigned)qs.size()); return false; }
        memcpy(&hd, &qs[0], sizeof(hd));
        if (memcmp(hd.magic, "DSFIXQS1", 8) != 0 || hd.version != 1 || hd.slot > 9 || hd.summarySize != kSummarySize || hd.entrySize != kEntrySize) {
            sprintf_s(why, whyLen, "the quick save file is not one DSfix wrote");
            return false;
        }
        hd.name[16] = 0;
        const BYTE* summary = &qs[sizeof(hd)];
        const BYTE* entry = summary + kSummarySize;
        BYTE h[16];
        if (!md5(entry + 16, kEntrySize - 16, h) || memcmp(h, entry, 16) != 0) { sprintf_s(why, whyLen, "the quick save file is damaged (checksum)"); return false; }
        wchar_t sumName[17] = {}, slotName[17] = {};
        memcpy(sumName, summary, 32);
        memcpy(slotName, entry + kSlotNameOffset, 32);
        if (!sameName(hd.name, sumName) || !sameName(hd.name, slotName)) { sprintf_s(why, whyLen, "the names in the quick save file do not match"); return false; }
        return true;
    }

    std::string narrow(const std::wstring& w) {
        char nm[64] = "";
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nm, sizeof(nm), NULL, NULL);
        return nm;
    }

    // Why a quick load is not possible right now (NULL when it is). 'noSave' = the reason is the quick save file itself.
    // Fills g_qlSavePath, g_qlSlot, g_qlName and g_qlData.
    const char* loadBlockedReason(bool& inGame, bool& noSave, char* buf, size_t bufLen) {
        noSave = false;
        int slot = -1;
        const char* why = blockedReason(slot, inGame);
        if (why) return why;
        DWORD gm = 0, players = 0, playerList = 0;
        BYTE quitOk = 0;
        readDword(kGameMan, gm);
        if (!readByte(gm + 0xB4F, quitOk) || !quitOk) return "the game does not allow Quit Game right now";
        if (readDword(gm + 0xBEC, playerList) && playerList && readDword(playerList + 0x10, players) && players > 1)
            return "more than one player is in the world";

        // the quick save next to the save file (if there are several save folders: the newest quick save)
        const std::vector<std::string> paths = candidatePaths();
        std::string savePath, qsPath;
        FILETIME best = {};
        for (size_t i = 0; i < paths.size(); ++i) {
            const std::string dir = paths[i].substr(0, paths[i].find_last_of('\\'));
            const std::string q = dir + "\\QuickSave\\QS_DRAKS0005";
            FILETIME t, ts;
            if (!fileTime(q, t) || !fileTime(paths[i], ts)) continue;
            if (qsPath.empty() || CompareFileTime(&t, &best) > 0) { best = t; qsPath = q; savePath = paths[i]; }
        }
        noSave = true;
        if (qsPath.empty()) return "there is no quick save (QuickSave\\QS_DRAKS0005 next to the save file)";
        std::vector<BYTE> qs;
        DWORD err = 0;
        if (!readWholeFile(qsPath, qs, err)) { sprintf_s(buf, bufLen, "could not read %s (error %lu)", qsPath.c_str(), err); return buf; }
        QsHeader hd;
        if (!validQuickSave(qs, hd, buf, bufLen)) return buf;
        if ((int)hd.slot != slot) { sprintf_s(buf, bufLen, "the quick save is of slot %lu, the character in play is in slot %d", hd.slot, slot); return buf; }
        // the character in play: its name in the save file (the game wrote it when it last saved this slot)
        std::vector<BYTE> d;
        if (!readWholeFile(savePath, d, err)) { sprintf_s(buf, bufLen, "could not read %s (error %lu)", savePath.c_str(), err); return buf; }
        DWORD off[kEntryCount];
        if (!parseSave(d, off, buf, bufLen)) return buf;
        wchar_t current[17] = {};
        memcpy(current, &d[off[slot] + kSlotNameOffset], 32);
        if (!sameName(hd.name, current)) {
            sprintf_s(buf, bufLen, "the quick save is of \"%s\", the character in play is \"%s\"", narrow(hd.name).c_str(), narrow(current).c_str());
            return buf;
        }
        noSave = false;
        g_qlSavePath = savePath;
        g_qlSlot = slot;
        g_qlName = hd.name;
        g_qlData.swap(qs);
        return NULL;
    }

    void quickLoadFail(const char* why) {
        SDLOG(0, "QuickSave: quick load failed - %s\n", why);
        showMessage(QuickSaveLook::kLoadFailedText, QuickSaveLook::kFailColor);
        g_qlState = QL_IDLE;
        g_qlData.clear();
    }

    // First press: checks and asks. Second press within kConfirmSeconds: quits to the title screen like "Quit Game".
    void quickLoadKeyPressed() {
        using namespace QuickSaveLook;
        if (g_state != ST_IDLE || (g_qlState != QL_IDLE && g_qlState != QL_ASKED)) return; // a quick save or load is running
        bool inGame = false, noSave = false;
        char buf[300] = "";
        const char* why = loadBlockedReason(inGame, noSave, buf, sizeof(buf));
        if (why) {
            SDLOG(0, "QuickSave: quick load not now - %s\n", why);
            if (inGame) showMessage(noSave ? kLoadNoSaveText : kLoadBlockedText, kFailColor);
            g_qlState = QL_IDLE;
            g_qlData.clear();
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (g_qlState == QL_IDLE) {
            g_qlState = QL_ASKED;
            g_qlMs = now;
            float hold = kConfirmSeconds - kFadeInSeconds - kFadeOutSeconds;
            if (hold < 0.5f) hold = 0.5f;
            showMessage(kLoadAskText, kAskColor, hold);
            SDLOG(0, "QuickSave: quick load asked for slot %d (\"%s\")\n", g_qlSlot, narrow(g_qlName).c_str());
            return;
        }
        // confirmed: what the system menu's "Quit Game" does when you answer yes
        DWORD gm = 0, mm = 0, sys = 0, sys8 = 0, mm6c = 0;
        BYTE b0c = 1, b40 = 0;
        if (!readDword(kGameMan, gm) || !gm || !readDword(kMenuMan, mm) || !mm || !readDword(kSystemMan, sys) || !sys) { quickLoadFail("the game's managers are not there"); return; }
        readDword(sys + 8, sys8);
        readDword(mm + 0x6C, mm6c);
        readByte(gm + 0xB0C, b0c);
        readByte(gm + 0xB40, b40);
        bool saves = false;
        if (sys8 && !mm6c && !b0c) {
            writeDword(mm + 0x244, 1);
            if (b40) { writeByte(gm + 0xB10, 1); writeByte(gm + 0xB0F, 1); writeByte(gm + 0xB11, 0); saves = true; }
        }
        if (!writeDword(mm + 0x238, 1)) { quickLoadFail("could not ask the game to quit"); return; }
        g_qlState = QL_QUITTING;
        g_qlMs = now;
        g_qlBackedUp = false;
        g_qlTries = 0;
        showMessage(kLoadingText, kAskColor, 120.0f);
        SDLOG(0, "QuickSave: quick load confirmed - quitting to the title screen (%s)\n", saves ? "the game saves first" : "the game does not save now");
    }

    // On the title screen: keeps the save file as it is, then puts the quick save's slot back. 'retry' = worth another try.
    bool restoreSlot(char* why, size_t whyLen, bool& retry) {
        retry = true;
        std::vector<BYTE> d;
        DWORD err = 0;
        if (!readWholeFile(g_qlSavePath, d, err)) { sprintf_s(why, whyLen, "could not read %s (error %lu)", g_qlSavePath.c_str(), err); return false; }
        DWORD off[kEntryCount];
        if (!parseSave(d, off, why, whyLen)) return false;
        BYTE* menu = &d[off[kMenuEntry]];
        BYTE h[16];
        if (!md5(menu + 16, kEntrySize - 16, h) || memcmp(h, menu, 16) != 0) { sprintf_s(why, whyLen, "menu data checksum does not match (file still being written?)"); return false; }
        BYTE* slotEntry = &d[off[g_qlSlot]];
        wchar_t onDisk[17] = {};
        memcpy(onDisk, slotEntry + kSlotNameOffset, 32);
        if (!sameName(g_qlName.c_str(), onDisk)) {
            retry = false;
            sprintf_s(why, whyLen, "slot %d in the save file is now \"%s\", not \"%s\" - nothing changed", g_qlSlot, narrow(onDisk).c_str(), narrow(g_qlName).c_str());
            return false;
        }
        const size_t cut = g_qlSavePath.find_last_of('\\');
        const std::string dir = g_qlSavePath.substr(0, cut) + "\\QuickSave";
        // 1. keep the save file as it is now
        if (!g_qlBackedUp) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char name[96];
            sprintf_s(name, "\\BeforeQuickLoad%02u%02u%02u%02u%02u%02u_DRAKS0005.sl2.bak", st.wYear % 100, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            CreateDirectoryA(dir.c_str(), NULL);
            if (!writeFileReplace(dir + name, d, why, whyLen)) return false;
            g_qlBackedUp = true;
            SDLOG(0, "QuickSave: the save file was kept as %s%s\n", dir.c_str(), name);
            // only the 20 newest
            std::vector<std::string> old;
            WIN32_FIND_DATAA fd;
            HANDLE fh = FindFirstFileA((dir + "\\BeforeQuickLoad*_DRAKS0005.sl2.bak").c_str(), &fd);
            if (fh != INVALID_HANDLE_VALUE) {
                do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) old.push_back(fd.cFileName); } while (FindNextFileA(fh, &fd));
                FindClose(fh);
            }
            std::sort(old.begin(), old.end());
            for (size_t i = 0; i + 20 < old.size(); ++i) DeleteFileA((dir + "\\" + old[i]).c_str());
        }
        // 2. the slot and its summary from the quick save; the slot is marked used; the menu data's MD5 again
        memcpy(slotEntry, &g_qlData[sizeof(QsHeader) + kSummarySize], kEntrySize);
        memcpy(menu + kSummaryBase + g_qlSlot * kSummarySize, &g_qlData[sizeof(QsHeader)], kSummarySize);
        menu[kUsedFlags + g_qlSlot] = 1;
        if (!md5(menu + 16, kEntrySize - 16, menu)) { sprintf_s(why, whyLen, "could not compute the checksum"); return false; }
        if (!writeFileReplace(g_qlSavePath, d, why, whyLen)) return false;
        // 3. read it back
        std::vector<BYTE> back;
        if (!readWholeFile(g_qlSavePath, back, err) || back != d) { retry = false; sprintf_s(why, whyLen, "the save file does not read back as written - restore it from the BeforeQuickLoad file in %s", dir.c_str()); return false; }
        sprintf_s(why, whyLen, "slot %d (\"%s\") put back into %s", g_qlSlot, narrow(g_qlName).c_str(), g_qlSavePath.c_str());
        return true;
    }

    // The automatic load stops: you choose Load Game yourself.
    void autoLoadStop(const char* why) {
        SDLOG(0, "QuickSave: automatic load stopped - %s; choose Load Game yourself\n", why);
        showMessage(QuickSaveLook::kLoadReadyText, QuickSaveLook::kColor, QuickSaveLook::kReadyHoldSeconds);
        g_qlState = QL_IDLE;
    }

    // true when the state has been there for 'ms' without a break
    bool stableFor(bool there, ULONGLONG now, ULONGLONG ms) {
        if (!there) { g_autoStableMs = 0; return false; }
        if (!g_autoStableMs) g_autoStableMs = now;
        return now - g_autoStableMs >= ms;
    }

    void autoNext(LoadState st, ULONGLONG now) {
        g_qlState = st;
        g_qlMs = now;
        g_autoStableMs = 0;
    }

    void followAutoLoad(ULONGLONG now) {
        DWORD mm = 0, gm = 0, flow = 0xFFFFFFFF;
        readDword(kMenuMan, mm);
        readDword(kGameMan, gm);
        readDword(kTitleFlow, flow);
        DWORD list = 0xFFFFFFFF, mode = 0, chosen = 0xFFFFFFFF;
        if (mm) { readDword(mm + 0x90, list); readDword(mm + 0x434, mode); readDword(mm + 0x438, chosen); }
        const ULONGLONG inState = now - g_qlMs;
        switch (g_qlState) {
        case QL_AUTO_MENU: {
            if (inState > 20000) { autoLoadStop("the title menu was not ready within 20 s"); break; }
            // the title menu, updated since the last frame, not closing, on the title screen
            const LONG tick = g_QsTitleDlgTick;
            const bool updated = tick != g_titleDlgLastTick;
            g_titleDlgLastTick = tick;
            const DWORD dlg = g_QsTitleDlg;
            DWORD vt = 0, closeNow = 1, closeWant = 1;
            BYTE items = 0xFF, loadOk = 0;
            const bool menu = updated && dlg && readDword(dlg, vt) && vt == kTitleDlgVtable && readByte(dlg + 0x10C, items) &&
                readDword(dlg + 0x110, closeNow) && !closeNow && readDword(dlg + 0x114, closeWant) && !closeWant &&
                flow == 0 && titleScreenShowing() && hudFramesSinceSeen() > 30;
            if (menu && items == 0) {
                // "PRESS START BUTTON": what pressing a button there does (0x00C77770)
                if (stableFor(true, now, 700)) {
                    if (!writeByte(dlg + 0x10C, 1) || !writeDword(dlg + 0xF4, 0)) { autoLoadStop("could not press start"); break; }
                    SDLOG(0, "QuickSave: automatic load - PRESS START BUTTON pressed\n");
                    g_autoStableMs = 0;
                }
                break;
            }
            // the menu shows its items and offers "Load Game"
            const bool ready = menu && items && readByte(dlg + 0x118, loadOk) && loadOk;
            if (stableFor(ready, now, 700)) {
                // what choosing "Load Game" does (0x00C77680)
                const float zero = 0.0f;
                DWORD zeroBits;
                memcpy(&zeroBits, &zero, 4);
                if (!writeDword(dlg + 0x100, 3) || !writeDword(dlg + 0xF4, zeroBits) || !writeDword(dlg + 0x108, 3) || !writeDword(dlg + 0x114, 2)) {
                    autoLoadStop("could not choose Load Game");
                    break;
                }
                SDLOG(0, "QuickSave: automatic load - Load Game chosen\n");
                autoNext(QL_AUTO_FLOW, now);
            }
            break;
        }
        case QL_AUTO_FLOW:
            if (flow == 3) autoNext(QL_AUTO_LIST, now);
            else if (inState > 4000) autoLoadStop("the title menu did not start Load Game within 4 s");
            break;
        case QL_AUTO_LIST: {
            // the character list is open and waits (state 4, load mode), its list part updated since the last frame
            const LONG tick = g_QsListDlgTick;
            const bool updated = tick != g_listDlgLastTick;
            g_listDlgLastTick = tick;
            const DWORD ld = g_QsListDlg;
            DWORD vt = 0, row = 0, scroll = 0, count = 0;
            const bool ready = updated && ld && readDword(ld, vt) && vt == kListDlgVtable && readDword(ld + 0x100, row) &&
                readDword(ld + 0x108, scroll) && readDword(ld + 0x104, count) && count == 10 && row <= 3 && scroll <= 6 &&
                list == 4 && mode == 1 && flow == 3;
            if (stableFor(ready, now, 300)) {
                // the row of the slot; the scroll only if the slot is not in view
                const DWORD slot = (DWORD)g_qlSlot;
                if (slot < scroll || slot > scroll + 3) {
                    scroll = slot > 6 ? 6 : slot;
                    if (!writeDword(ld + 0x108, scroll)) { autoLoadStop("could not scroll the list"); break; }
                }
                if (!writeDword(ld + 0x100, slot - scroll)) { autoLoadStop("could not move the cursor"); break; }
                autoNext(QL_AUTO_PICK, now);
            }
            else if (inState > 8000) autoLoadStop("the character list did not open within 8 s");
            break;
        }
        case QL_AUTO_PICK:
            if (list != 4 || mode != 1) { autoLoadStop("the character list changed before the pick"); break; }
            if (chosen == (DWORD)g_qlSlot) {
                // what picking a character does (0x00C1D375)
                if (!writeDword(mm + 0x448, 1) || !writeByte(kListPicked, 1) || !writeDword(mm + 0x90, 6)) { autoLoadStop("could not pick the character"); break; }
                SDLOG(0, "QuickSave: automatic load - slot %d picked\n", g_qlSlot);
                autoNext(QL_AUTO_LOAD, now);
            }
            else if (inState > 500) autoLoadStop("the character list did not take the slot");
            break;
        case QL_AUTO_LOAD: {
            DWORD slot = 0xFFFFFFFF, busy = 0;
            if (gm) { readDword(gm + 0xA70, slot); readDword(gm + 0xB1C, busy); }
            if (busy == 2) {
                if ((int)slot == g_qlSlot) { SDLOG(0, "QuickSave: automatic load - the game is loading slot %d\n", g_qlSlot); }
                else { SDLOG(0, "QuickSave: automatic load - the game is loading slot %ld, not %d\n", (long)slot, g_qlSlot); }
                autoNext(QL_AUTO_PLAY, now);
            }
            else if (inState > 8000) autoLoadStop("the game did not start loading within 8 s");
            break;
        }
        case QL_AUTO_PLAY:
            if (hudFramesSinceSeen() <= 5) {
                showMessage(QuickSaveLook::kLoadedText, QuickSaveLook::kColor);
                SDLOG(0, "QuickSave: automatic load - playing again\n");
                g_qlState = QL_IDLE;
            }
            else if (inState > 120000) g_qlState = QL_IDLE;   // loaded, but no HUD seen (a cutscene?): nothing more to do
            break;
        default:
            break;
        }
    }

    void followQuickLoad() {
        using namespace QuickSaveLook;
        const ULONGLONG now = GetTickCount64();
        DWORD gm = 0, mm = 0, quit = 0;
        BYTE r0 = 1, r1 = 1, busy = 1;
        if (readDword(kGameMan, gm) && gm) { readByte(gm + 0xB0F, r0); readByte(gm + 0xB10, r1); readByte(gm + 0xB1C, busy); }
        switch (g_qlState) {
        case QL_ASKED:
            if (now - g_qlMs > (ULONGLONG)(kConfirmSeconds * 1000.0f)) {
                SDLOG(0, "QuickSave: quick load not confirmed\n");
                g_qlState = QL_IDLE;
                g_qlData.clear();
            }
            break;
        case QL_QUITTING:
            // the game clears MenuMan+0x238 when it starts leaving the game
            if (readDword(kMenuMan, mm) && mm && readDword(mm + 0x238, quit) && quit == 0) {
                SDLOG(0, "QuickSave: the game is quitting to the title screen (after %.2f s)\n", (now - g_qlMs) / 1000.0);
                g_qlState = QL_WAIT_TITLE;
                g_qlMs = now;
                g_qlReadySinceMs = 0;
            }
            else if (now - g_qlMs > 10000) {
                if (mm && quit) writeDword(mm + 0x238, 0); // not taken: take the request back
                quickLoadFail("the game did not start quitting within 10 s; nothing changed");
            }
            break;
        case QL_WAIT_TITLE: {
            // the title screen shows, no HUD, and the game is not saving - for one second
            const bool ready = titleScreenShowing() && hudFramesSinceSeen() > 30 && !r0 && !r1 && !busy;
            if (!ready) g_qlReadySinceMs = 0;
            else if (!g_qlReadySinceMs) g_qlReadySinceMs = now;
            if (g_qlReadySinceMs && now - g_qlReadySinceMs >= 1000) {
                g_qlState = QL_RESTORE;
                g_qlMs = now;
            }
            else if (now - g_qlMs > 90000) quickLoadFail("the title screen was not reached within 90 s; nothing changed");
            break;
        }
        case QL_RESTORE: {
            // done here, on the title screen, in one go: the game cannot load a character in the middle of it
            if (now - g_qlMs < (ULONGLONG)g_qlTries * 250) break;    // a failed try: again after 0.25 s
            if (!titleScreenShowing() || hudFramesSinceSeen() <= 30) { quickLoadFail("the title screen was left before the quick save was put back; nothing changed"); break; }
            char why[400] = "";
            bool retry = true;
            const bool ok = restoreSlot(why, sizeof(why), retry);
            ++g_qlTries;
            if (ok) {
                SDLOG(0, "QuickSave: quick load done - %s\n", why);
                g_qlData.clear();
                if (g_autoEnabled) {
                    g_qlState = QL_AUTO_MENU;
                    g_qlMs = now;
                    g_autoStableMs = 0;
                    g_titleDlgLastTick = g_QsTitleDlgTick;
                    g_listDlgLastTick = g_QsListDlgTick;
                }
                else {
                    showMessage(kLoadReadyText, kColor, kReadyHoldSeconds);
                    g_qlState = QL_IDLE;
                }
            }
            else if (!retry || g_qlTries >= 10) quickLoadFail(why);
            else SDLOG(0, "QuickSave: quick load try %d - %s\n", g_qlTries, why);
            break;
        }
        default:
            if (g_qlState >= QL_AUTO_MENU) followAutoLoad(now);
            break;
        }
    }

    // Holds the game's save icon on (see the top of this file). Only while the game is not writing a save itself: then
    // the game sets the flag every frame on its own.
    void updateGameIcon() {
        DWORD gm = 0, busy = 0, man = 0;
        if (!readDword(kGameMan, gm) || !gm || !readDword(gm + 0xB1C, busy) || busy) return;
        if (!readDword(kSaveIconMan, man) || !man) return;
        const bool want = GetTickCount64() < g_iconUntilMs;
        if (want) { writeDword(man + 0x248, 1); g_iconOn = true; }
        else {
            if (g_iconOn) writeDword(man + 0x248, 0);
            g_iconOn = false;
            g_iconUntilMs = 0;
        }
    }

    D3DCOLOR withAlpha(D3DCOLOR c, float f) {
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        const DWORD a = (DWORD)((float)(c >> 24) * f + 0.5f);
        return (c & 0x00FFFFFF) | (a << 24);
    }

    void drawMessage(IDirect3DDevice9* dev) {
        using namespace QuickSaveLook;
        if (!g_msgText || !dev) return;
        const float t = (GetTickCount64() - g_msgStartMs) / 1000.0f;
        if (t >= kFadeInSeconds + g_msgHold + kFadeOutSeconds) { g_msgText = NULL; return; }
        float alpha;
        if (t < kFadeInSeconds) alpha = t / kFadeInSeconds;   // 0 on the first frame: nothing to draw yet
        else if (t < kFadeInSeconds + g_msgHold) alpha = 1.0f;
        else alpha = 1.0f - (t - kFadeInSeconds - g_msgHold) / kFadeOutSeconds;
        if (alpha < 0.01f) return;
        const char* text = g_msgText;
        const D3DCOLOR color = g_msgColor;

        IDirect3DSurface9* bb = NULL;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
        D3DSURFACE_DESC desc;
        bb->GetDesc(&desc);
        const float W = (float)desc.Width, H = (float)desc.Height;
        int fontHeight = (int)(kHeight * H + 0.5f);
        if (fontHeight < 10) fontHeight = 10;
        if (!g_font || g_fontHeight != fontHeight) {
            quickSaveRelease();
            if (FAILED(D3DXCreateFontA(dev, fontHeight, 0, kFontWeight, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, kFontName, &g_font))) {
                g_font = NULL;
                bb->Release();
                return;
            }
            if (FAILED(D3DXCreateSprite(dev, &g_sprite))) g_sprite = NULL;
            g_fontHeight = fontHeight;
        }
        // Everything the game left set is saved in a state block and put back afterwards; the states that matter for
        // drawing text onto the back buffer are set explicitly (in game the game leaves shaders, depth and blend states
        // set at Present that the font's own sprite does not reset).
        IDirect3DStateBlock9* sb = NULL;
        if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) { bb->Release(); return; }
        IDirect3DSurface9* oldRT = NULL;
        IDirect3DSurface9* oldDS = NULL;
        dev->GetRenderTarget(0, &oldRT);
        dev->GetDepthStencilSurface(&oldDS);
        IDirect3DSurface9* oldMrt[3] = { NULL, NULL, NULL };
        for (DWORD i = 1; i < 4; ++i) {
            if (SUCCEEDED(dev->GetRenderTarget(i, &oldMrt[i - 1])) && oldMrt[i - 1]) dev->SetRenderTarget(i, NULL);
        }
        dev->SetRenderTarget(0, bb);              // also sets the viewport to the whole back buffer
        dev->SetDepthStencilSurface(NULL);         // the game's depth buffer may be smaller than the back buffer
        dev->SetVertexShader(NULL);
        dev->SetPixelShader(NULL);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        // If the game still has a scene open at Present, BeginScene fails but drawing inside that scene works.
        const HRESULT hrScene = dev->BeginScene();
        const bool ownScene = SUCCEEDED(hrScene);
        int drawn = 0;
        {
            int shadow = (int)(kShadowOffset * H + 0.5f);
            if (shadow < 1) shadow = 1;
            RECT r = { (LONG)(kLeft * W + 0.5f), (LONG)(kTop * H + 0.5f), (LONG)desc.Width, (LONG)desc.Height };
            // a text too long for the place right of the health bar moves left, so it stays on the screen
            RECT calc = { 0, 0, 0, 0 };
            g_font->DrawTextA(NULL, text, -1, &calc, DT_CALCRECT | DT_SINGLELINE | DT_LEFT | DT_TOP, 0);
            const LONG maxLeft = (LONG)((1.0f - kRightMargin) * W + 0.5f) - (calc.right - calc.left);
            if (r.left > maxLeft) r.left = maxLeft > 0 ? maxLeft : 0;
            RECT rs = { r.left + shadow, r.top + shadow, r.right + shadow, r.bottom + shadow };
            const DWORD fmt = DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP;
            if (g_sprite && SUCCEEDED(g_sprite->Begin(D3DXSPRITE_ALPHABLEND))) {
                g_font->DrawTextA(g_sprite, text, -1, &rs, fmt, withAlpha(kShadow, alpha));
                drawn = g_font->DrawTextA(g_sprite, text, -1, &r, fmt, withAlpha(color, alpha));
                g_sprite->End();
            }
            if (ownScene) dev->EndScene();
        }
        if (drawn == 0 && !g_drawFailLogged) {
            g_drawFailLogged = true;
            SDLOG(0, "QuickSave: the message could not be drawn (BeginScene 0x%08lX)\n", (unsigned long)hrScene);
        }
        // the render targets first: setting one resets the viewport, which the state block then puts back
        for (DWORD i = 1; i < 4; ++i) {
            if (oldMrt[i - 1]) { dev->SetRenderTarget(i, oldMrt[i - 1]); oldMrt[i - 1]->Release(); }
        }
        if (oldRT) { dev->SetRenderTarget(0, oldRT); oldRT->Release(); }
        dev->SetDepthStencilSurface(oldDS);
        if (oldDS) oldDS->Release();
        sb->Apply();
        sb->Release();
        bb->Release();
    }
}

void quickSaveInit() {
    if (!Settings::get().getQuickSave()) return;
    for (size_t i = 0; i < sizeof(kSignatures) / sizeof(kSignatures[0]); ++i) {
        if (!hexMatches(kSignatures[i].addr, kSignatures[i].hex)) {
            SDLOG(0, "QuickSave: the game's code at 0x%08lX does not match this build, quick save is off\n", kSignatures[i].addr);
            return;
        }
    }
    g_key = parseKey(Settings::get().getQuickSaveKey());
    if (!g_key) {
        SDLOG(0, "QuickSave: quickSaveKey \"%s\" is not a key name (use names like VK_F5), quick save is off\n", Settings::get().getQuickSaveKey().c_str());
        return;
    }
    g_enabled = true;
    SDLOG(0, "QuickSave: on, key 0x%02X\n", g_key);

    const std::string loadKey = Settings::get().getQuickLoadKey();
    if (loadKey.empty() || loadKey == "none") return;
    for (size_t i = 0; i < sizeof(kLoadSignatures) / sizeof(kLoadSignatures[0]); ++i) {
        if (!hexMatches(kLoadSignatures[i].addr, kLoadSignatures[i].hex)) {
            SDLOG(0, "QuickSave: the game's code at 0x%08lX does not match this build, quick load is off\n", kLoadSignatures[i].addr);
            return;
        }
    }
    g_loadKey = parseKey(loadKey);
    if (!g_loadKey || g_loadKey == g_key) {
        SDLOG(0, "QuickSave: quickLoadKey \"%s\" is not a key name or is the quick save key, quick load is off\n", loadKey.c_str());
        g_loadKey = 0;
        return;
    }
    g_loadEnabled = true;
    SDLOG(0, "QuickSave: quick load on, key 0x%02X\n", g_loadKey);

    // the automatic load: its game code, then the title menu's and the list's update slots pointed at the stubs
    for (size_t i = 0; i < sizeof(kAutoSignatures) / sizeof(kAutoSignatures[0]); ++i) {
        if (!hexMatches(kAutoSignatures[i].addr, kAutoSignatures[i].hex)) {
            SDLOG(0, "QuickSave: the game's code at 0x%08lX does not match this build, no automatic load (choose Load Game yourself)\n", kAutoSignatures[i].addr);
            return;
        }
    }
    DWORD slotValue = 0, oldProtect = 0;
    if (!readDword(kTitleDlgUpdateSlot, slotValue) || slotValue != kTitleDlgUpdate) {
        SDLOG(0, "QuickSave: the title menu's update is not where expected, no automatic load\n");
        return;
    }
    if (!readDword(kListDlgUpdateSlot, slotValue) || slotValue != kListDlgUpdate) {
        SDLOG(0, "QuickSave: the character list's update is not where expected, no automatic load\n");
        return;
    }
    if (!VirtualProtect((LPVOID)kTitleDlgUpdateSlot, 4, PAGE_READWRITE, &oldProtect)) {
        SDLOG(0, "QuickSave: could not hook the title menu (error %lu), no automatic load\n", GetLastError());
        return;
    }
    *(volatile DWORD*)kTitleDlgUpdateSlot = (DWORD)&qsTitleDlgUpdateStub;
    VirtualProtect((LPVOID)kTitleDlgUpdateSlot, 4, oldProtect, &oldProtect);
    if (!VirtualProtect((LPVOID)kListDlgUpdateSlot, 4, PAGE_READWRITE, &oldProtect)) {
        SDLOG(0, "QuickSave: could not hook the character list (error %lu), no automatic load\n", GetLastError());
        return;   // the title menu stub alone does nothing without g_autoEnabled
    }
    *(volatile DWORD*)kListDlgUpdateSlot = (DWORD)&qsListDlgUpdateStub;
    VirtualProtect((LPVOID)kListDlgUpdateSlot, 4, oldProtect, &oldProtect);
    g_autoEnabled = true;
    SDLOG(0, "QuickSave: automatic load on\n");
}

void quickSaveFrame(IDirect3DDevice9* dev) {
    if (!g_enabled) return;
    // the key, only while the game window has the focus (as for the other DSfix keys)
    HWND fg = ::GetForegroundWindow();
    DWORD pid = 0;
    if (fg) ::GetWindowThreadProcessId(fg, &pid);
    const bool focused = fg && pid == ::GetCurrentProcessId();
    const bool down = focused && (::GetAsyncKeyState(g_key) & 0x8000) != 0;
    const bool loading = g_qlState != QL_IDLE && g_qlState != QL_ASKED;
    if (down && !g_keyWasDown && g_state == ST_IDLE && !loading) {
        g_qlState = QL_IDLE; // a quick save cancels an unanswered quick load question
        g_qlData.clear();
        startQuickSave();
    }
    g_keyWasDown = down;
    if (g_loadEnabled) {
        const bool loadDown = focused && (::GetAsyncKeyState(g_loadKey) & 0x8000) != 0;
        if (loadDown && !g_loadKeyWasDown) quickLoadKeyPressed();
        g_loadKeyWasDown = loadDown;
        if (g_qlState != QL_IDLE) followQuickLoad();
    }
    if (g_state != ST_IDLE) followQuickSave();
    if (g_iconUntilMs || g_iconOn) updateGameIcon();
    drawMessage(dev);
}

void quickSaveRelease() {
    if (g_sprite) {
        g_sprite->Release();
        g_sprite = NULL;
    }
    if (g_font) {
        g_font->Release();
        g_font = NULL;
    }
    g_fontHeight = 0;
}
