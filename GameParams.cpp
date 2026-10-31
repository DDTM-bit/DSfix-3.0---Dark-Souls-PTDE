#include "GameParams.h"
#include "Settings.h" // SDLOG reads the log level when RELEASE_VER is not defined
#include "main.h"
#include <windows.h>
#include <cstring>

// Game parameter tables in memory (research: the Dark Souls Overhaul team, metal-crow & Sean Pesce, for the table
// headers and layouts; the search and checks are DSfix's own).
//
// A loaded .param file starts with a header: +0x06 two 16-bit values 1, 1 (for these two tables), +0x0A the number of
// rows, +0x0C the table name ("EQUIP_PARAM_GOODS_ST" / "EQUIP_PARAM_WEAPON_ST"), and from +0x30 one 12-byte entry per
// row: { row id, offset of the row from the start of the file, offset of its name }. The rows follow, all the same size.
//
// The tables are found by searching readable memory for the name, once the game has loaded them (a few seconds after
// start-up). A hit counts only if the whole header is consistent: row offsets evenly spaced and inside the memory block,
// ids in increasing order, the expected row size, and known rows present. Anything else is ignored.

namespace {
    struct ParamTable {
        const uint8_t* base;
        uint32_t count;
        uint32_t stride;
    };

    struct RowInfo {
        uint32_t id;
        uint32_t offset;
        uint32_t nameOffset;
    };

    // EquipParamGoods: one row is 0x5C bytes. Byte 0x45 holds the flags isAutoEquip (bit 0), isEstablishment,
    // isOnlyOne, isDrop, isDeposit, isDisableHand, IsTravelItem, isSuppleItem. The Estus Flasks are ids 200-215.
    const char kGoodsName[] = "EQUIP_PARAM_GOODS_ST";
    const uint32_t kGoodsStride = 0x5C;
    const uint32_t kGoodsAutoEquipByte = 0x45;
    const uint8_t kGoodsAutoEquipBit = 0x01;
    const uint32_t kEstusFirst = 200, kEstusLast = 215;

    // EquipParamWeapon: the maximum durability ("durabilityMax", 16 bit) is at +0xBE of a row, the starting durability
    // ("durability") at +0xBC.
    const char kWeaponName[] = "EQUIP_PARAM_WEAPON_ST";
    const uint32_t kWeaponDurability = 0xBC;
    const uint32_t kWeaponDurabilityMax = 0xBE;

    // Published by the search thread once found (the table memory stays valid for the whole session).
    const uint8_t* volatile g_weaponBase = NULL;
    volatile uint32_t g_weaponCount = 0;

    bool readableProtect(DWORD p) {
        if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
            PAGE_EXECUTE_WRITECOPY)) != 0;
    }

    // Checks a candidate header. 'avail' = bytes readable from 'base' to the end of its memory region.
    // wantStride: required row size (0 = any). Called inside __try by the caller.
    bool checkTable(const uint8_t* base, size_t avail, uint32_t wantStride, ParamTable* out) {
        if (avail < 0x30 + 12) return false;
        if (base[6] != 1 || base[7] != 0 || base[8] != 1 || base[9] != 0) return false;
        const uint32_t count = *(const uint16_t*)(base + 0x0A);
        if (count < 16 || count > 20000) return false;
        if (avail < 0x30 + (size_t)count * 12) return false;
        const RowInfo* info = (const RowInfo*)(base + 0x30);
        const uint32_t stride = info[1].offset - info[0].offset;
        if (stride < 16 || stride > 0x1000) return false;
        if (wantStride && stride != wantStride) return false;
        if (info[0].offset < 0x30 + count * 12) return false;
        for (uint32_t i = 1; i < count; ++i) {
            if (info[i].offset - info[i - 1].offset != stride) return false;
            if (info[i].id <= info[i - 1].id) return false;
        }
        if ((size_t)info[count - 1].offset + stride > avail) return false;
        out->base = base;
        out->count = count;
        out->stride = stride;
        return true;
    }

    // Searches one memory region for a table name; fills up to maxOut valid tables. No C++ objects here (__try).
    int searchRegion(const uint8_t* start, size_t size, const char* name, size_t nameLen, uint32_t wantStride,
        ParamTable* out, int maxOut) {
        int found = 0;
        __try {
            if (size < 0x0C + nameLen + 1) return 0;
            const uint8_t* p = start + 0x0C;
            const uint8_t* end = start + size - nameLen - 1;
            while (p < end && found < maxOut) {
                p = (const uint8_t*)memchr(p, name[0], end - p);
                if (!p) break;
                if (memcmp(p, name, nameLen + 1) == 0) { // the name and its terminating 0
                    const uint8_t* base = p - 0x0C;
                    ParamTable t;
                    if (checkTable(base, (size_t)(start + size - base), wantStride, &t)) out[found++] = t;
                }
                ++p;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // the region was released while being read: whatever was found before stays valid
        }
        return found;
    }

    // Searches all readable, committed, non-image memory. Returns the number of valid tables (up to maxOut).
    int findTables(const char* name, uint32_t wantStride, ParamTable* out, int maxOut) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const uint8_t* p = (const uint8_t*)si.lpMinimumApplicationAddress;
        const uint8_t* maxAddr = (const uint8_t*)si.lpMaximumApplicationAddress;
        const size_t nameLen = strlen(name);
        int found = 0;
        while (p < maxAddr && found < maxOut) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
            const uint8_t* regionStart = (const uint8_t*)mbi.BaseAddress;
            const uint8_t* regionEnd = regionStart + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Type != MEM_IMAGE && readableProtect(mbi.Protect)) {
                found += searchRegion(regionStart, mbi.RegionSize, name, nameLen, wantStride, out + found, maxOut - found);
            }
            if (regionEnd <= p) break;
            p = regionEnd;
        }
        return found;
    }

    bool hasRow(const ParamTable& t, uint32_t id) {
        const RowInfo* info = (const RowInfo*)(t.base + 0x30);
        for (uint32_t i = 0; i < t.count; ++i) if (info[i].id == id) return true;
        return false;
    }

    // Weapon table: the durability fields must look right in most rows (start <= max, max between 1 and 9999).
    bool weaponLayoutPlausible(const ParamTable& t) {
        if (t.stride < kWeaponDurabilityMax + 2) return false;
        const RowInfo* info = (const RowInfo*)(t.base + 0x30);
        uint32_t good = 0;
        for (uint32_t i = 0; i < t.count; ++i) {
            const uint8_t* row = t.base + info[i].offset;
            const uint16_t start = *(const uint16_t*)(row + kWeaponDurability);
            const uint16_t mx = *(const uint16_t*)(row + kWeaponDurabilityMax);
            if (mx >= 1 && mx <= 9999 && start <= mx) ++good;
        }
        return good * 10 >= t.count * 9;
    }

    // Clears isAutoEquip in every row except the Estus Flasks. Returns the number of rows changed.
    int disableAutoEquip(const ParamTable& t) {
        int changed = 0;
        __try {
            const RowInfo* info = (const RowInfo*)(t.base + 0x30);
            for (uint32_t i = 0; i < t.count; ++i) {
                if (info[i].id >= kEstusFirst && info[i].id <= kEstusLast) continue;
                uint8_t* flags = (uint8_t*)(t.base + info[i].offset + kGoodsAutoEquipByte);
                if (*flags & kGoodsAutoEquipBit) {
                    *flags &= (uint8_t)~kGoodsAutoEquipBit;
                    ++changed;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1; // not writable: left as it is
        }
        return changed;
    }

    DWORD WINAPI gameParamsThread(LPVOID) {
        bool needGoods = Settings::get().getDisableItemAutoEquip();
        bool needWeapon = Settings::get().getWeaponWearBars();
        Sleep(2000);
        // The tables are loaded during start-up; try every 2 s for 5 minutes, then give up.
        for (int attempt = 0; attempt < 150 && (needGoods || needWeapon); ++attempt) {
            if (needGoods) {
                ParamTable t[8];
                const int n = findTables(kGoodsName, kGoodsStride, t, 8);
                int tables = 0, rows = 0;
                for (int i = 0; i < n; ++i) {
                    if (!hasRow(t[i], kEstusFirst)) continue; // must be the real item table
                    const int c = disableAutoEquip(t[i]);
                    if (c >= 0) { ++tables; rows += c; }
                }
                if (tables > 0) {
                    needGoods = false;
                    SDLOG(0, "disableItemAutoEquip: auto-equip turned off for %d items (%d table copies; Estus unchanged)\n", rows, tables);
                }
            }
            if (needWeapon) {
                ParamTable t[8];
                const int n = findTables(kWeaponName, 0, t, 8);
                for (int i = 0; i < n; ++i) {
                    if (!weaponLayoutPlausible(t[i])) continue;
                    g_weaponCount = t[i].count;
                    g_weaponBase = t[i].base; // published last
                    needWeapon = false;
                    SDLOG(0, "weaponWearBars: weapon table found (%u weapons)\n", t[i].count);
                    break;
                }
            }
            if (needGoods || needWeapon) Sleep(2000);
        }
        if (needGoods) SDLOG(0, "disableItemAutoEquip: item table not found, auto-equip unchanged\n");
        if (needWeapon) SDLOG(0, "weaponWearBars: weapon table not found, no durability bars\n");
        return 0;
    }
}

void startGameParamsThread() {
    static bool started = false;
    if (started) return;
    if (!Settings::get().getDisableItemAutoEquip() && !Settings::get().getWeaponWearBars()) return;
    started = true;
    HANDLE h = CreateThread(NULL, 0, gameParamsThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

bool weaponMaxDurability(uint32_t weaponId, uint32_t* maxDurability) {
    const uint8_t* base = g_weaponBase;
    const uint32_t count = g_weaponCount;
    if (!base || count == 0 || !maxDurability) return false;
    // The row of the weapon as it is; if there is none, the row without the upgrade level (the last two digits)
    const RowInfo* info = (const RowInfo*)(base + 0x30);
    const uint32_t ids[2] = { weaponId, weaponId - weaponId % 100 };
    for (int k = 0; k < 2; ++k) {
        uint32_t lo = 0, hi = count;
        while (lo < hi) { // ids are in increasing order (checked when the table was found)
            const uint32_t mid = (lo + hi) / 2;
            if (info[mid].id < ids[k]) lo = mid + 1;
            else hi = mid;
        }
        if (lo < count && info[lo].id == ids[k]) {
            *maxDurability = *(const uint16_t*)(base + info[lo].offset + kWeaponDurabilityMax);
            return *maxDurability > 0;
        }
    }
    return false;
}

// =====================================================================================================================
// WeaponsArmorSort - sort the Weapons and Armor lists of the Inventory, the Equipment "select item" list and the
// Bottomless Box by one of the six values shown on every row.
//
//   Controller: hold RS, D-pad left / right picks the value, D-pad up / down switches DESC / ASC, release RS sorts.
//   Keyboard:   hold LALT, left / right arrow picks the value, up / down arrow switches DESC / ASC, release LALT sorts.
//   Choices (both lists remember their own value and order): 0 game order, 1 physical, 2 magic, 3 fire, 4 lightning
//   (attack for weapons, defense for armor), 5 durability, 6 weight, 7 storage (Bottomless Box only: how many are in
//   the box; elsewhere it counts as game order). DESC = highest first (the default); for game order DESC is the game's
//   own order and ASC the same list reversed.
//   Ties: weight (lightest first), durability (highest first), name, upgrade level (highest first), game order.
//   For storage, ties simply keep the game's order.
//
// How it works: every one of these windows builds its list in two steps - it collects the items of the current tab
// or slot into a list of entries, then puts them in order (a second list: the order on screen). Right after the game
// has done both, DSfix re-orders the second list. The rows, numbers, icons, cursor and scrolling are all the game's
// own; only the order changes. Asking the game to rebuild a list uses the game's own two "rebuild" switches.
// The values come from the same parameter rows and the same lookups the game uses to print the row, so the order
// always matches the numbers on screen (weapon attack cut to a whole number, armor defense rounded, as printed).
//
// The choice is kept in %LOCALAPPDATA%\NBGI\DarkSouls\DSfixSort.ini (next to DarkSouls.ini). A missing or broken
// file is written again quietly; if it cannot be written, the choice is only kept while the game runs.
// =====================================================================================================================
#include <cstdio>
#include <cmath>

// QuickSelectEstus (further down) is told about every pad read by the XInput hook below.
namespace estus { void padSeen(DWORD idx, WORD buttons); }

namespace wasort {
    // ---------------------------------------------------------------------------------------------------------------
    // Addresses in DARKSOULS.exe (Steam build, PE timestamp 0x557F2FD0)
    // ---------------------------------------------------------------------------------------------------------------
    const DWORD kMenuMan = 0x013786D0;           // [kMenuMan] = menu manager; +0x1F0 != 0: the inventory shows other data
    const DWORD kGameDataMan = 0x01378700;       // [[kGameDataMan] + 8] = the player's data
    const DWORD kVtInventory = 0x011C4DEC;       // FrpgMenuDlgInventory
    const DWORD kVtRepository = 0x011C6644;      // FrpgMenuDlgRepository (Bottomless Box)
    const DWORD kVtEquipList = 0x011B995C;       // FrpgMenuDlgEquipList ("Select item to equip")
    const DWORD kFnWeaponLookup = 0x00D1A950;    // esi = weapon id, [esp+4] = out: +4 weapon row, +0x10 upgrade rates
    const DWORD kFnWeaponName = 0x00D1A800;      // eax = that out block -> name text
    const DWORD kFnArmorLookup = 0x00D1C3C0;     // the same for armor
    const DWORD kFnArmorName = 0x00D1C370;
    const DWORD kXInputGetStateImport = 0x010CC3E8;

    // The list object (shared base FrpgMenuDlgEquipListCommon)
    const DWORD kListCursor = 0x100;             // cursor row on screen
    const DWORD kListScroll = 0x108;             // first row on screen (cursor index = cursor + scroll)
    const DWORD kListEntries = 0x134;            // vector of entries (begin +4, end +8)
    const DWORD kListOrder = 0x138;              // vector: the order on screen, as positions in the entries
    const DWORD kListNeedUpdate = 0x144;         // rebuild switches the game itself uses
    const DWORD kListNeedCollect = 0x145;

    // Parameter rows
    const DWORD kWepWeight = 0x0C, kWepDurability = 0xBC, kWepAttack = 0xC4;      // attack: 4 x u16 (P M F L)
    const DWORD kProWeight = 0x20, kProDurability = 0xAA, kProDefense = 0xB2;     // defense: 4 x u16 (P M F L)

    // Inventory (player data + 0x1B8): +0x110 slots, +0x118 split, +0x120 / +0x124 the two blocks, 28 bytes a slot:
    // +0 kind, +4 id, +0x14 durability. Bottomless Box ([player data + 0x420] + 8): 32 bytes an entry, +0 kind | id.
    const DWORD kKindWeapon = 0x00000000, kKindArmor = 0x10000000;

    // ---------------------------------------------------------------------------------------------------------------
    // Hooks. All are checked against these bytes first; one wrong byte and nothing at all is patched.
    // ---------------------------------------------------------------------------------------------------------------
    //  1. rebuild starts:     cmp byte ptr [esi+0x145],1 / jne +0x2A       (notes which list, tab and slot)
    //  2. collect step:       mov byte ptr [esi+0x145],0 / call eax         (re-orders the list after it)
    //  3. list takes input:   fld dword ptr [esp+8] / push ecx              (only reached while the list has focus)
    //  4. keyboard read:      cmp eax,0x8007001E                            (right after the game reads the keys)
    //  5. XInput:             jmp dword ptr [XInputGetState]                (every pad read of the game goes here)
    const BYTE kExpRefresh[9] = { 0x80,0xBE,0x45,0x01,0x00,0x00,0x01, 0x75,0x2A };
    const BYTE kExpCollect[9] = { 0xC6,0x86,0x45,0x01,0x00,0x00,0x00, 0xFF,0xD0 };
    const BYTE kExpTick[5] = { 0xD9,0x44,0x24,0x08, 0x51 };
    const BYTE kExpKeyboard[5] = { 0x3D,0x1E,0x00,0x07,0x80 };
    const BYTE kExpXInput[6] = { 0xFF,0x25,0xE8,0xC3,0x0C,0x01 };
    DWORD g_refreshCont = 0x00CA098E;  // after the compare: needs collecting
    DWORD g_refreshSkip = 0x00CA09B8;  // where the original jne went
    DWORD g_collectCont = 0x00CA09B8;
    DWORD g_tickCont = 0x00C8AFAF;
    DWORD g_keyboardCont = 0x006447CD; // the jne that uses the compare

    bool g_installed = false;
    bool g_refreshPatched = false;     // hook 1 is in place (WeaponsArmorSort or BottomlessBoxMarkers put it there)
    bool g_xinputPatched = false;      // hook 5 is in place (WeaponsArmorSort or QuickSelectEstus put it there)
    bool g_markersInstalled = false;   // BottomlessBoxMarkers

    // ---------------------------------------------------------------------------------------------------------------
    // The remembered choice and the file
    // ---------------------------------------------------------------------------------------------------------------
    const int kChoices = 7;                       // 0 game order, 1..4 the four attack/defense values, 5 durability, 6 weight
    const int kChoicesBox = 8;                    // the Bottomless Box also has 7 storage
    const int kChoiceStorage = 7;
    volatile int g_choice[3] = { 0, 0, 0 };      // [1] weapons, [2] armor
    volatile int g_desc[3] = { 1, 1, 1 };        // 1 = DESC (default), 0 = ASC
    volatile LONG g_saveBusy = 0;
    volatile LONG g_saveDirty = 0;

    bool sortFilePath(wchar_t* dir1, wchar_t* dir2, wchar_t* file, wchar_t* tmp, DWORD size) {
        wchar_t base[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 64) return false;
        _snwprintf_s(dir1, size, _TRUNCATE, L"%s\\NBGI", base);
        _snwprintf_s(dir2, size, _TRUNCATE, L"%s\\NBGI\\DarkSouls", base);
        _snwprintf_s(file, size, _TRUNCATE, L"%s\\NBGI\\DarkSouls\\DSfixSort.ini", base);
        _snwprintf_s(tmp, size, _TRUNCATE, L"%s\\NBGI\\DarkSouls\\DSfixSort.ini.tmp", base);
        return true;
    }

    // Writes both choices. Any failure is silent: the choice then simply lives only while the game runs.
    void writeSortFile() {
        wchar_t dir1[MAX_PATH], dir2[MAX_PATH], file[MAX_PATH], tmp[MAX_PATH];
        if (!sortFilePath(dir1, dir2, file, tmp, MAX_PATH)) return;
        CreateDirectoryW(dir1, NULL);
        CreateDirectoryW(dir2, NULL);
        char text[64];
        const int len = _snprintf_s(text, sizeof(text), _TRUNCATE, "Weapons=%d\r\nWeaponsOrder=%s\r\nArmor=%d\r\nArmorOrder=%s\r\n",
            g_choice[1], g_desc[1] ? "DESC" : "ASC", g_choice[2], g_desc[2] ? "DESC" : "ASC");
        HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        const BOOL ok = len > 0 && WriteFile(h, text, (DWORD)len, &written, NULL) && written == (DWORD)len;
        CloseHandle(h);
        if (!ok || !MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(tmp);
            return;
        }
    }

    DWORD WINAPI saveThread(LPVOID) {
        do {
            while (InterlockedExchange(&g_saveDirty, 0)) writeSortFile();
            InterlockedExchange(&g_saveBusy, 0);
        } while (g_saveDirty && InterlockedCompareExchange(&g_saveBusy, 1, 0) == 0);
        return 0;
    }

    // Asks for the file to be written, on a short-lived thread of its own (never on the game's thread).
    void requestSave() {
        InterlockedExchange(&g_saveDirty, 1);
        if (InterlockedCompareExchange(&g_saveBusy, 1, 0) != 0) return; // a writer is running and will see the flag
        HANDLE h = CreateThread(NULL, 0, saveThread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        else InterlockedExchange(&g_saveBusy, 0);
    }

    // "DESC" -> 1, "ASC" -> 0, anything else -> -1
    int parseOrder(const char* v) {
        if (_stricmp(v, "DESC") == 0) return 1;
        if (_stricmp(v, "ASC") == 0) return 0;
        return -1;
    }

    // Reads "Weapons=n", "WeaponsOrder=DESC|ASC", "Armor=n" and "ArmorOrder=DESC|ASC". Returns false when the file is
    // missing or anything in it is wrong (an older file with only the two value lines counts as wrong).
    bool readSortFile(int* weapons, int* armor, int* weaponsDesc, int* armorDesc) {
        wchar_t dir1[MAX_PATH], dir2[MAX_PATH], file[MAX_PATH], tmp[MAX_PATH];
        *weapons = 0; *armor = 0; *weaponsDesc = 1; *armorDesc = 1;
        if (!sortFilePath(dir1, dir2, file, tmp, MAX_PATH)) return false;
        HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return false;
        char text[256];
        DWORD got = 0;
        const BOOL ok = ReadFile(h, text, sizeof(text) - 1, &got, NULL);
        CloseHandle(h);
        if (!ok) return false;
        text[got] = 0;
        bool haveW = false, haveA = false, haveWo = false, haveAo = false, good = true;
        char* line = text;
        while (*line) {
            char* end = line;
            while (*end && *end != '\n' && *end != '\r') ++end;
            const char keep = *end;
            *end = 0;
            if (*line) {
                int value = -1;
                char extra = 0;
                if (_strnicmp(line, "Weapons=", 8) == 0 && sscanf_s(line + 8, "%d%c", &value, &extra, 1) == 1 &&
                    value >= 0 && value < kChoicesBox) { *weapons = value; haveW = true; }
                else if (_strnicmp(line, "Armor=", 6) == 0 && sscanf_s(line + 6, "%d%c", &value, &extra, 1) == 1 &&
                    value >= 0 && value < kChoicesBox) { *armor = value; haveA = true; }
                else if (_strnicmp(line, "WeaponsOrder=", 13) == 0 && (value = parseOrder(line + 13)) >= 0) { *weaponsDesc = value; haveWo = true; }
                else if (_strnicmp(line, "ArmorOrder=", 11) == 0 && (value = parseOrder(line + 11)) >= 0) { *armorDesc = value; haveAo = true; }
                else good = false;
            }
            if (!keep) break;
            line = end + 1;
        }
        return good && haveW && haveA && haveWo && haveAo;
    }

    DWORD WINAPI loadThread(LPVOID) {
        int w = 0, a = 0, wd = 1, ad = 1;
        const bool good = readSortFile(&w, &a, &wd, &ad);
        g_choice[1] = w;
        g_choice[2] = a;
        g_desc[1] = wd;
        g_desc[2] = ad;
        if (!good) requestSave();
        return 0;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Safe reads
    // ---------------------------------------------------------------------------------------------------------------
    DWORD rd32(DWORD a) {
        DWORD v = 0;
        __try { v = *(const DWORD*)a; }
        __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
        return v;
    }
    DWORD playerData() {
        const DWORD top = rd32(kGameDataMan);
        return top ? rd32(top + 8) : 0;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Which lists are ours. Noted at every rebuild (hook 1); read every frame (hook 3).
    // kind: 0 not ours, 1 weapons (shields included, ammo not), 2 armor.
    // ---------------------------------------------------------------------------------------------------------------
    enum Window { W_NONE = 0, W_INVENTORY, W_BOX, W_EQUIP };
    struct ListInfo { DWORD list; int window; int kind; };
    const int kListCache = 8;
    ListInfo g_lists[kListCache];
    int g_listNext = 0;

    int windowOf(DWORD list) {
        const DWORD vt = rd32(list);
        if (vt == kVtInventory) return W_INVENTORY;
        if (vt == kVtRepository) return W_BOX;
        if (vt == kVtEquipList) return W_EQUIP;
        return W_NONE;
    }

    // frame = ebp of the rebuild function: [frame+8] slot (Equipment), [frame+0xC] tab category (4 weapons, 6 armor).
    int kindOf(DWORD list, DWORD frame, int* windowOut) {
        const int window = windowOf(list);
        if (windowOut) *windowOut = window;
        if (window == W_NONE) return 0;
        const int slot = (int)rd32(frame + 8);
        const int category = (int)rd32(frame + 0xC);
        if (window == W_EQUIP) {
            if (slot >= 0 && slot <= 3) return 1;   // left / right hand 1 and 2
            if (slot >= 8 && slot <= 11) return 2;  // head, chest, hands, legs
            return 0;                               // ammo, rings, quick items
        }
        if (window == W_INVENTORY) {
            const DWORD mm = rd32(kMenuMan);
            if (!mm || rd32(mm + 0x1F0) != 0) return 0; // the inventory showing other data: not touched
        }
        if (category == 4) return 1;
        if (category == 6) return 2;
        return 0;
    }

    void rememberList(DWORD list, int window, int kind) {
        for (int i = 0; i < kListCache; ++i) {
            if (g_lists[i].list == list) { g_lists[i].window = window; g_lists[i].kind = kind; return; }
        }
        g_lists[g_listNext].list = list;
        g_lists[g_listNext].window = window;
        g_lists[g_listNext].kind = kind;
        g_listNext = (g_listNext + 1) % kListCache;
    }

    int cachedKind(DWORD list) {
        for (int i = 0; i < kListCache; ++i) if (g_lists[i].list == list) return g_lists[i].kind;
        return 0;
    }

    // The list that took input most recently (only ours are noted).
    volatile DWORD g_focusList = 0;
    volatile int g_focusKind = 0;
    volatile int g_focusWindow = 0;
    volatile DWORD g_focusTime = 0;
    const DWORD kFocusMs = 120;                   // no input step for this long: the list has lost focus

    int focusedKind() {
        if (!g_focusKind) return 0;
        if (GetTickCount() - g_focusTime > kFocusMs) return 0;
        return g_focusKind;
    }

    // Requests from the input side to the game side
    volatile int g_resortKind = 0;                // a list of this kind must be rebuilt
    volatile DWORD g_toTopList = 0;               // after rebuilding this list, put the cursor on the first row

    // ---------------------------------------------------------------------------------------------------------------
    // Choosing (input side)
    // ---------------------------------------------------------------------------------------------------------------
    volatile bool g_padMode = false;              // RS held in a list
    volatile bool g_kbMode = false;               // LALT held in a list
    volatile int g_modeKind = 0;
    volatile int g_temp = 0;                      // the choice shown while held
    volatile int g_tempDesc = 1;                  // the order shown while held
    volatile int g_modeWindow = 0;                // the window the choosing started in
    int g_startChoice = 0, g_startDesc = 1;       // what was shown when the button went down
    volatile bool g_lastKeyboard = true;          // without DDTM: keyboard / mouse used last (for the prompt)

    // The value shown for a list: storage outside the Bottomless Box counts as game order.
    int shownChoice(int kind, int window) {
        const int c = g_choice[kind];
        return (c == kChoiceStorage && window != W_BOX) ? 0 : c;
    }

    void beginChoosing(int kind) {
        g_modeKind = kind;
        g_modeWindow = g_focusWindow;
        g_temp = shownChoice(kind, g_modeWindow);
        g_tempDesc = g_desc[kind];
        g_startChoice = g_temp;
        g_startDesc = g_tempDesc;
    }
    void stepChoice(int dir) {
        const int n = g_modeWindow == W_BOX ? kChoicesBox : kChoices;
        g_temp = (g_temp + dir + n) % n;
    }
    void toggleOrder() {
        g_tempDesc = g_tempDesc ? 0 : 1;
    }
    // commit: the button was let go while the list still had focus. Nothing changes if nothing was chosen.
    void endChoosing(bool commit) {
        const int kind = g_modeKind;
        if (!commit || kind == 0) return;
        if (g_temp == g_startChoice && g_tempDesc == g_startDesc) return;
        g_choice[kind] = g_temp;
        g_desc[kind] = g_tempDesc;
        g_resortKind = kind;
        requestSave();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Controller (hook 5). XINPUT_STATE without the XInput header.
    // ---------------------------------------------------------------------------------------------------------------
    struct PadState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
    typedef DWORD(WINAPI* XInputGetState_t)(DWORD, PadState*);
    const WORD kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008, kPadRS = 0x0080;
    WORD g_padPrev[4] = { 0, 0, 0, 0 };
    DWORD g_padIdx = 0;
    WORD g_padWaitUp = 0;                         // buttons still held when RS was released: hidden until let go

    void padFilter(DWORD idx, PadState* s) {
        const WORD raw = s->buttons;
        const WORD prev = g_padPrev[idx];
        g_padPrev[idx] = raw;
        const int kind = focusedKind();
        // the prompt follows the device used last (without DDTM): a new button or a stick / trigger pushed
        if ((raw & (WORD)~prev) || s->lt > 30 || s->rt > 30 || s->lx > 8000 || s->lx < -8000 || s->ly > 8000 ||
            s->ly < -8000 || s->rx > 8000 || s->rx < -8000 || s->ry > 8000 || s->ry < -8000) g_lastKeyboard = false;

        if (!g_padMode && !g_kbMode && kind && (raw & kPadRS)) {
            g_padMode = true;
            g_padIdx = idx;
            beginChoosing(kind);
        }
        if (g_padMode && idx == g_padIdx) {
            if (!(raw & kPadRS)) {
                g_padMode = false;
                g_padWaitUp = raw;
                endChoosing(kind == g_modeKind);
            }
            else if (kind != g_modeKind) {
                g_padMode = false;
                g_padWaitUp = raw;
                endChoosing(false);
            }
            else {
                if ((raw & kPadLeft) && !(prev & kPadLeft)) stepChoice(-1);
                if ((raw & kPadRight) && !(prev & kPadRight)) stepChoice(+1);
                if ((raw & kPadUp) && !(prev & kPadUp)) toggleOrder();
                if ((raw & kPadDown) && !(prev & kPadDown)) toggleOrder();
            }
        }
        if (g_padMode) {
            // everything is hidden from the game while RS is held (every pad)
            s->buttons = 0; s->lt = 0; s->rt = 0; s->lx = 0; s->ly = 0; s->rx = 0; s->ry = 0;
            return;
        }
        if (g_padWaitUp && idx == g_padIdx) {
            g_padWaitUp &= raw;
            s->buttons &= (WORD)~g_padWaitUp;
        }
    }

    DWORD WINAPI sortXInputGetState(DWORD idx, PadState* s) {
        const XInputGetState_t real = *(XInputGetState_t*)kXInputGetStateImport; // read every time: others may hook it
        const DWORD r = real(idx, s);
        if (r == ERROR_SUCCESS && s && idx < 4) {
            if (g_installed) padFilter(idx, s);
            estus::padSeen(idx, s->buttons);       // what the game sees (after the sort's filter)
        }
        else estus::padSeen(idx, 0);               // no pad there (or unplugged): nothing held on it
        return r;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Keyboard (hook 4): the real keys are read with GetAsyncKeyState, so it works with or without DDTM.
    // ---------------------------------------------------------------------------------------------------------------
    const int kDikUp = 0xC8, kDikDown = 0xD0, kDikLeft = 0xCB, kDikRight = 0xCD;
    bool g_kbPrevLeft = false, g_kbPrevRight = false, g_kbPrevUp = false, g_kbPrevDown = false;
    int g_kbWaitUp = 0;                           // bits: 1 left, 2 right, 4 up, 8 down - still held when LALT was released

    bool gameHasFocus() {
        const HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        return fg && pid == GetCurrentProcessId();
    }

    void __stdcall sortOnKeyboard(LONG hr, DWORD reader) {
        if (hr < 0 || !reader) return;
        BYTE* keys = (BYTE*)(reader + 0xDC); // the game's 256-byte key buffer, just filled
        const bool focus = gameHasFocus();
        const bool alt = focus && (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0;
        const bool left = focus && (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
        const bool right = focus && (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
        const bool up = focus && (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
        const bool down = focus && (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
        const int kind = focusedKind();
        // the prompt follows the device used last (without DDTM): any key the game sees, or a mouse button
        if (focus && ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000)) g_lastKeyboard = true;
        for (int k = 0; k < 256 && !g_lastKeyboard; ++k) if (keys[k] & 0x80) g_lastKeyboard = true;

        if (!g_kbMode && !g_padMode && kind && alt) {
            g_kbMode = true;
            beginChoosing(kind);
        }
        if (g_kbMode) {
            if (!alt || kind != g_modeKind) {
                g_kbMode = false;
                g_kbWaitUp = (left ? 1 : 0) | (right ? 2 : 0) | (up ? 4 : 0) | (down ? 8 : 0);
                endChoosing(!alt && kind == g_modeKind);
            }
            else {
                if (left && !g_kbPrevLeft) stepChoice(-1);
                if (right && !g_kbPrevRight) stepChoice(+1);
                if (up && !g_kbPrevUp) toggleOrder();
                if (down && !g_kbPrevDown) toggleOrder();
            }
        }
        g_kbPrevLeft = left;
        g_kbPrevRight = right;
        g_kbPrevUp = up;
        g_kbPrevDown = down;
        if (!left) g_kbWaitUp &= ~1;
        if (!right) g_kbWaitUp &= ~2;
        if (!up) g_kbWaitUp &= ~4;
        if (!down) g_kbWaitUp &= ~8;
        if (g_kbMode || (g_kbWaitUp & 1)) keys[kDikLeft] = 0;
        if (g_kbMode || (g_kbWaitUp & 2)) keys[kDikRight] = 0;
        if (g_kbMode || (g_kbWaitUp & 4)) keys[kDikUp] = 0;
        if (g_kbMode || (g_kbWaitUp & 8)) keys[kDikDown] = 0;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Calling the game's own lookups (they take the id in esi / the block in eax, so a small bridge is needed)
    // ---------------------------------------------------------------------------------------------------------------
    __declspec(naked) void __stdcall gameLookup(DWORD fn, DWORD id, DWORD* out) {
        __asm {
            pushad
            mov esi, dword ptr[esp + 0x28]      // id
            push dword ptr[esp + 0x2C]          // out (the function removes it)
            call dword ptr[esp + 0x28]          // fn (one push further down now)
            popad
            ret 12
        }
    }
    __declspec(naked) const wchar_t* __stdcall gameName(DWORD fn, DWORD* block) {
        __asm {
            push ecx
            push edx
            push esi
            push edi
            push ebx
            push ebp
            mov eax, dword ptr[esp + 0x20]      // block
            call dword ptr[esp + 0x1C]          // fn
            pop ebp
            pop ebx
            pop edi
            pop esi
            pop edx
            pop ecx
            ret 8
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Sorting (hook 2, on the game's thread, right after the game collected and ordered the list)
    // ---------------------------------------------------------------------------------------------------------------
    struct Item {
        int pos;            // place in the game's order
        int index;          // the entry it shows
        int value[4];       // physical, magic, fire, lightning (attack or defense, as printed)
        int durability;
        int weight10;       // weight x 10, as printed (one decimal)
        int upgrade;
        int storage;        // Bottomless Box: how many are in the box (the "Storage" number of the row)
        const wchar_t* name;
    };
    const int kMaxItems = 2048;
    Item g_items[kMaxItems];
    int g_orderA[kMaxItems], g_orderB[kMaxItems];
    int g_sortChoice = 0;
    int g_sortDesc = 1;

    // Reads one entry of the list into 'it'. Returns false for anything that is not a weapon / armor piece.
    // How many of this item the box holds: the first box entry with the same kind and id, as the box screen finds it.
    int boxCount(DWORD player, DWORD kind, DWORD id) {
        const DWORD box = rd32(player + 0x420);
        if (!box) return 0;
        int n = (int)rd32(box + 0x10008) + 1;
        if (n > 0x800) n = 0x800;
        const DWORD key = kind | id;
        for (int i = 0; i < n; ++i) {
            const DWORD e = box + 8 + (DWORD)i * 32;
            if (rd32(e) == key) return (int)(rd32(e + 8) & 0xFFFF);
        }
        return 0;
    }

    bool readItem(int window, DWORD player, DWORD entry, int wantKind, bool wantStorage, Item* it) {
        DWORD kind = 0xFFFFFFFF, id = 0;
        int durability = -1;
        DWORD raw[8] = { 0 };
        bool boxOnly = false;
        if (window == W_BOX && !(entry & 1)) {
            // only in the box: an entry of the box itself
            const DWORD i = entry >> 1;
            const DWORD box = rd32(player + 0x420);
            if (!box || i > 0x7FF) return false;
            const DWORD e = box + 8 + i * 32;
            for (int k = 0; k < 8; ++k) raw[k] = rd32(e + 4 * k);
            kind = raw[0] & 0xF0000000;
            id = raw[0] & 0x0FFFFFFF;
            boxOnly = true;
        }
        else {
            const DWORD i = (window == W_BOX) ? (entry >> 1) : entry;
            const DWORD inv = player + 0x1B8;
            if ((int)i < 0 || i >= rd32(inv + 0x110)) return false;
            const DWORD block = (i < rd32(inv + 0x118)) ? rd32(inv + 0x120) : rd32(inv + 0x124);
            if (!block) return false;
            const DWORD slot = block + i * 28;
            kind = rd32(slot);
            id = rd32(slot + 4);
            durability = (int)rd32(slot + 0x14);
        }
        if (wantKind == 1 && kind != kKindWeapon) return false;
        if (wantKind == 2 && kind != kKindArmor) return false;

        DWORD look[5] = { 0xFFFFFFFF, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0 };
        gameLookup(wantKind == 1 ? kFnWeaponLookup : kFnArmorLookup, id, look);
        const DWORD row = look[1];
        const DWORD rates = look[4];
        int maxDurability = 0;
        float weight = 0.0f;
        __try {
            for (int k = 0; k < 4; ++k) {
                const DWORD base = row ? *(const WORD*)(row + (wantKind == 1 ? kWepAttack : kProDefense) + 2 * k) : 0;
                const float rate = rates ? *(const float*)(rates + 4 * k) : 1.0f;
                const float f = (float)((double)base * (double)rate);
                if (!row) it->value[k] = wantKind == 1 ? -1 : 0;
                else if (wantKind == 1) it->value[k] = (int)f;                       // printed with %d after a cut
                else it->value[k] = (int)floor((double)f + 0.5);                     // printed with %0.0f
            }
            if (row) {
                weight = *(const float*)(row + (wantKind == 1 ? kWepWeight : kProWeight));
                maxDurability = *(const WORD*)(row + (wantKind == 1 ? kWepDurability : kProDurability));
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        // an item only in the box keeps its durability in its box entry (+0xC); the maximum is the fallback
        if (boxOnly && (int)raw[3] > 0 && (int)raw[3] <= maxDurability) durability = (int)raw[3];
        it->durability = durability >= 0 ? durability : maxDurability;
        it->weight10 = (int)floor((double)weight * 10.0 + 0.5);
        it->upgrade = (int)(id % 100);
        it->storage = !wantStorage ? 0 : (boxOnly ? (int)(raw[2] & 0xFFFF) : boxCount(player, kind, id));
        it->name = row ? gameName(wantKind == 1 ? kFnWeaponName : kFnArmorName, look) : L"";
        return true;
    }

    int compareItems(const Item& a, const Item& b) {
        // the chosen value: DESC = highest first, ASC = lowest first
        const int c = g_sortChoice;
        if (c == kChoiceStorage) {
            // storage: equal counts keep the game's order
            if (a.storage != b.storage) return (g_sortDesc ? a.storage > b.storage : a.storage < b.storage) ? -1 : 1;
            return a.pos < b.pos ? -1 : (a.pos > b.pos ? 1 : 0);
        }
        int va = 0, vb = 0;
        if (c == 6) { va = a.weight10; vb = b.weight10; }
        else if (c == 5) { va = a.durability; vb = b.durability; }
        else if (c >= 1 && c <= 4) { va = a.value[c - 1]; vb = b.value[c - 1]; }
        if (va != vb) return (g_sortDesc ? va > vb : va < vb) ? -1 : 1;
        // ties: always the same rules
        if (a.weight10 != b.weight10) return a.weight10 < b.weight10 ? -1 : 1;
        if (a.durability != b.durability) return a.durability > b.durability ? -1 : 1;
        const int n = CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, a.name ? a.name : L"", -1, b.name ? b.name : L"", -1);
        if (n == CSTR_LESS_THAN) return -1;
        if (n == CSTR_GREATER_THAN) return 1;
        if (a.upgrade != b.upgrade) return a.upgrade > b.upgrade ? -1 : 1;
        return a.pos < b.pos ? -1 : (a.pos > b.pos ? 1 : 0);
    }

    // Bottom-up merge sort of item numbers, with fixed buffers (no allocation while the game runs).
    void sortItems(int n) {
        int* src = g_orderA;
        int* dst = g_orderB;
        for (int i = 0; i < n; ++i) src[i] = i;
        for (int width = 1; width < n; width *= 2) {
            for (int lo = 0; lo < n; lo += 2 * width) {
                const int mid = lo + width < n ? lo + width : n;
                const int hi = lo + 2 * width < n ? lo + 2 * width : n;
                int i = lo, j = mid, k = lo;
                while (i < mid && j < hi) dst[k++] = compareItems(g_items[src[j]], g_items[src[i]]) < 0 ? src[j++] : src[i++];
                while (i < mid) dst[k++] = src[i++];
                while (j < hi) dst[k++] = src[j++];
            }
            int* t = src; src = dst; dst = t;
        }
        if (src != g_orderA) for (int i = 0; i < n; ++i) g_orderA[i] = src[i];
    }

    void sortList(DWORD list, int window, int kind) {
        const int choice = shownChoice(kind, window);
        const int desc = g_desc[kind];
        const DWORD player = playerData();
        const DWORD entriesVec = rd32(list + kListEntries);
        const DWORD orderVec = rd32(list + kListOrder);
        if (!player || !entriesVec || !orderVec) return;
        const DWORD eBegin = rd32(entriesVec + 4), eEnd = rd32(entriesVec + 8);
        const DWORD oBegin = rd32(orderVec + 4), oEnd = rd32(orderVec + 8);
        if (!eBegin || !oBegin || eEnd < eBegin || oEnd < oBegin) return;
        const int entries = (int)((eEnd - eBegin) / 4);
        const int n = (int)((oEnd - oBegin) / 4);
        const bool toTop = (g_toTopList == list);
        int* order = (int*)oBegin;
        if (choice == 0 && !desc && n >= 2) {
            // game order, ASC: the game's own order reversed
            for (int i = 0, j = n - 1; i < j; ++i, --j) { const int t = order[i]; order[i] = order[j]; order[j] = t; }
        }
        if (choice == 0 || n < 2 || n > kMaxItems) {
            if (toTop) { *(DWORD*)(list + kListCursor) = 0; *(DWORD*)(list + kListScroll) = 0; g_toTopList = 0; }
            return;
        }
        for (int k = 0; k < n; ++k) {
            const int index = order[k];
            if (index < 0 || index >= entries) return;   // not what is expected: the list is left as it is
            Item& it = g_items[k];
            it.pos = k;
            it.index = index;
            if (!readItem(window, player, rd32(eBegin + 4 * index), kind, choice == kChoiceStorage, &it)) return;
        }
        g_sortChoice = choice;
        g_sortDesc = desc;
        sortItems(n);
        for (int k = 0; k < n; ++k) order[k] = g_items[g_orderA[k]].index;
        if (toTop) { *(DWORD*)(list + kListCursor) = 0; *(DWORD*)(list + kListScroll) = 0; g_toTopList = 0; }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Hook bodies
    // ---------------------------------------------------------------------------------------------------------------
    // the Bottomless Box's list, noted at its last rebuild (BottomlessBoxMarkers reads it while the box is on screen)
    volatile DWORD g_boxList = 0;

    void __stdcall sortOnRefresh(DWORD list, DWORD frame) {
        int window = W_NONE;
        const int kind = kindOf(list, frame, &window);
        if (window == W_NONE) return; // shops and the rest: nothing to note
        if (window == W_BOX) g_boxList = list;
        rememberList(list, window, kind);
    }

    void __stdcall sortOnCollected(DWORD list, DWORD frame) {
        int window = W_NONE;
        const int kind = kindOf(list, frame, &window);
        if (window == W_NONE) return;
        rememberList(list, window, kind);
        if (kind) sortList(list, window, kind);
        else if (g_toTopList == list) g_toTopList = 0;
    }

    void __stdcall sortOnListTick(DWORD list) {
        const int kind = cachedKind(list);
        if (!kind) return;
        g_focusList = list;
        g_focusWindow = windowOf(list);
        g_focusKind = kind;
        g_focusTime = GetTickCount();
        if (g_resortKind == kind) {
            g_resortKind = 0;
            g_toTopList = list;
            *(BYTE*)(list + kListNeedCollect) = 1;
            *(BYTE*)(list + kListNeedUpdate) = 1;
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Stubs the game jumps into
    // ---------------------------------------------------------------------------------------------------------------
    __declspec(naked) void refresh_stub() {
        __asm {
            pushad
            pushfd
            push ebp                            // the rebuild function's frame: its arguments
            push esi                            // the list
            call sortOnRefresh
            popfd
            popad
            cmp byte ptr[esi + 0x145], 1        // the instructions the jump replaced
            jne skip
            jmp dword ptr[g_refreshCont]
        skip:
            jmp dword ptr[g_refreshSkip]
        }
    }

    __declspec(naked) void collect_stub() {
        __asm {
            mov byte ptr[esi + 0x145], 0        // the instructions the jump replaced: the game's collect step
            call eax                            // (its five arguments are already on the stack; it removes them)
            pushad
            pushfd
            push ebp
            push esi
            call sortOnCollected
            popfd
            popad
            jmp dword ptr[g_collectCont]
        }
    }

    __declspec(naked) void tick_stub() {
        __asm {
            pushad
            pushfd
            push esi                            // the list
            call sortOnListTick
            popfd
            popad
            fld dword ptr[esp + 8]              // the instructions the jump replaced
            push ecx
            jmp dword ptr[g_tickCont]
        }
    }

    __declspec(naked) void keyboard_stub() {
        __asm {
            pushad
            pushfd
            push esi                            // the keyboard reader (buffer at +0xDC)
            push eax                            // result of the read
            call sortOnKeyboard
            popfd
            popad
            cmp eax, 0x8007001E                 // the instruction the jump replaced; the game's jne follows
            jmp dword ptr[g_keyboardCont]
        }
    }

    struct Site { const char* name; DWORD address; DWORD length; const BYTE* expected; const void* target; };

    bool writeJump(DWORD address, DWORD length, const void* target) {
        DWORD oldProtect;
        if (!VirtualProtect((LPVOID)address, length, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
        BYTE* p = (BYTE*)address;
        p[0] = 0xE9;
        *(DWORD*)(p + 1) = (DWORD)target - (address + 5);
        for (DWORD i = 5; i < length; ++i) p[i] = 0x90;
        VirtualProtect((LPVOID)address, length, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)address, length);
        return true;
    }
}

bool installWeaponsArmorSort() {
    using namespace wasort;
    if (g_installed) return true;
    if (!Settings::get().getWeaponsArmorSort()) return false;

    const Site sites[5] = {
        { "rebuild start",  0x00CA0985, 9, kExpRefresh,  (const void*)&refresh_stub },
        { "collect step",   0x00CA09AF, 9, kExpCollect,  (const void*)&collect_stub },
        { "list input",     0x00C8AFAA, 5, kExpTick,     (const void*)&tick_stub },
        { "keyboard read",  0x006447C8, 5, kExpKeyboard, (const void*)&keyboard_stub },
        { "XInput",         0x00ABA542, 6, kExpXInput,   (const void*)&sortXInputGetState },
    };
    for (int i = 0; i < 5; ++i) {
        if (i == 0 && g_refreshPatched) continue;   // already put there by BottomlessBoxMarkers
        if (i == 4 && g_xinputPatched) continue;    // already put there by QuickSelectEstus
        if (memcmp((const void*)sites[i].address, sites[i].expected, sites[i].length) != 0) {
            SDLOG(0, "WeaponsArmorSort: the code for '%s' at 0x%08X is not what this build should have, sorting stays off\n",
                sites[i].name, sites[i].address);
            return false;
        }
    }
    for (int i = 0; i < 5; ++i) {
        if (i == 0 && g_refreshPatched) continue;
        if (i == 4 && g_xinputPatched) continue;
        if (!writeJump(sites[i].address, sites[i].length, sites[i].target)) {
            SDLOG(0, "WeaponsArmorSort: could not write at 0x%08X, sorting stays off\n", sites[i].address);
            return false;
        }
    }
    g_installed = true;
    g_refreshPatched = true;
    g_xinputPatched = true;
    HANDLE h = CreateThread(NULL, 0, loadThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    SDLOG(0, "WeaponsArmorSort: on (Weapons and Armor lists: RS + D-pad / LALT + arrows)\n");
    return true;
}

bool weaponsArmorSortActive() {
    return wasort::g_installed;
}

bool weaponsArmorSortPrompt(SortPromptInfo* out) {
    using namespace wasort;
    if (!g_installed || !out) return false;
    const int kind = focusedKind();
    if (!kind) return false;
    const bool held = (g_padMode || g_kbMode) && g_modeKind == kind;
    out->window = g_focusWindow == W_BOX ? 2 : 1;
    out->kind = kind;
    out->choice = held ? g_temp : shownChoice(kind, g_focusWindow);
    out->desc = held ? g_tempDesc : g_desc[kind];
    out->held = g_padMode ? 1 : (g_kbMode ? 2 : 0);
    out->ownKeyboard = g_lastKeyboard ? 1 : 0;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// BottomlessBoxMarkers: in the Bottomless Box list, the rows with nothing held and some stored get a small chest picture
// (drawn by RenderstateManager.cpp). The game side only needs hook 1 above, to know the box's list object.
// ---------------------------------------------------------------------------------------------------------------------
namespace wasort {
    // the inventory slot of an inventory position (28 bytes: +0 kind, +4 id, +8 count held, +0x14 durability)
    DWORD inventorySlot(DWORD player, DWORD i) {
        const DWORD inv = player + 0x1B8;
        if ((int)i < 0 || i >= rd32(inv + 0x110)) return 0;
        const DWORD block = (i < rd32(inv + 0x118)) ? rd32(inv + 0x120) : rd32(inv + 0x124);
        return block ? block + i * 28 : 0;
    }

    // An entry of the box list: (inventory position * 2) | 1 for a held item, (box position * 2) for an item only in the
    // box. True when nothing of it is held and some of it is stored.
    bool allInBox(DWORD player, DWORD entry) {
        const DWORD box = rd32(player + 0x420);
        if (!box) return false;
        if (!(entry & 1)) {
            const DWORD i = entry >> 1;
            if (i > 0x7FF) return false;
            return (rd32(box + 8 + i * 32 + 8) & 0xFFFF) > 0;   // the count stored
        }
        // held in the inventory: a slot can stay with 0 held (used up) while the box keeps some
        const DWORD slot = inventorySlot(player, entry >> 1);
        if (!slot || rd32(slot + 8) != 0) return false;
        return boxCount(player, rd32(slot), rd32(slot + 4)) > 0;
    }
}

bool installBottomlessBoxMarkers() {
    using namespace wasort;
    if (g_markersInstalled) return true;
    if (!Settings::get().getBottomlessBoxMarkers()) return false;
    if (!g_refreshPatched) {
        const DWORD address = 0x00CA0985;
        if (memcmp((const void*)address, kExpRefresh, sizeof(kExpRefresh)) != 0) {
            SDLOG(0, "BottomlessBoxMarkers: the code at 0x%08X is not what this build should have, markers stay off\n", address);
            return false;
        }
        if (!writeJump(address, sizeof(kExpRefresh), (const void*)&refresh_stub)) {
            SDLOG(0, "BottomlessBoxMarkers: could not write at 0x%08X, markers stay off\n", address);
            return false;
        }
        g_refreshPatched = true;
    }
    g_markersInstalled = true;
    SDLOG(0, "BottomlessBoxMarkers: on\n");
    return true;
}

bool bottomlessBoxMarkersActive() {
    return wasort::g_markersInstalled;
}

int bottomlessBoxMarkerRows(unsigned char* flags, int max) {
    using namespace wasort;
    if (!g_markersInstalled || !flags || max <= 0) return 0;
    const DWORD list = g_boxList;
    if (!list || windowOf(list) != W_BOX || !(rd32(list + 0x98) & 0xFF)) return 0;   // +0x98: the list is shown
    const DWORD player = playerData();
    const DWORD entriesVec = rd32(list + kListEntries);
    const DWORD orderVec = rd32(list + kListOrder);
    if (!player || !entriesVec || !orderVec) return 0;
    const DWORD eBegin = rd32(entriesVec + 4), eEnd = rd32(entriesVec + 8);
    const DWORD oBegin = rd32(orderVec + 4), oEnd = rd32(orderVec + 8);
    if (!eBegin || !oBegin || eEnd < eBegin || oEnd < oBegin) return 0;
    const int entries = (int)((eEnd - eBegin) / 4);
    const int n = (int)((oEnd - oBegin) / 4);
    const int scroll = (int)rd32(list + kListScroll);
    if (scroll < 0 || scroll > n) return 0;
    int rows = 0;
    for (; rows < max && scroll + rows < n; ++rows) {
        const int index = (int)rd32(oBegin + 4 * (DWORD)(scroll + rows));
        flags[rows] = (index >= 0 && index < entries && allInBox(player, rd32(eBegin + 4 * (DWORD)index))) ? 1 : 0;
    }
    return rows;
}

// =====================================================================================================================
// QuickSelectEstus: during play, holding D-pad Down for 350 ms selects the Estus Flask in the quick-item slots. A tap
// still switches to the next item at once, as the game does (so a hold first switches once, then jumps to the Estus).
// The jump is the game's own: the selected item is set, the HUD plays its item change animation and the switch sound.
// Keyboard with DDTMControllerFix: its SWITCH_ITEM key is D-pad Down for the game, so holding it works the same way.
//
// How the game switches (FrpgMenuDlgFeShortcut: one dialog per D-pad slot of the HUD, updated every frame):
//   0x00C83470  the input step of a slot (argument: the dialog). For the quick-item slot (+0xF4 == 1) a D-pad Down
//               press calls "select next" on EquipItemData, sets +0xF8 = 1 and plays the menu sound (0, 2).
//               It acts only when IsShowMenu() & 0xFFFFFF7F == 0, MenuMan+0x2D0 == 0 and MenuMan+0x200 == 0.
//   +0xF8 = 1   right after, the slot plays its change animation ("F20_item_change" for the quick-item slot).
//   EquipItemData = player data + 0x310: +0xC five quick-item slots (inventory positions, -1 = empty),
//               +0x20 the selected item (an inventory position).
// The hook sits on the first instructions of 0x00C83470, so the jump is done before the game's own step of that frame.
// The D-pad comes from the XInput hook of WeaponsArmorSort (hook 5), which this feature shares.
// =====================================================================================================================
namespace estus {
    const DWORD kVtShortcut = 0x011C00F4;        // FrpgMenuDlgFeShortcut
    const DWORD kShortcutSlot = 0xF4;            // which D-pad slot: 1 = quick item
    const DWORD kShortcutChange = 0xF8;          // byte: 1 = play the slot's change animation
    const DWORD kFnIsShowMenu = 0x00C2ABE0;      // returns the "a menu is open" mask
    const DWORD kFnMenuSound = 0x00C267E0;       // stdcall (0, 2): the quick-item switch sound
    const DWORD kItemData = 0x310;               // player data + 0x310: EquipItemData
    const DWORD kItemSlots = 0x0C;               // five inventory positions, -1 = empty
    const DWORD kItemSelected = 0x20;            // the selected inventory position
    const DWORD kKindGoods = 0x40000000;
    const DWORD kEstusFirst = 200, kEstusLast = 215;   // every Estus Flask, full or empty, +0 to +7
    const DWORD kHoldMs = 350;                   // how long D-pad Down must be held
    const DWORD kGapMs = 200;                    // the slot was not updated for this long: a menu or loading was in between
    const WORD kPadDown = 0x0002;

    // the first instructions of 0x00C83470: sub esp,0x14 / push ebx / push ebp
    const BYTE kExpShortcut[5] = { 0x83,0xEC,0x14, 0x53, 0x55 };
    DWORD g_shortcutCont = 0x00C83475;

    bool g_installed = false;

    // Pad side (XInput hook): which pads hold D-pad Down, and when it went down on the first of them.
    volatile LONG g_downMask = 0;
    volatile DWORD g_pressId = 0;                // +1 at every new press
    volatile DWORD g_pressTime = 0;

    void padSeen(DWORD idx, WORD buttons) {
        if (!g_installed || idx >= 4) return;
        const LONG bit = 1L << idx;
        const LONG before = g_downMask;
        const LONG after = (buttons & kPadDown) ? (before | bit) : (before & ~bit);
        if (after == before) return;
        if (!before) {
            g_pressTime = GetTickCount();
            g_pressId = g_pressId + 1;
        }
        g_downMask = after;
    }

    // Game side (the quick-item slot's input step, every frame)
    DWORD g_seenPress = 0;
    bool g_pressDone = true;                     // this press has jumped already, or can no longer
    DWORD g_lastStep = 0;

    // The same checks the game makes before its own switch.
    bool canSwitch() {
        const DWORD mm = wasort::rd32(wasort::kMenuMan);
        if (!mm) return false;
        typedef DWORD(__cdecl* IsShowMenu_t)();
        if (((IsShowMenu_t)kFnIsShowMenu)() & 0xFFFFFF7F) return false;
        return wasort::rd32(mm + 0x2D0) == 0 && wasort::rd32(mm + 0x200) == 0;
    }

    // The inventory position of the first quick-item slot that holds an Estus Flask, or -1.
    int estusPosition(DWORD player) {
        const DWORD items = player + kItemData;
        for (DWORD k = 0; k < 5; ++k) {
            const int pos = (int)wasort::rd32(items + kItemSlots + 4 * k);
            if (pos < 0) continue;
            const DWORD slot = wasort::inventorySlot(player, (DWORD)pos);
            if (!slot) continue;
            const DWORD id = wasort::rd32(slot + 4);
            if (wasort::rd32(slot) == kKindGoods && id >= kEstusFirst && id <= kEstusLast) return pos;
        }
        return -1;
    }

    void selectEstus(DWORD dlg) {
        const DWORD player = wasort::playerData();
        if (!player) return;
        const int pos = estusPosition(player);
        if (pos < 0) return;                     // no Estus Flask in the quick-item slots
        const DWORD selected = player + kItemData + kItemSelected;
        if ((int)wasort::rd32(selected) == pos) return;   // already selected
        __try {
            *(DWORD*)selected = (DWORD)pos;
            *(BYTE*)(dlg + kShortcutChange) = 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        typedef void(__stdcall* MenuSound_t)(int, int);
        ((MenuSound_t)kFnMenuSound)(0, 2);
    }

    void __stdcall onShortcutStep(DWORD dlg) {
        if (!dlg || wasort::rd32(dlg) != kVtShortcut || wasort::rd32(dlg + kShortcutSlot) != 1) return;
        const DWORD now = GetTickCount();
        const bool gap = now - g_lastStep > kGapMs;
        g_lastStep = now;
        if (!g_downMask) return;
        const DWORD press = g_pressId;
        if (press != g_seenPress) {
            g_seenPress = press;
            g_pressDone = false;
        }
        if (g_pressDone) return;
        if (gap || !canSwitch()) {
            // a menu, a window or a loading screen during this press: it does nothing more
            g_pressDone = true;
            return;
        }
        if (now - g_pressTime < kHoldMs) return;
        g_pressDone = true;
        selectEstus(dlg);
    }

    __declspec(naked) void shortcut_stub() {
        __asm {
            pushad
            pushfd
            push dword ptr[esp + 0x28]          // the slot dialog (the function's argument)
            call onShortcutStep
            popfd
            popad
            sub esp, 0x14                       // the instructions the jump replaced
            push ebx
            push ebp
            jmp dword ptr[g_shortcutCont]
        }
    }
}

bool installQuickSelectEstus() {
    using namespace wasort;
    if (estus::g_installed) return true;
    if (!Settings::get().getQuickSelectEstus()) return false;
    const DWORD shortcutSite = 0x00C83470;
    const DWORD xinputSite = 0x00ABA542;
    if (memcmp((const void*)shortcutSite, estus::kExpShortcut, sizeof(estus::kExpShortcut)) != 0 ||
        (!g_xinputPatched && memcmp((const void*)xinputSite, kExpXInput, sizeof(kExpXInput)) != 0)) {
        SDLOG(0, "QuickSelectEstus: the game code is not what this build should have, it stays off\n");
        return false;
    }
    estus::g_installed = true;                  // the pad side works as soon as the XInput hook is live
    if (!g_xinputPatched) {
        if (!writeJump(xinputSite, sizeof(kExpXInput), (const void*)&sortXInputGetState)) {
            estus::g_installed = false;
            SDLOG(0, "QuickSelectEstus: could not write at 0x%08X, it stays off\n", xinputSite);
            return false;
        }
        g_xinputPatched = true;
    }
    if (!writeJump(shortcutSite, sizeof(estus::kExpShortcut), (const void*)&estus::shortcut_stub)) {
        estus::g_installed = false;             // the XInput hook stays, but does nothing for this feature
        SDLOG(0, "QuickSelectEstus: could not write at 0x%08X, it stays off\n", shortcutSite);
        return false;
    }
    SDLOG(0, "QuickSelectEstus: on (hold D-pad Down)\n");
    return true;
}
