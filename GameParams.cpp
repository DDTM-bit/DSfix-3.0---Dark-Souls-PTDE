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
#include <algorithm>

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
    const DWORD kWepCategory = 0xE2;                                              // byte: 13 / 14 = arrows / bolts
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
    //  6. Change Order:       call 0xC089A0                                 (Inventory: the move of "Change order")
    //  7. Equipment list open: mov eax,[esi] / mov edx,[eax+0x8C]          (after the game placed the list's view)
    const BYTE kExpRefresh[9] = { 0x80,0xBE,0x45,0x01,0x00,0x00,0x01, 0x75,0x2A };
    const BYTE kExpCollect[9] = { 0xC6,0x86,0x45,0x01,0x00,0x00,0x00, 0xFF,0xD0 };
    const BYTE kExpTick[5] = { 0xD9,0x44,0x24,0x08, 0x51 };
    const BYTE kExpKeyboard[5] = { 0x3D,0x1E,0x00,0x07,0x80 };
    const BYTE kExpXInput[6] = { 0xFF,0x25,0xE8,0xC3,0x0C,0x01 };
    const BYTE kExpChangeOrder[5] = { 0xE8,0x9B,0x0C,0xFA,0xFF };   // at 0x00C67D00: call 0x00C089A0
    const BYTE kExpEquipOpen[8] = { 0x8B,0x06, 0x8B,0x90,0x8C,0x00,0x00,0x00 };
    DWORD g_moveFn = 0x00C089A0;       // the game's move: shifts the order numbers between two rows of the list
    DWORD g_refreshCont = 0x00CA098E;  // after the compare: needs collecting
    DWORD g_refreshSkip = 0x00CA09B8;  // where the original jne went
    DWORD g_collectCont = 0x00CA09B8;
    DWORD g_tickCont = 0x00C8AFAF;
    DWORD g_keyboardCont = 0x006447CD; // the jne that uses the compare

    bool g_installed = false;
    bool g_refreshPatched = false;     // hook 1 is in place (WeaponsArmorSort or BottomlessBoxMarkers put it there)
    bool g_xinputPatched = false;      // hook 5 is in place (WeaponsArmorSort or QuickSelectEstus put it there)
    bool g_tickPatched = false;        // hook 3 is in place (WeaponsArmorSort or the item list wheel put it there)
    void(__stdcall* g_wheelTick)(DWORD list) = NULL;   // the item list wheel (end of this file), when it is on
    void(__stdcall* g_mapPad)(unsigned long idx, void* state) = NULL;   // gameMap (GameMap.h): sees every pad read first
    bool g_markersInstalled = false;   // BottomlessBoxMarkers

    // ---------------------------------------------------------------------------------------------------------------
    // The remembered choice and the file
    // ---------------------------------------------------------------------------------------------------------------
    // Choices: 0 game order, 1..4 the four attack/defense values, 5 durability, 6 weight, 7 storage (Bottomless Box only),
    // 8 most recent. Weapons and armor cycle through all of them, most recent right after game order. Every other item
    // list (kind 3) has only game order and most recent, one choice for all of them.
    const int kKindOther = 3;
    const int kChoiceStorage = 7;
    const int kChoiceRecent = 8;
    const int kCycle[] = { 0, kChoiceRecent, 1, 2, 3, 4, 5, 6, kChoiceStorage };
    const int kCycleLen = 8, kCycleLenBox = 9;    // storage only in the box
    const int kCycleOther[] = { 0, kChoiceRecent };
    volatile int g_choice[4] = { 0, 0, 0, 0 };   // [1] weapons, [2] armor, [3] every other item list
    volatile int g_desc[4] = { 1, 1, 1, 1 };     // 1 = DESC (default), 0 = ASC
    volatile LONG g_saveBusy = 0;
    volatile LONG g_saveDirty = 0;

    bool validChoice(int kind, int c) {
        if (kind == kKindOther) return c == 0 || c == kChoiceRecent;
        return c >= 0 && c <= kChoiceRecent;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // "Most recent". The game keeps no time for its items, so DSfix numbers them itself: each time an item is picked up,
    // bought or received, upgraded or infused, equipped or attuned, it gets the next number of its character (taking it
    // out of the Bottomless Box does not count). The numbers are kept in DSfixSort.ini, one block per save slot, with the
    // character's name: another character made in that slot starts a new block.
    // ---------------------------------------------------------------------------------------------------------------
    const int kRecentSlots = 10;                  // the game's save slots
    const int kRecentMax = 3072;                  // records per character; when full, the oldest quarter is dropped
    const int kNameLen = 16;                      // the character's name: 15 characters at most
    const int kPositions = 2048;                  // inventory positions
    // Weapons, armor and rings do not stack: two copies with the same id are two rows. They also get a number per
    // inventory position, so only the copy that was gained or equipped is recent. Everything else (and an item only
    // in the Bottomless Box) uses the number of its id.
    struct RecentSet {
        int used;
        wchar_t name[kNameLen];
        DWORD next;                               // the last number given
        int n;
        DWORD kid[kRecentMax];                    // kind | id, as the inventory has it
        DWORD stamp[kRecentMax];
        DWORD posKid[kPositions];                 // per inventory position: the copy that got posStamp
        DWORD posStamp[kPositions];
    };
    RecentSet g_sets[kRecentSlots];
    CRITICAL_SECTION g_recentLock;                // the save thread copies the sets while the game adds to them
    bool g_recentLockReady = false;
    volatile LONG g_recentLoaded = 0;             // the file was read: nothing is written before
    volatile LONG g_recentDirty = 0;              // numbers given since the last write
    DWORD g_dirtySince = 0;
    int g_activeSet = -1;                         // the save slot of the character in play
    const int kHashSize = 8192;                   // a power of 2, well above kRecentMax
    short g_hash[kHashSize];                      // the active set's records by kind | id (-1 = empty)
    DWORD g_stampTmp[kRecentMax];

    int hashOf(DWORD kid) { return (int)((kid * 2654435761u) >> 19) & (kHashSize - 1); }

    // From here to stampItem: only with g_recentLock held.
    void hashInsert(DWORD kid, int index) {
        int h = hashOf(kid);
        while (g_hash[h] >= 0) h = (h + 1) & (kHashSize - 1);
        g_hash[h] = (short)index;
    }
    void rebuildHash() {
        memset(g_hash, 0xFF, sizeof(g_hash));
        if (g_activeSet < 0) return;
        const RecentSet& s = g_sets[g_activeSet];
        for (int i = 0; i < s.n; ++i) hashInsert(s.kid[i], i);
    }
    int findRecord(DWORD kid) {
        if (g_activeSet < 0) return -1;
        const RecentSet& s = g_sets[g_activeSet];
        for (int h = hashOf(kid), k = 0; k < kHashSize; ++k, h = (h + 1) & (kHashSize - 1)) {
            const int i = g_hash[h];
            if (i < 0) return -1;
            if (i < s.n && s.kid[i] == kid) return i;
        }
        return -1;
    }
    // an item's number for the character in play (0 = none: not gained or equipped since "Most recent" came)
    DWORD recentStamp(DWORD kid) {
        const int i = findRecord(kid);
        return i >= 0 ? g_sets[g_activeSet].stamp[i] : 0;
    }
    void dropOldest() {
        RecentSet& s = g_sets[g_activeSet];
        if (s.n < 4) return;
        memcpy(g_stampTmp, s.stamp, sizeof(DWORD) * s.n);
        std::nth_element(g_stampTmp, g_stampTmp + s.n / 4, g_stampTmp + s.n);
        const DWORD cut = g_stampTmp[s.n / 4];
        int m = 0;
        for (int i = 0; i < s.n; ++i) if (s.stamp[i] > cut) { s.kid[m] = s.kid[i]; s.stamp[m] = s.stamp[i]; ++m; }
        s.n = m;
        rebuildHash();
    }
    // the character in play: its block (a new one when the slot has none or another name)
    void activateSet(int slot, const wchar_t* name) {
        RecentSet& s = g_sets[slot];
        if (!s.used || wcsncmp(s.name, name, kNameLen) != 0) {
            s.used = 1;
            wcsncpy_s(s.name, kNameLen, name, _TRUNCATE);
            s.next = 0;
            s.n = 0;
            memset(s.posKid, 0, sizeof(s.posKid));
            memset(s.posStamp, 0, sizeof(s.posStamp));
        }
        g_activeSet = slot;
        rebuildHash();
    }

    // weapons (not arrows / bolts: ids 2000000..2199999), armor and rings: one row per copy
    bool singleCopy(DWORD kid) {
        const DWORD kind = kid & 0xF0000000, id = kid & 0x0FFFFFFF;
        if (kind == 0x10000000 || kind == 0x20000000) return true;
        return kind == 0 && !(id >= 2000000 && id < 2200000);
    }
    // the number of a list row: pos = its inventory position (-1: only in the Bottomless Box)
    DWORD entryStamp(DWORD kid, int pos) {
        if (kid == 0xFFFFFFFF) return 0;
        if (pos >= 0 && pos < kPositions && singleCopy(kid)) {
            if (g_activeSet < 0) return 0;
            const RecentSet& s = g_sets[g_activeSet];
            return s.posKid[pos] == kid ? s.posStamp[pos] : 0;
        }
        return recentStamp(kid);
    }

    // gives the item the next number of the character in play (pos: its inventory position, -1 = none)
    void stampItem(DWORD kid, int pos) {
        if (g_activeSet < 0 || kid == 0xFFFFFFFF) return;
        EnterCriticalSection(&g_recentLock);
        RecentSet& s = g_sets[g_activeSet];
        int i = findRecord(kid);
        if (i < 0) {
            if (s.n >= kRecentMax) dropOldest();
            i = s.n++;
            s.kid[i] = kid;
            hashInsert(kid, i);
        }
        s.stamp[i] = ++s.next;
        if (pos >= 0 && pos < kPositions && singleCopy(kid)) { s.posKid[pos] = kid; s.posStamp[pos] = s.next; }
        LeaveCriticalSection(&g_recentLock);
        if (!g_recentDirty) g_dirtySince = GetTickCount();
        InterlockedExchange(&g_recentDirty, 1);
    }

    // a copy that came out of the Bottomless Box keeps the number its id has (it is not made recent)
    void keepBoxStamp(DWORD kid, int pos) {
        if (g_activeSet < 0 || pos < 0 || pos >= kPositions || !singleCopy(kid)) return;
        EnterCriticalSection(&g_recentLock);
        RecentSet& s = g_sets[g_activeSet];
        s.posKid[pos] = kid;
        s.posStamp[pos] = recentStamp(kid);
        LeaveCriticalSection(&g_recentLock);
        if (!g_recentDirty) g_dirtySince = GetTickCount();
        InterlockedExchange(&g_recentDirty, 1);
    }

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

    // Writes the choices and every character's numbers (on the save thread). Any failure is silent: they then live only
    // while the game runs.
    void writeSortFile() {
        if (!g_recentLoaded) return;
        wchar_t dir1[MAX_PATH], dir2[MAX_PATH], file[MAX_PATH], tmp[MAX_PATH];
        if (!sortFilePath(dir1, dir2, file, tmp, MAX_PATH)) return;
        HANDLE heap = GetProcessHeap();
        RecentSet* sets = (RecentSet*)HeapAlloc(heap, 0, sizeof(g_sets));
        if (!sets) return;
        EnterCriticalSection(&g_recentLock);
        memcpy(sets, g_sets, sizeof(g_sets));
        LeaveCriticalSection(&g_recentLock);
        size_t cap = 512;
        for (int k = 0; k < kRecentSlots; ++k) if (sets[k].used) cap += 160 + (size_t)(sets[k].n + kPositions) * 32;
        char* text = (char*)HeapAlloc(heap, 0, cap);
        if (!text) { HeapFree(heap, 0, sets); return; }
        int len = _snprintf_s(text, cap, _TRUNCATE,
            "Weapons=%d\r\nWeaponsOrder=%s\r\nArmor=%d\r\nArmorOrder=%s\r\nItems=%d\r\nItemsOrder=%s\r\n",
            g_choice[1], g_desc[1] ? "DESC" : "ASC", g_choice[2], g_desc[2] ? "DESC" : "ASC",
            g_choice[3], g_desc[3] ? "DESC" : "ASC");
        for (int k = 0; k < kRecentSlots && len > 0; ++k) {
            const RecentSet& s = sets[k];
            if (!s.used) continue;
            char name[64] = { 0 };
            if (!WideCharToMultiByte(CP_UTF8, 0, s.name, -1, name, sizeof(name), NULL, NULL)) name[0] = 0;
            int m = _snprintf_s(text + len, cap - len, _TRUNCATE, "Character=%d %s\r\nNext=%u\r\n", k, name, s.next);
            len = m < 0 ? -1 : len + m;
            for (int i = 0; i < s.n && len > 0; ++i) {
                m = _snprintf_s(text + len, cap - len, _TRUNCATE, "Recent=%08X %u\r\n", s.kid[i], s.stamp[i]);
                len = m < 0 ? -1 : len + m;
            }
            for (int i = 0; i < kPositions && len > 0; ++i) {
                if (!s.posStamp[i]) continue;
                m = _snprintf_s(text + len, cap - len, _TRUNCATE, "Copy=%d %08X %u\r\n", i, s.posKid[i], s.posStamp[i]);
                len = m < 0 ? -1 : len + m;
            }
        }
        HeapFree(heap, 0, sets);
        if (len > 0) {
            CreateDirectoryW(dir1, NULL);
            CreateDirectoryW(dir2, NULL);
            HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                const BOOL ok = WriteFile(h, text, (DWORD)len, &written, NULL) && written == (DWORD)len;
                CloseHandle(h);
                if (!ok || !MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) DeleteFileW(tmp);
            }
        }
        HeapFree(heap, 0, text);
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

    // the text after "key" when the line starts with it (any case), else NULL
    const char* valueOf(const char* line, const char* key) {
        const size_t n = strlen(key);
        return _strnicmp(line, key, n) == 0 ? line + n : NULL;
    }

    // Reads the choices (by kind: [1] weapons, [2] armor, [3] the other lists) and the characters' numbers:
    //   Weapons=n / WeaponsOrder=DESC|ASC / Armor=n / ArmorOrder=... / Items=n / ItemsOrder=...
    //   Character=<save slot> <name>, then Next=<last number>, one Recent=<kind | id in hex> <number> per item, and one
    //   Copy=<inventory position> <kind | id in hex> <number> per weapon, armor piece or ring copy.
    // Returns false when the file is missing or anything in it is wrong (an older file without the Items lines counts as
    // wrong, so it is written anew). With g_recentLock held.
    bool readSortFile(int* choice, int* desc) {
        static const char* const kKeys[4] = { NULL, "Weapons=", "Armor=", "Items=" };
        static const char* const kOrderKeys[4] = { NULL, "WeaponsOrder=", "ArmorOrder=", "ItemsOrder=" };
        wchar_t dir1[MAX_PATH], dir2[MAX_PATH], file[MAX_PATH], tmp[MAX_PATH];
        for (int k = 0; k < 4; ++k) { choice[k] = 0; desc[k] = 1; }
        if (!sortFilePath(dir1, dir2, file, tmp, MAX_PATH)) return false;
        HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return false;
        const DWORD size = GetFileSize(h, NULL);
        if (size == INVALID_FILE_SIZE || size > 16 * 1024 * 1024) { CloseHandle(h); return false; }
        HANDLE heap = GetProcessHeap();
        char* text = (char*)HeapAlloc(heap, 0, (SIZE_T)size + 1);
        if (!text) { CloseHandle(h); return false; }
        DWORD got = 0;
        const BOOL ok = ReadFile(h, text, size, &got, NULL);
        CloseHandle(h);
        if (!ok) { HeapFree(heap, 0, text); return false; }
        text[got] = 0;
        bool have[4] = { false, false, false, false }, haveOrder[4] = { false, false, false, false }, good = true;
        int cur = -1;                             // the character block being read
        char* line = text;
        while (*line) {
            char* end = line;
            while (*end && *end != '\n' && *end != '\r') ++end;
            const char keep = *end;
            *end = 0;
            if (*line) {
                int value = -1;
                char extra = 0;
                unsigned int a = 0, b = 0;
                const char* v = NULL;
                bool known = false;
                for (int k = 1; k <= 3 && !known; ++k) {
                    if ((v = valueOf(line, kKeys[k])) != NULL) {
                        known = true;
                        if (sscanf_s(v, "%d%c", &value, &extra, 1) == 1 && validChoice(k, value)) { choice[k] = value; have[k] = true; }
                        else good = false;
                    }
                    else if ((v = valueOf(line, kOrderKeys[k])) != NULL) {
                        known = true;
                        if ((value = parseOrder(v)) >= 0) { desc[k] = value; haveOrder[k] = true; }
                        else good = false;
                    }
                }
                if (known) {
                    // a choice line (read above)
                }
                else if ((v = valueOf(line, "Character=")) != NULL) {
                    const char* space = strchr(v, ' ');
                    if (space && sscanf_s(v, "%d", &value) == 1 && value >= 0 && value < kRecentSlots) {
                        RecentSet& s = g_sets[value];
                        s.used = 1;
                        s.next = 0;
                        s.n = 0;
                        memset(s.posKid, 0, sizeof(s.posKid));
                        memset(s.posStamp, 0, sizeof(s.posStamp));
                        memset(s.name, 0, sizeof(s.name));
                        if (!MultiByteToWideChar(CP_UTF8, 0, space + 1, -1, s.name, kNameLen)) memset(s.name, 0, sizeof(s.name));
                        s.name[kNameLen - 1] = 0;
                        cur = value;
                    }
                    else { good = false; cur = -1; }
                }
                else if ((v = valueOf(line, "Next=")) != NULL) {
                    if (cur >= 0 && sscanf_s(v, "%u%c", &a, &extra, 1) == 1) { if (a > g_sets[cur].next) g_sets[cur].next = a; }
                    else good = false;
                }
                else if ((v = valueOf(line, "Recent=")) != NULL) {
                    if (cur >= 0 && sscanf_s(v, "%x %u%c", &a, &b, &extra, 1) == 2 && b) {
                        RecentSet& s = g_sets[cur];
                        if (s.n < kRecentMax) { s.kid[s.n] = a; s.stamp[s.n] = b; ++s.n; }
                        if (b > s.next) s.next = b;
                    }
                    else good = false;
                }
                else if ((v = valueOf(line, "Copy=")) != NULL) {
                    unsigned int c = 0;
                    if (cur >= 0 && sscanf_s(v, "%u %x %u%c", &a, &b, &c, &extra, 1) == 3 && a < (unsigned)kPositions && c) {
                        RecentSet& s = g_sets[cur];
                        s.posKid[a] = b;
                        s.posStamp[a] = c;
                        if (c > s.next) s.next = c;
                    }
                    else good = false;
                }
                else good = false;
            }
            if (!keep) break;
            line = end + 1;
        }
        HeapFree(heap, 0, text);
        for (int k = 1; k <= 3; ++k) if (!have[k] || !haveOrder[k]) good = false;
        return good;
    }

    DWORD WINAPI loadThread(LPVOID) {
        int c[4], d[4];
        EnterCriticalSection(&g_recentLock);
        const bool good = readSortFile(c, d);
        LeaveCriticalSection(&g_recentLock);
        for (int k = 1; k <= 3; ++k) { g_choice[k] = c[k]; g_desc[k] = d[k]; }
        InterlockedExchange(&g_recentLoaded, 1);
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
    // kind: 0 not ours, 1 weapons (shields included, ammo not), 2 armor, 3 every other item list.
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

    // frame = ebp of the rebuild function: [frame+8] slot (Equipment), [frame+0xC] tab category (Inventory and Bottomless
    // Box: 0..3 items, materials, keys, spells, 4 weapons, 5 arrows / bolts, 6 armor, 7 rings).
    int kindOf(DWORD list, DWORD frame, int* windowOut) {
        const int window = windowOf(list);
        if (windowOut) *windowOut = window;
        if (window == W_NONE) return 0;
        const int slot = (int)rd32(frame + 8);
        const int category = (int)rd32(frame + 0xC);
        if (window == W_EQUIP) {
            if (slot >= 0 && slot <= 3) return 1;   // left / right hand 1 and 2
            if (slot >= 8 && slot <= 11) return 2;  // head, chest, hands, legs
            if ((slot >= 4 && slot <= 7) || (slot >= 13 && slot <= 19)) return kKindOther;   // ammo, rings, quick items
            return 0;
        }
        if (window == W_INVENTORY) {
            const DWORD mm = rd32(kMenuMan);
            if (!mm || rd32(mm + 0x1F0) != 0) return 0; // the inventory showing other data: not touched
        }
        if (category == 4) return 1;
        if (category == 6) return 2;
        if (category >= 0 && category <= 7) return kKindOther;
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
        const int* cycle = g_modeKind == kKindOther ? kCycleOther : kCycle;
        const int n = g_modeKind == kKindOther ? 2 : (g_modeWindow == W_BOX ? kCycleLenBox : kCycleLen);
        int at = 0;
        for (int i = 0; i < n; ++i) if (cycle[i] == g_temp) at = i;
        g_temp = cycle[(at + dir + n) % n];
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
            if (g_mapPad) g_mapPad(idx, s);        // gameMap: while the map shows it takes RS, R3, LB / RB, LT / RT
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

    bool rdBlock(DWORD a, void* out, DWORD n) {
        if (!n) return true;
        __try { memcpy(out, (const void*)a, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // the inventory slot of an inventory position (28 bytes: +0 kind, +4 id, +8 count held, +0xC order, +0x10 used,
    // +0x14 durability)
    DWORD inventorySlot(DWORD player, DWORD i) {
        const DWORD inv = player + 0x1B8;
        if ((int)i < 0 || i >= rd32(inv + 0x110)) return 0;
        const DWORD block = (i < rd32(inv + 0x118)) ? rd32(inv + 0x120) : rd32(inv + 0x124);
        return block ? block + i * 28 : 0;
    }

    // kind | id of a list entry (0xFFFFFFFF when unknown) and its inventory position (-1: only in the box). Bottomless
    // Box entries: (inventory position * 2) | 1 for a held item, (box position * 2) for an item only in the box.
    DWORD entryKid(int window, DWORD player, DWORD entry, int* pos) {
        *pos = -1;
        if (window == W_BOX && !(entry & 1)) {
            const DWORD i = entry >> 1;
            const DWORD box = rd32(player + 0x420);
            if (!box || i > 0x7FF) return 0xFFFFFFFF;
            return rd32(box + 8 + i * 32);
        }
        const DWORD p = window == W_BOX ? (entry >> 1) : entry;
        const DWORD slot = inventorySlot(player, p);
        if (!slot) return 0xFFFFFFFF;
        *pos = (int)p;
        return rd32(slot) | rd32(slot + 4);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // "Most recent": the look (wasortFrameTick, every frame at Present; it works 4 times a second). It compares the
    // inventory, the Bottomless Box, the equipment and the attunement slots with the last look:
    //   - an inventory slot that is new, holds another id (upgrade, infusion) or holds more: gained. Not when the same
    //     item left the Bottomless Box in that moment (taken out of the box), and not the Estus Flask filling or
    //     emptying (only a new flask or a higher level counts);
    //   - an equipment slot (player data + 0x1D4: 20 inventory positions) that now holds another position: equipped;
    //   - a spell in the attunement slots (player data + 0x50C: 12 x { magic id, -1 = empty; uses }) that was not there.
    // After a loading screen or with another character the first look only notes what is there. So does a look that
    // finds more than 16 gains at once (the game filling the inventory while it loads a save).
    // Equipment slots: 0..3 left / right hand 1 and 2, 4..7 arrows / bolts 1 and 2, 8..11 head / chest / hands / legs,
    // 13 / 14 rings, 15..19 quick items (12 is not an item list).
    // ---------------------------------------------------------------------------------------------------------------
    const DWORD kWorldChrMan = 0x0137D644;        // [+0x3C] = the hero (0 at the title screen)
    const DWORD kGameMan = 0x013784A0;            // +0xA70 = the save slot in play (0..9)
    const DWORD kEquipSlots = 0x1D4;              // player data: EquipGameData (+0x1B8) + 0x1C
    const DWORD kMagicSlots = 0x50C;              // player data: EquipMagicData (+0x500) + 0xC
    const int kEquipCount = 20, kMagicCount = 12;
    const DWORD kInvMax = 2048, kBoxMax = 2048;
    const int kMassGains = 16;
    const DWORD kPollMs = 250, kFlushMs = 10000;
    BYTE g_invNow[kInvMax * 28];
    BYTE g_boxNow[kBoxMax * 32];
    struct Seen { DWORD kid; DWORD count; BYTE used; };
    Seen g_seen[kInvMax];
    DWORD g_seenCount = 0;
    DWORD g_boxKid[kBoxMax], g_boxCnt[kBoxMax];
    DWORD g_boxSeenN = 0;
    DWORD g_equipSeen[kEquipCount];
    DWORD g_magicSeen[kMagicCount];
    bool g_have = false;                          // the last look is valid
    DWORD g_havePlayer = 0;
    DWORD g_lastPoll = 0;

    bool isEstus(DWORD kid) {
        const DWORD id = kid & 0x0FFFFFFF;
        return (kid & 0xF0000000) == 0x40000000 && id >= 200 && id <= 215;
    }
    void slotNow(DWORD i, DWORD count, Seen& out) {
        if (i >= count) { out.kid = 0; out.count = 0; out.used = 0; return; }
        const BYTE* s = g_invNow + i * 28;
        out.used = s[0x10] ? 1 : 0;
        out.kid = *(const DWORD*)s | *(const DWORD*)(s + 4);
        out.count = *(const DWORD*)(s + 8);
    }
    void boxNow(DWORD i, DWORD boxN, DWORD& kid, DWORD& cnt) {
        const BYTE* e = g_boxNow + i * 32;
        const bool used = i < boxN && (e[0xA] & 1);
        kid = used ? *(const DWORD*)e : 0;
        cnt = used ? *(const WORD*)(e + 8) : 0;
    }

    void recentTick() {
        if (!g_installed || !g_recentLoaded) return;
        const DWORD now = GetTickCount();
        if (now - g_lastPoll < kPollMs) return;
        g_lastPoll = now;
        const DWORD player = playerData();
        const DWORD mm = rd32(kMenuMan);
        const int loading = mm ? (int)rd32(mm + 0x7EC) : 0;
        const DWORD gm = rd32(kGameMan);
        const DWORD saveSlot = gm ? rd32(gm + 0xA70) : 0xFFFFFFFF;
        const DWORD wcm = rd32(kWorldChrMan);
        const DWORD hero = wcm ? rd32(wcm + 0x3C) : 0;
        const bool inPlay = player && hero && saveSlot < (DWORD)kRecentSlots && !(loading >= 1 && loading <= 4);
        // the numbers are written 10 s after the first new one, or at once when play stops (loading screen, title)
        if (g_recentDirty && (!inPlay || now - g_dirtySince >= kFlushMs)) {
            InterlockedExchange(&g_recentDirty, 0);
            requestSave();
        }
        if (!inPlay) { g_have = false; return; }
        wchar_t name[kNameLen];
        memset(name, 0, sizeof(name));
        if (!rdBlock(player + 0xA0, name, sizeof(wchar_t) * (kNameLen - 1)) || !name[0]) { g_have = false; return; }
        if (g_activeSet != (int)saveSlot || wcsncmp(g_sets[saveSlot].name, name, kNameLen) != 0) {
            EnterCriticalSection(&g_recentLock);
            activateSet((int)saveSlot, name);
            LeaveCriticalSection(&g_recentLock);
            g_have = false;
        }
        if (player != g_havePlayer) { g_have = false; g_havePlayer = player; }

        // inventory, box, equipment, attunement
        const DWORD inv = player + 0x1B8;
        DWORD count = rd32(inv + 0x110), split = rd32(inv + 0x118);
        const DWORD blockA = rd32(inv + 0x120), blockB = rd32(inv + 0x124);
        if (count > kInvMax) count = kInvMax;
        if (split > count) split = count;
        if (!blockA || (count > split && !blockB) || !rdBlock(blockA, g_invNow, split * 28) ||
            !rdBlock(blockB + split * 28, g_invNow + split * 28, (count - split) * 28)) { g_have = false; return; }
        const DWORD box = rd32(player + 0x420);
        DWORD boxN = box ? rd32(box + 0x10008) + 1 : 0;
        if (boxN > kBoxMax) boxN = kBoxMax;
        if (boxN && !rdBlock(box + 8, g_boxNow, boxN * 32)) boxN = 0;
        DWORD equip[kEquipCount], magic[kMagicCount * 2];
        if (!rdBlock(player + kEquipSlots, equip, sizeof(equip)) || !rdBlock(player + kMagicSlots, magic, sizeof(magic))) {
            g_have = false;
            return;
        }

        if (!g_have) {
            // the first look: only noted
            for (DWORD i = 0; i < kInvMax; ++i) slotNow(i, count, g_seen[i]);
            g_seenCount = count;
            for (DWORD i = 0; i < kBoxMax; ++i) boxNow(i, boxN, g_boxKid[i], g_boxCnt[i]);
            g_boxSeenN = boxN;
            for (int k = 0; k < kEquipCount; ++k) g_equipSeen[k] = equip[k];
            for (int k = 0; k < kMagicCount; ++k) g_magicSeen[k] = magic[2 * k];
            g_have = true;
            return;
        }

        // what left the box in this moment
        DWORD leftBox[64];
        int nLeft = 0;
        const DWORD boxUpto = boxN > g_boxSeenN ? boxN : g_boxSeenN;
        for (DWORD i = 0; i < boxUpto; ++i) {
            DWORD kid, cnt;
            boxNow(i, boxN, kid, cnt);
            if (g_boxKid[i] && (kid != g_boxKid[i] || cnt < g_boxCnt[i]) && nLeft < 64) leftBox[nLeft++] = g_boxKid[i];
            g_boxKid[i] = kid;
            g_boxCnt[i] = cnt;
        }
        g_boxSeenN = boxN;

        // gains
        DWORD found[64];
        int foundPos[64];
        int nFound = 0, nGains = 0;
        const DWORD upto = count > g_seenCount ? count : g_seenCount;
        for (DWORD i = 0; i < upto; ++i) {
            Seen cur;
            slotNow(i, count, cur);
            Seen& was = g_seen[i];
            bool gained = false;
            if (cur.used) {
                if (!was.used) gained = true;                                   // picked up, bought or received
                else if (cur.kid != was.kid) {
                    const bool estusSame = isEstus(cur.kid) && isEstus(was.kid) &&
                        ((cur.kid & 0x0FFFFFFF) - 200) / 2 == ((was.kid & 0x0FFFFFFF) - 200) / 2;
                    if (!estusSame) gained = true;                              // upgraded or infused
                }
                else if (cur.count > was.count && !isEstus(cur.kid)) gained = true;   // more of it
            }
            was = cur;
            if (!gained) continue;
            bool fromBox = false;
            for (int k = 0; k < nLeft; ++k) if (leftBox[k] == cur.kid) fromBox = true;
            if (fromBox) {
                keepBoxStamp(cur.kid, (int)i);
                continue;
            }
            ++nGains;
            if (nFound < 32) { found[nFound] = cur.kid; foundPos[nFound] = (int)i; ++nFound; }
        }
        g_seenCount = count;
        // equipped: a slot that holds another inventory position now
        for (int k = 0; k < kEquipCount; ++k) {
            const DWORD pos = equip[k];
            if (pos != g_equipSeen[k] && k != 12 && pos < count && nFound < 64) {
                Seen s;
                slotNow(pos, count, s);
                if (s.used) {
                    found[nFound] = s.kid;
                    foundPos[nFound] = (int)pos;      // equipped
                    ++nFound;
                }
            }
            g_equipSeen[k] = pos;
        }
        // attuned: a spell that was in no attunement slot
        for (int k = 0; k < kMagicCount; ++k) {
            const DWORD id = magic[2 * k];
            if (id == 0xFFFFFFFF || id == 0 || nFound >= 64) continue;
            bool before = false;
            for (int j = 0; j < kMagicCount; ++j) if (g_magicSeen[j] == id) before = true;
            if (!before) { found[nFound] = 0x40000000 | id; foundPos[nFound] = -1; ++nFound; }   // attuned
        }
        for (int k = 0; k < kMagicCount; ++k) g_magicSeen[k] = magic[2 * k];

        if (nGains > kMassGains) return;          // the game loading: not counted
        for (int k = 0; k < nFound; ++k) stampItem(found[k], foundPos[k]);
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
        DWORD recent;       // "Most recent": the item's number (0 = none)
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
        bool stackable = false;   // arrows and bolts (weapon category 13 / 14), as the game's test at 0xEA70E0
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
                if (wantKind == 1) {
                    const BYTE category = *(const BYTE*)(row + kWepCategory);
                    stackable = category == 13 || category == 14;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        // an item only in the box keeps its durability in its box entry (+0xC); the maximum is the fallback
        if (boxOnly && (int)raw[3] > 0 && (int)raw[3] <= maxDurability) durability = (int)raw[3];
        it->durability = durability >= 0 ? durability : maxDurability;
        it->weight10 = (int)floor((double)weight * 10.0 + 0.5);
        it->upgrade = (int)(id % 100);
        // Storage as the box screen prints it (0xC612E0): a box row shows its own count; a held row shows the box's count
        // only for stackable items. A held weapon or armor piece is its own row with Storage 0, even when a copy with the
        // same id is in the box.
        if (!wantStorage) it->storage = 0;
        else if (boxOnly) it->storage = (int)(raw[2] & 0xFFFF);
        else it->storage = stackable ? boxCount(player, kind, id) : 0;
        it->name = row ? gameName(wantKind == 1 ? kFnWeaponName : kFnArmorName, look) : L"";
        return true;
    }

    int compareItems(const Item& a, const Item& b) {
        // the chosen value: DESC = highest first, ASC = lowest first
        const int c = g_sortChoice;
        if (c == kChoiceRecent) {
            // most recent: the highest number first, the items without one after them; equal numbers keep the game's order
            if (a.recent != b.recent) return a.recent > b.recent ? -1 : 1;
            return a.pos < b.pos ? -1 : (a.pos > b.pos ? 1 : 0);
        }
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

    // Most recent: DESC = the item gained or equipped last first, then the items without a number in the game's order;
    // ASC = exactly the other way round.
    void sortRecent(int window, DWORD player, DWORD eBegin, int entries, int* order, int n, int desc) {
        bool ok = true;
        EnterCriticalSection(&g_recentLock);
        for (int k = 0; k < n; ++k) {
            const int index = order[k];
            if (index < 0 || index >= entries) { ok = false; break; }   // not what is expected: the list is left as it is
            Item& it = g_items[k];
            it.pos = k;
            it.index = index;
            int pos = -1;
            const DWORD kid = entryKid(window, player, rd32(eBegin + 4 * index), &pos);
            it.recent = entryStamp(kid, pos);
        }
        LeaveCriticalSection(&g_recentLock);
        if (!ok) return;
        g_sortChoice = kChoiceRecent;
        g_sortDesc = 1;
        sortItems(n);
        for (int k = 0; k < n; ++k) order[desc ? k : n - 1 - k] = g_items[g_orderA[k]].index;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // When an Equipment list opens (0xCA1C40), the game puts the equipped item at the top of the view: it looks for it
    // in its own order. A sorted list shows another order, so another item got the cursor. Hook 7 notes the list and the
    // equipped inventory position; the first sort of that list (within 1 s) puts the view on that item again, the same
    // way the game does. With nothing equipped the game opens where the list was left; that is not changed.
    // ---------------------------------------------------------------------------------------------------------------
    volatile DWORD g_openList = 0;
    DWORD g_openPos = 0, g_openTime = 0;
    const DWORD kListRows = 0xF4;                 // rows the list shows at once

    void __stdcall onEquipOpen(DWORD list) {
        g_openList = 0;
        const DWORD mm = rd32(kMenuMan);
        const DWORD player = playerData();
        const DWORD slot = mm ? rd32(mm + 0x134) : 0xFFFFFFFF;   // the equipment slot of the list
        if (!player || slot >= (DWORD)kEquipCount) return;
        const DWORD pos = rd32(player + kEquipSlots + 4 * slot);
        if (pos >= kInvMax) return;               // nothing equipped there
        g_openPos = pos;
        g_openTime = GetTickCount();
        g_openList = list;
    }

    // after a sort: the view on the equipped item, if this list just opened
    void showEquipped(DWORD list, const int* order, int n, DWORD eBegin, int entries) {
        if (g_openList != list) return;
        g_openList = 0;
        if (GetTickCount() - g_openTime > 1000) return;
        for (int k = 0; k < n; ++k) {
            const int index = order[k];
            if (index < 0 || index >= entries || rd32(eBegin + 4 * index) != g_openPos) continue;
            const int rows = (int)rd32(list + kListRows);
            int scroll = k, cursor = 0;
            if (rows <= 0 || rows > n) { scroll = 0; cursor = k; }
            else if (k + rows > n) { scroll = n - rows; cursor = k - scroll; }
            *(DWORD*)(list + kListScroll) = (DWORD)scroll;
            *(DWORD*)(list + kListCursor) = (DWORD)cursor;
            return;
        }
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
        if (choice == kChoiceRecent) {
            if (n >= 2 && n <= kMaxItems) sortRecent(window, player, eBegin, entries, order, n, desc);
            showEquipped(list, order, n, eBegin, entries);
            if (toTop) { *(DWORD*)(list + kListCursor) = 0; *(DWORD*)(list + kListScroll) = 0; g_toTopList = 0; }
            return;
        }
        if (choice == 0 && !desc && n >= 2) {
            // game order, ASC: the game's own order reversed
            for (int i = 0, j = n - 1; i < j; ++i, --j) { const int t = order[i]; order[i] = order[j]; order[j] = t; }
            showEquipped(list, order, n, eBegin, entries);
        }
        if (choice == 0 || n < 2 || n > kMaxItems) {
            if (g_openList == list) g_openList = 0;   // the game's own order: the game's own view is right
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
        showEquipped(list, order, n, eBegin, entries);
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

    // hook 3: every frame a list dialog takes input (shown, no window on top of it)
    void __stdcall listTick(DWORD list) {
        if (g_installed) sortOnListTick(list);
        if (g_wheelTick) g_wheelTick(list);
    }

    __declspec(naked) void tick_stub() {
        __asm {
            pushad
            pushfd
            push esi                            // the list
            call listTick
            popfd
            popad
            fld dword ptr[esp + 8]              // the instructions the jump replaced
            push ecx
            jmp dword ptr[g_tickCont]
        }
    }

    // hook 7: right after the Equipment list placed its view (0xCA1F5E: mov eax,[esi] / mov edx,[eax+0x8C])
    DWORD g_openCont = 0x00CA1F66;
    __declspec(naked) void equipopen_stub() {
        __asm {
            pushad
            pushfd
            push esi                            // the list
            call onEquipOpen
            popfd
            popad
            mov eax, dword ptr[esi]             // the instructions the jump replaced
            mov edx, dword ptr[eax + 0x8C]
            jmp dword ptr[g_openCont]
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

    // ---------------------------------------------------------------------------------------------------------------
    // Change Order (hook 6). The Inventory's "Change order" (0xC6A5BC -> 0xC67CE0) calls the game's move with the two
    // positions as shown in the list; the move gives the shifted rows each other's order numbers (inventory slot +0xC)
    // and the list is rebuilt in that order. That is only right while the list shows the game's order (DESC, or ASC
    // which is the same order reversed). Sorted by a value, the rows between the two positions are not neighbours in
    // the game's order: nothing seemed to move and the hidden order got mixed up. So the move is skipped then.
    // ---------------------------------------------------------------------------------------------------------------
    volatile bool g_changeOrderAllowed = true;

    void __stdcall changeOrderCheck(DWORD list) {
        bool allow = true;
        if (windowOf(list) == W_INVENTORY) {
            const int kind = cachedKind(list);
            if (kind) allow = (shownChoice(kind, W_INVENTORY) == 0);
        }
        g_changeOrderAllowed = allow;
    }

    // Replaces the call to the move: eax = inventory, ecx = target position, esi = the list; on the stack the entries,
    // the order and the source position (the move removes them: ret 12).
    __declspec(naked) void changeorder_stub() {
        __asm {
            pushad
            pushfd
            push esi                            // the list
            call changeOrderCheck
            popfd
            popad
            cmp byte ptr[g_changeOrderAllowed], 0
            je skip
            jmp dword ptr[g_moveFn]             // the game's move, with the same arguments and return address
        skip:
            ret 12                              // as the move would: its three arguments removed
        }
    }

    struct Site { const char* name; DWORD address; DWORD length; const BYTE* expected; const void* target; BYTE opcode; };

    bool writeJump(DWORD address, DWORD length, const void* target, BYTE opcode = 0xE9) {
        DWORD oldProtect;
        if (!VirtualProtect((LPVOID)address, length, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
        BYTE* p = (BYTE*)address;
        p[0] = opcode;                          // 0xE9 jmp, 0xE8 call
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
    if (!g_recentLockReady) {
        InitializeCriticalSection(&g_recentLock);
        g_recentLockReady = true;
    }

    const int kSites = 7;
    const Site sites[kSites] = {
        { "rebuild start",  0x00CA0985, 9, kExpRefresh,     (const void*)&refresh_stub,       0xE9 },
        { "collect step",   0x00CA09AF, 9, kExpCollect,     (const void*)&collect_stub,       0xE9 },
        { "list input",     0x00C8AFAA, 5, kExpTick,        (const void*)&tick_stub,          0xE9 },
        { "keyboard read",  0x006447C8, 5, kExpKeyboard,    (const void*)&keyboard_stub,      0xE9 },
        { "XInput",         0x00ABA542, 6, kExpXInput,      (const void*)&sortXInputGetState, 0xE9 },
        { "change order",   0x00C67D00, 5, kExpChangeOrder, (const void*)&changeorder_stub,   0xE8 },
        { "equipment open", 0x00CA1F5E, 8, kExpEquipOpen,   (const void*)&equipopen_stub,     0xE9 },
    };
    for (int i = 0; i < kSites; ++i) {
        if (i == 0 && g_refreshPatched) continue;   // already put there by BottomlessBoxMarkers
        if (i == 2 && g_tickPatched) continue;      // already put there by the item list wheel
        if (i == 4 && g_xinputPatched) continue;    // already put there by QuickSelectEstus
        if (memcmp((const void*)sites[i].address, sites[i].expected, sites[i].length) != 0) {
            SDLOG(0, "WeaponsArmorSort: the code for '%s' at 0x%08X is not what this build should have, sorting stays off\n",
                sites[i].name, sites[i].address);
            return false;
        }
    }
    for (int i = 0; i < kSites; ++i) {
        if (i == 0 && g_refreshPatched) continue;
        if (i == 2 && g_tickPatched) continue;
        if (i == 4 && g_xinputPatched) continue;
        if (!writeJump(sites[i].address, sites[i].length, sites[i].target, sites[i].opcode)) {
            SDLOG(0, "WeaponsArmorSort: could not write at 0x%08X, sorting stays off\n", sites[i].address);
            return false;
        }
    }
    g_installed = true;
    g_refreshPatched = true;
    g_tickPatched = true;
    g_xinputPatched = true;
    HANDLE h = CreateThread(NULL, 0, loadThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    SDLOG(0, "WeaponsArmorSort: on (Weapons and Armor values, Most recent in every item list: RS + D-pad / LALT + arrows)\n");
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

// =====================================================================================================================
// Item list wheel (with DDTMControllerFix, no setting): in the item lists the mouse wheel scrolls the list. One notch down
// selects the first item below the visible rows (the list scrolls one row and that item is the bottom row); one notch up
// the first item above them. When there is none, nothing happens. Every notch counts, also fast ones.
// Lists: Inventory, Bottomless Box, Equipment "select item", shop (buy), sell, repair, upgrade - every
// FrpgMenuDlgEquipListCommon list. Everywhere else the wheel is the game's own.
//
// How the game does it (and why it felt like a D-pad that needs insisting): the window procedure (0xF6FD80) stores the
// delta of each WM_MOUSEWHEEL in FrpgMouseManImp +0x30 ([0x137CF84]), overwriting it (two notches in one frame are one),
// and every frame (0xF70520) clears it when it equals the previous frame's value (+0x34), so the second of two quick
// notches is lost. The menus then turn it into up / down steps.
//   1. hook at 0x00F6FD9A (the store): while one of the lists above has focus, the delta is added up here instead and the
//      game gets nothing (no double moves); otherwise the game's store, as before.
//   2. hook 3 of WeaponsArmorSort (0x00C8AFAA, the list input step, reached only while the list is shown and no window is
//      on top of it): the notches are used there. List fields (FrpgMenuDlgEquipListCommon): +0xF4 visible rows, +0x100
//      cursor row on screen, +0x104 rows in the list, +0x108 first row on screen. A notch down puts the cursor on the
//      bottom row, then calls the game's own "one row down, no wrap" (0xC8AC30, esi = list, push wrap, push up; ret 8),
//      which scrolls one row and runs the list's own updates (rows, item info, scroll bar), as for the D-pad.
// Installed when DDTMControllerFix is set up (dinput8dllWrapper names it, or DDTMControllerFix.dll is in the game folder);
// it only acts once the DLL is really loaded (its export DDTM_GetSharedState is found). Otherwise nothing changes.
// =====================================================================================================================
namespace listwheel {
    const DWORD kWheelSite = 0x00F6FD9A;         // shr eax,10h / cwde / mov [ecx+30h],eax ; then xor eax,eax / ret
    const BYTE kExpWheel[7] = { 0xC1, 0xE8, 0x10, 0x98, 0x89, 0x41, 0x30 };
    DWORD g_wheelCont = 0x00F6FDA1;
    DWORD g_moveFn = 0x00C8AC30;
    const DWORD kListVtables[7] = {
        0x011C4DEC,   // FrpgMenuDlgInventory
        0x011C6644,   // FrpgMenuDlgRepository (Bottomless Box)
        0x011B995C,   // FrpgMenuDlgEquipList ("select item to equip")
        0x011C87CC,   // FrpgMenuDlgBasicShop
        0x011C9164,   // FrpgMenuDlgSellShop
        0x011C8984,   // FrpgMenuDlgFixShop (repair)
        0x011C8F5C,   // FrpgMenuDlgEquipCustomShop (upgrade)
    };
    const DWORD kFocusMs = 150;                  // no list input step for this long: no list has focus
    bool g_installed = false;
    volatile LONG g_acc = 0;                     // wheel delta not used yet (+120 per notch up)
    volatile DWORD g_focusTime = 0;
    volatile bool g_consume = false;             // set by onWheel for the stub
    bool g_sitePatched = false;                  // the wheel hook is in place (the item list wheel or gameMap put it there)
    bool(__stdcall* g_mapWheel)(int delta) = NULL;   // gameMap: true = the notch is for the map (mouse over it)
    int g_rest = 0;                              // part of a notch (high resolution wheels)

    // DDTMControllerFix.dll is loaded (looked up a few times, then the answer is kept)
    int g_ddtm = 0;                              // 0 not known yet, 1 yes, 2 no
    DWORD g_nextLook = 0;
    int g_looks = 0;
    bool ddtmLoaded() {
        if (g_ddtm) return g_ddtm == 1;
        const DWORD now = GetTickCount();
        if ((int)(now - g_nextLook) < 0) return false;
        g_nextLook = now + 2000;
        std::string names[3] = { Settings::get().getDinput8dllWrapper(), "DDTMControllerFix.dll", "dinput8.dll" };
        for (int i = 0; i < 3; ++i) {
            if (names[i].empty() || names[i].find("none") == 0) continue;
            HMODULE m = GetModuleHandleA(names[i].c_str());
            if (m && (GetProcAddress(m, "DDTM_GetSharedState") || GetProcAddress(m, "_DDTM_GetSharedState@0"))) {
                g_ddtm = 1;
                SDLOG(0, "Item list wheel: DDTMControllerFix.dll found, the wheel scrolls the item lists\n");
                return true;
            }
        }
        if (++g_looks >= 10) { g_ddtm = 2; SDLOG(0, "Item list wheel: DDTMControllerFix.dll is not loaded, the wheel stays the game's\n"); }
        return false;
    }

    bool isItemList(DWORD list) {
        const DWORD vt = wasort::rd32(list);
        for (int i = 0; i < 7; ++i) if (vt == kListVtables[i]) return true;
        return false;
    }

    // window thread, for every WM_MOUSEWHEEL: keep the notch for the list, or leave it to the game
    void __stdcall onWheel(int delta) {
        if (g_mapWheel && g_mapWheel(delta)) {
            g_consume = true;
        }
        else if (g_ddtm == 1 && GetTickCount() - g_focusTime < kFocusMs) {
            InterlockedExchangeAdd(&g_acc, delta);
            g_consume = true;
        }
        else {
            g_consume = false;
        }
    }

    __declspec(naked) void wheel_stub() {
        __asm {
            shr eax, 0x10                       // the replaced instructions: eax = the wheel delta
            cwde
            pushad
            pushfd
            push eax
            call onWheel
            popfd
            popad
            cmp byte ptr[g_consume], 0
            jne kept
            mov dword ptr[ecx + 0x30], eax      // the game's own store
        kept:
            jmp dword ptr[g_wheelCont]          // xor eax, eax / ret
        }
    }

    // the game's "one row up / down" of a list (esi = list; it removes its two arguments)
    __declspec(naked) void __stdcall callMove(DWORD list, DWORD up, DWORD wrap) {
        __asm {
            push esi
            mov esi, dword ptr[esp + 8]         // list
            push dword ptr[esp + 0x10]          // wrap
            push dword ptr[esp + 0x10]          // up (one push further down now)
            call dword ptr[g_moveFn]
            pop esi
            ret 12
        }
    }

    // One notch: the first item below (or above) the visible rows. Returns false when there is none.
    bool step(DWORD list, bool up) {
        int vis = 0, total = 0, cur = 0, top = 0;
        __try {
            vis = *(const int*)(list + 0xF4);
            total = *(const int*)(list + 0x104);
            cur = *(const int*)(list + 0x100);
            top = *(const int*)(list + 0x108);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (vis <= 0 || total <= 0 || cur < 0 || cur >= vis || top < 0) return false;
        if (up) {
            if (top <= 0) return false;                // nothing hidden above
            *(int*)(list + 0x100) = 0;                 // from the top row, one up scrolls the list
            callMove(list, 1, 0);
        }
        else {
            if (top + vis >= total) return false;      // nothing hidden below
            *(int*)(list + 0x100) = vis - 1;           // from the bottom row, one down scrolls the list
            callMove(list, 0, 0);
        }
        return true;
    }

    // hook 3, every frame a list takes input
    void __stdcall tick(DWORD list) {
        if (!ddtmLoaded() || !isItemList(list)) return;
        g_focusTime = GetTickCount();
        int a = (int)InterlockedExchange(&g_acc, 0);
        if (a == 0) return;
        a += g_rest;
        int notches = a / 120;                         // + up, - down
        g_rest = a - notches * 120;
        if (notches > 16) notches = 16;
        if (notches < -16) notches = -16;
        for (int i = 0; i < (notches > 0 ? notches : -notches); ++i)
            if (!step(list, notches > 0)) { g_rest = 0; break; }   // the end of the list: nothing more to do
    }
}

bool installListWheelScroll() {
    using namespace wasort;
    if (listwheel::g_installed) return true;
    // only when DDTMControllerFix is set up
    const std::string wrapper = Settings::get().getDinput8dllWrapper();
    std::string lower = wrapper;
    for (size_t i = 0; i < lower.size(); ++i) lower[i] = (char)tolower((unsigned char)lower[i]);
    const bool named = lower.find("ddtm") != std::string::npos;
    const bool file = GetFileAttributesA(GetDirectoryFile("DDTMControllerFix.dll")) != INVALID_FILE_ATTRIBUTES;
    if (!named && !file) return false;
    const DWORD tickSite = 0x00C8AFAA;
    if ((!listwheel::g_sitePatched && memcmp((const void*)listwheel::kWheelSite, listwheel::kExpWheel, sizeof(listwheel::kExpWheel)) != 0) ||
        (!g_tickPatched && memcmp((const void*)tickSite, kExpTick, sizeof(kExpTick)) != 0)) {
        SDLOG(0, "Item list wheel: the game code is not what this build should have, the wheel stays the game's\n");
        return false;
    }
    g_wheelTick = &listwheel::tick;              // before the hooks: hook 3 may already be live (WeaponsArmorSort)
    if (!g_tickPatched) {
        if (!writeJump(tickSite, sizeof(kExpTick), (const void*)&tick_stub)) {
            g_wheelTick = NULL;
            SDLOG(0, "Item list wheel: could not write at 0x%08X, the wheel stays the game's\n", tickSite);
            return false;
        }
        g_tickPatched = true;
    }
    if (!listwheel::g_sitePatched) {
        if (!writeJump(listwheel::kWheelSite, sizeof(listwheel::kExpWheel), (const void*)&listwheel::wheel_stub)) {
            g_wheelTick = NULL;                  // hook 3 stays, but does nothing for this feature
            SDLOG(0, "Item list wheel: could not write at 0x%08X, the wheel stays the game's\n", listwheel::kWheelSite);
            return false;
        }
        listwheel::g_sitePatched = true;
    }
    listwheel::g_installed = true;
    SDLOG(0, "Item list wheel: on (the wheel scrolls the item lists, with DDTMControllerFix)\n");
    return true;
}

// gameMap (GameMap.h): its controller and wheel input go through the shared hooks above - XInput (hook 5) and the
// window's WM_MOUSEWHEEL store (the item list wheel's hook). Both are checked first; one wrong byte and nothing is patched.
bool installGameMapInput(void(__stdcall* pad)(unsigned long idx, void* state), bool(__stdcall* wheel)(int delta)) {
    using namespace wasort;
    const DWORD xinputSite = 0x00ABA542;
    if ((!g_xinputPatched && memcmp((const void*)xinputSite, kExpXInput, sizeof(kExpXInput)) != 0) ||
        (!listwheel::g_sitePatched && memcmp((const void*)listwheel::kWheelSite, listwheel::kExpWheel, sizeof(listwheel::kExpWheel)) != 0)) {
        SDLOG(0, "gameMap: the game code is not what this build should have, the map has no controls\n");
        return false;
    }
    g_mapPad = pad;
    listwheel::g_mapWheel = wheel;
    if (!g_xinputPatched) {
        if (!writeJump(xinputSite, sizeof(kExpXInput), (const void*)&sortXInputGetState)) {
            g_mapPad = NULL;
            listwheel::g_mapWheel = NULL;
            SDLOG(0, "gameMap: could not write at 0x%08X, the map has no controls\n", xinputSite);
            return false;
        }
        g_xinputPatched = true;
    }
    if (!listwheel::g_sitePatched) {
        if (!writeJump(listwheel::kWheelSite, sizeof(listwheel::kExpWheel), (const void*)&listwheel::wheel_stub)) {
            listwheel::g_mapWheel = NULL;        // the pad works, the wheel stays the game's
            SDLOG(0, "gameMap: could not write at 0x%08X, the mouse wheel does not zoom the map\n", listwheel::kWheelSite);
            return true;
        }
        listwheel::g_sitePatched = true;
    }
    return true;
}

// "Most recent" sorting: the look at what the hero gains, equips and attunes (WeaponsArmorSort, above). Called every frame
// from RSManager::redirectPresent; it works 4 times a second, and does nothing when WeaponsArmorSort is off.
void wasortFrameTick() { wasort::recentTick(); }
