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
//   Every other item list: game order or most recent; in the Bottomless Box also storage, in the tabs whose items can be
//   stored (consumables, materials, arrows / bolts, rings). The box's item lists keep a choice of their own.
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
    void(__stdcall* g_newTick)(DWORD list) = NULL;     // NewItemMarkers (end of this file), when it is on
    void(__stdcall* g_mapPad)(unsigned long idx, void* state) = NULL;   // gameMap (GameMap.h): sees every pad read first
    void(__stdcall* g_loadPad)(DWORD idx, void* state) = NULL;          // EquipmentLoadouts (end of this file): second
    void(__stdcall* g_loadKeyboard)(LONG hr, DWORD reader) = NULL;      // EquipmentLoadouts: every keyboard read
    bool g_keyboardPatched = false;    // hook 4 is in place (WeaponsArmorSort or EquipmentLoadouts put it there)
    bool g_markersInstalled = false;   // BottomlessBoxMarkers

    // ---------------------------------------------------------------------------------------------------------------
    // The remembered choice and the file
    // ---------------------------------------------------------------------------------------------------------------
    // Choices: 0 game order, 1..4 the four attack/defense values, 5 durability, 6 weight, 7 storage (Bottomless Box only),
    // 8 most recent. Weapons and armor cycle through all of them, most recent right after game order. Every other item
    // list (kind 3) has game order and most recent, one choice for the Inventory and the Equipment lists; the Bottomless
    // Box's item lists have their own choice (slot 4), with storage too in the tabs whose items can be stored.
    const int kKindOther = 3;
    const int kSlotBoxItems = 4;                  // the choice of the Bottomless Box's other item lists
    const int kChoiceStorage = 7;
    const int kChoiceRecent = 8;
    const int kCycle[] = { 0, kChoiceRecent, 1, 2, 3, 4, 5, 6, kChoiceStorage };
    const int kCycleLen = 8, kCycleLenBox = 9;    // storage only in the box
    const int kCycleOther[] = { 0, kChoiceRecent };
    const int kCycleOtherBox[] = { 0, kChoiceRecent, kChoiceStorage };   // the box's tabs whose items can be stored
    volatile int g_choice[5] = { 0, 0, 0, 0, 0 };   // [1] weapons, [2] armor, [3] every other item list, [4] the box's
    volatile int g_desc[5] = { 1, 1, 1, 1, 1 };     // 1 = DESC (default), 0 = ASC
    volatile LONG g_saveBusy = 0;
    volatile LONG g_saveDirty = 0;

    bool validChoice(int slot, int c) {
        if (slot == kKindOther) return c == 0 || c == kChoiceRecent;
        if (slot == kSlotBoxItems) return c == 0 || c == kChoiceRecent || c == kChoiceStorage;
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
            "Weapons=%d\r\nWeaponsOrder=%s\r\nArmor=%d\r\nArmorOrder=%s\r\nItems=%d\r\nItemsOrder=%s\r\n"
            "BoxItems=%d\r\nBoxItemsOrder=%s\r\n",
            g_choice[1], g_desc[1] ? "DESC" : "ASC", g_choice[2], g_desc[2] ? "DESC" : "ASC",
            g_choice[3], g_desc[3] ? "DESC" : "ASC", g_choice[4], g_desc[4] ? "DESC" : "ASC");
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

    // Reads the choices ([1] weapons, [2] armor, [3] the other lists, [4] the Bottomless Box's other lists) and the
    // characters' numbers:
    //   Weapons=n / WeaponsOrder=DESC|ASC / Armor=n / ArmorOrder=... / Items=n / ItemsOrder=... / BoxItems=n /
    //   BoxItemsOrder=...
    //   Character=<save slot> <name>, then Next=<last number>, one Recent=<kind | id in hex> <number> per item, and one
    //   Copy=<inventory position> <kind | id in hex> <number> per weapon, armor piece or ring copy.
    // Returns false when the file is missing or anything in it is wrong (an older file without the Items lines counts as
    // wrong, so it is written anew; a file without the BoxItems lines gives the box the Items choice). With
    // g_recentLock held.
    bool readSortFile(int* choice, int* desc) {
        static const char* const kKeys[5] = { NULL, "Weapons=", "Armor=", "Items=", "BoxItems=" };
        static const char* const kOrderKeys[5] = { NULL, "WeaponsOrder=", "ArmorOrder=", "ItemsOrder=", "BoxItemsOrder=" };
        wchar_t dir1[MAX_PATH], dir2[MAX_PATH], file[MAX_PATH], tmp[MAX_PATH];
        for (int k = 0; k < 5; ++k) { choice[k] = 0; desc[k] = 1; }
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
        bool have[5] = { false, false, false, false, false }, haveOrder[5] = { false, false, false, false, false }, good = true;
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
                for (int k = 1; k <= 4 && !known; ++k) {
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
        // older files have no BoxItems lines: the box starts with the Items choice (game order or most recent)
        if (!have[4]) { choice[4] = choice[3]; good = false; }
        if (!haveOrder[4]) { desc[4] = desc[3]; good = false; }
        return good;
    }

    DWORD WINAPI loadThread(LPVOID) {
        int c[5], d[5];
        EnterCriticalSection(&g_recentLock);
        const bool good = readSortFile(c, d);
        LeaveCriticalSection(&g_recentLock);
        for (int k = 1; k <= 4; ++k) { g_choice[k] = c[k]; g_desc[k] = d[k]; }
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
    struct ListInfo { DWORD list; int window; int kind; int category; };
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

    // category: the tab (Inventory and Bottomless Box, see kindOf)
    void rememberList(DWORD list, int window, int kind, int category) {
        for (int i = 0; i < kListCache; ++i) {
            if (g_lists[i].list == list) {
                g_lists[i].window = window; g_lists[i].kind = kind; g_lists[i].category = category;
                return;
            }
        }
        g_lists[g_listNext].list = list;
        g_lists[g_listNext].window = window;
        g_lists[g_listNext].kind = kind;
        g_lists[g_listNext].category = category;
        g_listNext = (g_listNext + 1) % kListCache;
    }

    int cachedKind(DWORD list) {
        for (int i = 0; i < kListCache; ++i) if (g_lists[i].list == list) return g_lists[i].kind;
        return 0;
    }
    int cachedCategory(DWORD list) {
        for (int i = 0; i < kListCache; ++i) if (g_lists[i].list == list) return g_lists[i].category;
        return -1;
    }

    // The choice a list uses: the Bottomless Box's other item lists have their own (slot 4).
    int choiceSlot(int kind, int window) {
        return (kind == kKindOther && window == W_BOX) ? kSlotBoxItems : kind;
    }
    // Storage is offered in the Bottomless Box only: weapons and armor, and of the other tabs those whose items can be
    // stored: 0 consumables, 1 materials, 5 arrows / bolts, 7 rings (not 2 key items or 3 spells).
    bool storageOffered(int kind, int window, int category) {
        if (window != W_BOX) return false;
        if (kind != kKindOther) return true;
        return category == 0 || category == 1 || category == 5 || category == 7;
    }

    // The list that took input most recently (only ours are noted).
    volatile DWORD g_focusList = 0;
    volatile int g_focusKind = 0;
    volatile int g_focusWindow = 0;
    volatile int g_focusCategory = -1;
    volatile DWORD g_focusTime = 0;
    const DWORD kFocusMs = 120;                   // no input step for this long: the list has lost focus
    // Frames shown (counted at Present, after the prompt's look) and the frame of the list's last input step: the prompt
    // shows only in frames where its list took input, so it never stays a frame on the next window (from the Inventory
    // to the Equipment with LB / RB, the Equipment's help bar showed the sort prompt for one frame).
    volatile LONG g_sortFrame = 0, g_focusFrame = -100;

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
    volatile int g_modeCategory = -1;             // and its tab
    volatile int g_modeSlot = 0;                  // the choice it changes (g_choice / g_desc)
    int g_startChoice = 0, g_startDesc = 1;       // what was shown when the button went down
    volatile bool g_lastKeyboard = true;          // without DDTM: keyboard / mouse used last (for the prompt)

    // The value shown for a list: storage where it is not offered (outside the Bottomless Box, the box's key items and
    // spells) counts as game order.
    int shownChoice(int kind, int window, int category) {
        const int c = g_choice[choiceSlot(kind, window)];
        return (c == kChoiceStorage && !storageOffered(kind, window, category)) ? 0 : c;
    }

    void beginChoosing(int kind) {
        g_modeKind = kind;
        g_modeWindow = g_focusWindow;
        g_modeCategory = g_focusCategory;
        g_modeSlot = choiceSlot(kind, g_modeWindow);
        g_temp = shownChoice(kind, g_modeWindow, g_modeCategory);
        g_tempDesc = g_desc[g_modeSlot];
        g_startChoice = g_temp;
        g_startDesc = g_tempDesc;
    }
    void stepChoice(int dir) {
        const bool storage = storageOffered(g_modeKind, g_modeWindow, g_modeCategory);
        const int* cycle = g_modeKind == kKindOther ? (storage ? kCycleOtherBox : kCycleOther) : kCycle;
        const int n = g_modeKind == kKindOther ? (storage ? 3 : 2) : (storage ? kCycleLenBox : kCycleLen);
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
        g_choice[g_modeSlot] = g_temp;
        g_desc[g_modeSlot] = g_tempDesc;
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
            if (g_loadPad) g_loadPad(idx, s);      // EquipmentLoadouts: RS held in the Equipment window's slot grid
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

    // hook 4 calls this: the sorting (when on) and EquipmentLoadouts (when on) each see the key read
    void __stdcall keyboardRead(LONG hr, DWORD reader) {
        if (g_installed) sortOnKeyboard(hr, reader);
        if (g_loadKeyboard) g_loadKeyboard(hr, reader);
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

    // Storage of a row in the Bottomless Box's other item lists, as the box screen prints it (0xC612E0): a box row shows
    // its own count; a held row shows the box's count when the item stacks (the game's test at 0xEA70E0: goods and
    // arrows / bolts stack, armor and rings never), else 0.
    bool readStorage(DWORD player, DWORD entry, Item* it) {
        it->storage = 0;
        const DWORD i = entry >> 1;
        if (!(entry & 1)) {
            const DWORD box = rd32(player + 0x420);
            if (!box || i > 0x7FF) return false;
            it->storage = (int)(rd32(box + 8 + i * 32 + 8) & 0xFFFF);
            return true;
        }
        const DWORD inv = player + 0x1B8;
        if ((int)i < 0 || i >= rd32(inv + 0x110)) return false;
        const DWORD block = (i < rd32(inv + 0x118)) ? rd32(inv + 0x120) : rd32(inv + 0x124);
        if (!block) return false;
        const DWORD slot = block + i * 28;
        const DWORD kind = rd32(slot), id = rd32(slot + 4);
        bool stackable = kind == 0x40000000;      // goods
        if (kind == kKindWeapon) {
            DWORD look[5] = { 0xFFFFFFFF, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0 };
            gameLookup(kFnWeaponLookup, id, look);
            const DWORD row = look[1];
            if (row) {
                const DWORD category = rd32(row + kWepCategory) & 0xFF;
                stackable = category == 13 || category == 14;
            }
        }
        if (stackable) it->storage = boxCount(player, kind, id);
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

    void sortList(DWORD list, int window, int kind, int category) {
        const int choice = shownChoice(kind, window, category);
        const int desc = g_desc[choiceSlot(kind, window)];
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
            if (kind == kKindOther) {
                // the box's other item lists: only storage reaches here (game order and most recent are done above)
                if (!readStorage(player, rd32(eBegin + 4 * index), &it)) return;
            }
            else if (!readItem(window, player, rd32(eBegin + 4 * index), kind, choice == kChoiceStorage, &it)) return;
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
        rememberList(list, window, kind, (int)rd32(frame + 0xC));
    }

    void __stdcall sortOnCollected(DWORD list, DWORD frame) {
        int window = W_NONE;
        const int kind = kindOf(list, frame, &window);
        if (window == W_NONE) return;
        const int category = (int)rd32(frame + 0xC);
        rememberList(list, window, kind, category);
        if (kind) sortList(list, window, kind, category);
        else if (g_toTopList == list) g_toTopList = 0;
    }

    void __stdcall sortOnListTick(DWORD list) {
        const int kind = cachedKind(list);
        if (!kind) return;
        g_focusList = list;
        g_focusWindow = windowOf(list);
        g_focusCategory = cachedCategory(list);
        g_focusKind = kind;
        g_focusTime = GetTickCount();
        g_focusFrame = g_sortFrame;
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
        if (g_newTick) g_newTick(list);
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
            call keyboardRead
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
            if (kind) allow = (shownChoice(kind, W_INVENTORY, cachedCategory(list)) == 0);
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
        if (i == 3 && g_keyboardPatched) continue;  // already put there by EquipmentLoadouts
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
        if (i == 3 && g_keyboardPatched) continue;
        if (i == 4 && g_xinputPatched) continue;
        if (!writeJump(sites[i].address, sites[i].length, sites[i].target, sites[i].opcode)) {
            SDLOG(0, "WeaponsArmorSort: could not write at 0x%08X, sorting stays off\n", sites[i].address);
            return false;
        }
    }
    g_installed = true;
    g_refreshPatched = true;
    g_tickPatched = true;
    g_keyboardPatched = true;
    g_xinputPatched = true;
    HANDLE h = CreateThread(NULL, 0, loadThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    SDLOG(0, "WeaponsArmorSort: on (Weapons and Armor values, Most recent in every item list: RS + D-pad / LALT + arrows)\n");
    return true;
}

bool weaponsArmorSortActive() {
    return wasort::g_installed;
}

// for RenderstateManager (at Present, after the prompt's look for the next frame): one more frame shown
void weaponsArmorSortNewFrame() {
    InterlockedIncrement(&wasort::g_sortFrame);
}

bool weaponsArmorSortPrompt(SortPromptInfo* out) {
    using namespace wasort;
    if (!g_installed || !out) return false;
    const int kind = focusedKind();
    if (!kind) return false;
    if (g_focusFrame != g_sortFrame) return false;   // its list took no input this frame (LB / RB to another window)
    const bool held = (g_padMode || g_kbMode) && g_modeKind == kind;
    out->window = g_focusWindow == W_BOX ? 2 : 1;
    out->kind = kind;
    out->choice = held ? g_temp : shownChoice(kind, g_focusWindow, g_focusCategory);
    out->desc = held ? g_tempDesc : g_desc[choiceSlot(kind, g_focusWindow)];
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

// =====================================================================================================================
// EquipmentLoadouts: 5 builds per character, in the Equipment window's slot grid. A build is the whole equipment:
// weapons (left / right 1 and 2), arrows and bolts 1 and 2, head, chest, hands, legs, rings 1 and 2, quick items 1..5.
// Hold RS (keyboard: LALT) while the slot grid has focus:
//   - D-pad left / right (arrows): browse Build 1..5. The slots show the build's items: what would not change is
//     dimmed, what would change is at full brightness on a blue background (a slot the build empties: the background
//     alone). A slot whose item is not held any more keeps the current item (dimmed). Let go of RS: the real equipment.
//   - D-pad down (arrow down) held 1 s: the build is put on. D-pad up (arrow up) held 1.5 s: the equipment worn now
//     is saved as the build (it replaces what was there). LB (keyboard: Delete) held 1.5 s: the build is deleted.
//   - Every action needs its bar to fill; two of its buttons held together start nothing. Holding RS starts on the
//     build worn now (when one is), else on the build shown last.
// All timing is in milliseconds of the clock, so the frame rate (30, 60, 300 fps, an fps unlock mod) changes nothing.
// While RS / LALT is held the game sees no controller input and no arrow keys (as for WeaponsArmorSort).
// The same item at any upgrade or infusion counts (two copies: the saved one, else the highest upgrade). An empty slot
// of a build empties the slot. An item no longer held, or one the game would refuse or ask about, leaves the slot as it is.
//
// How (DARKSOULS.exe, Steam build):
//   FrpgMenuDlgEquip (vtable 0x011B9054) is the slot grid. Its method 36 (0x00CA6420) runs every frame the grid has
//   focus and gives each slot picture its item: widget = 0x00784690(grid, widget id table 0x012DEBB0[slot]), then
//   widget->vtable[0x60](0x1388, {kind | id, 2}) and (0x1394, {count, 2}). DSfix's method 36 calls the game's one and,
//   during the preview, gives the slots the build's items the same way right after it. The equipment does not change.
//   Putting an item on is the game's own "picked from the list" step 0x00CA38B0(list, slot, inventory position): it
//   takes the item out of another slot that has it, then 0x00CA37E0 sets the slot, the stats and the character model.
//   The list it reads is a small block of DSfix's own holding the slot's remembered cursor / scroll (0x012DEB60), so
//   the slot's list still opens where it did. Emptying a slot is the grid's own "remove" 0x00CA6C10(grid, slot, 0).
//   The game's checks come first: 0x00CA5E20 (the equipment can change now), for armor 0x00CA1600 (gender, covenant),
//   for rings 0x00C9F520 (taking the ring off asks something: max HP / equip load, attunement slots, the Ring of Favor
//   and Protection breaks) and no second copy of a worn ring. All of it runs inside the grid's own frame update.
// Builds are kept per character (save slot + name) in %LOCALAPPDATA%\NBGI\DarkSouls\DSfixLoadouts.ini. A missing,
// broken or read-only file is never shown in the game: the builds then live while the game runs.
// =====================================================================================================================
namespace loadouts {
    using wasort::rd32;
    using wasort::rdBlock;
    using wasort::kNameLen;
    using wasort::kRecentSlots;
    using wasort::PadState;
    using wasort::kPadUp;
    using wasort::kPadDown;
    using wasort::kPadLeft;
    using wasort::kPadRight;
    using wasort::kPadRS;
    using wasort::kDikUp;
    using wasort::kDikDown;
    using wasort::kDikLeft;
    using wasort::kDikRight;

    const DWORD kVtGrid = 0x011B9054;            // FrpgMenuDlgEquip (the slot grid)
    const int kFillMethod = 36;                  // its "fill the slot pictures" method (every frame it has focus)
    const DWORD kFillOriginal = 0x00CA6420;
    DWORD g_fnFill = 0x00CA6420;
    DWORD g_fnFindWidget = 0x00784690;           // thiscall (grid, widget id) -> widget
    DWORD g_fnEquip = 0x00CA38B0;                // stdcall (list, slot, inventory position) -> al
    DWORD g_fnRemove = 0x00CA6C10;               // stdcall (grid, slot, 0)
    DWORD g_fnRingCheck = 0x00C9F520;            // ecx = new position, eax = old position -> 0 = nothing to ask
    DWORD g_fnArmorCheck = 0x00CA1600;           // eax = kind, ecx = id, stack: list -> 0 = allowed
    DWORD g_fnCanChange = 0x00CA5E20;            // -> al: the equipment can change now
    DWORD g_fnCounted = 0x00EA70E0;              // ecx = kind, edx = id -> al: the slot shows a count
    DWORD g_fnSound = 0x00C267E0;                // stdcall (sound, 2): 0x64 = the equip sound of the lists
    struct Code { DWORD address; BYTE bytes[8]; };
    const Code kCode[9] = {
        { 0x00CA6420, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC } },
        { 0x00784690, { 0x51, 0x56, 0x8B, 0x71, 0x24, 0x85, 0xF6, 0x74 } },
        { 0x00CA38B0, { 0x83, 0xEC, 0x18, 0x53, 0x55, 0x8B, 0x6C, 0x24 } },
        { 0x00CA6C10, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x51, 0xA1 } },
        { 0x00C9F520, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC } },
        { 0x00CA1600, { 0x83, 0xEC, 0x1C, 0x53, 0x55, 0x56, 0x57, 0x8B } },
        { 0x00CA5E20, { 0x8B, 0x0D, 0x44, 0xD6, 0x37, 0x01, 0x57, 0x8B } },
        { 0x00EA70E0, { 0xC1, 0xF9, 0x1C, 0x83, 0xE1, 0x0F, 0x83, 0xEC } },
        { 0x00C267E0, { 0x6A, 0xFF, 0x68, 0xB3, 0x4E, 0x05, 0x01, 0x64 } },
    };
    const DWORD kWidgetIds = 0x012DEBB0;         // per slot: the widget id of its picture
    const DWORD kListMemory = 0x012DEB60;        // per slot: (scroll << 16) | cursor of its "select item" list
    const DWORD kMenuMan = 0x013786D0;           // +0x138: the last position picked from a list (the equip step sets it)
    const DWORD kGameMan = 0x013784A0;           // +0xA70: the save slot in play
    const DWORD kWorldChrMan = 0x0137D644;       // +0x3C: the hero
    const DWORD kPropItem = 0x1388, kPropCount = 0x1394;
    // player data: +0x1D4 the slots' inventory positions, +0x24C their item ids (no kind), +0x1B8 the inventory
    const DWORD kSlotPos = 0x1D4, kSlotId = 0x24C;
    const int kSlots = 20, kHair = 12, kBuilds = 5;
    const DWORD kEmpty = 0xFFFFFFFF;
    const DWORD kInvMax = 2048;
    const DWORD kFocusMs = 300;                  // no fill call for this long: the grid has no focus (any frame rate)
    const DWORD kApplyMs = 1000, kSaveMs = 1500, kDeleteMs = 1500, kMessageMs = 1500, kLookMs = 250, kWornMs = 500;
    const WORD kPadLB = 0x0100;
    const int kDikDelete = 0xD3;

    struct Build { int used; DWORD kid[kSlots]; int pos[kSlots]; };   // kind | id and inventory position (empty: kEmpty, -1)
    struct CharSet { int used; wchar_t name[kNameLen]; Build b[kBuilds]; };
    CharSet g_sets[kRecentSlots];
    CRITICAL_SECTION g_lock;                     // the save thread copies the sets while a build is saved
    bool g_lockReady = false;
    bool g_installed = false;
    volatile LONG g_loaded = 0;                  // the file was read (or there is none): nothing happens before
    volatile LONG g_saveBusy = 0, g_saveDirty = 0;
    volatile int g_active = -1;                  // the save slot of the character in play (-1: none)

    // ---- input side (XInput / keyboard hooks) ----
    volatile bool g_padMode = false, g_kbMode = false;
    DWORD g_padIdx = 0;
    WORD g_padPrev[4] = { 0, 0, 0, 0 };
    WORD g_padWaitUp = 0;                        // buttons still held when RS was let go: hidden until released
    bool g_kbPrevLeft = false, g_kbPrevRight = false;
    int g_kbWaitUp = 0;                          // bits: 1 left, 2 right, 4 up, 8 down, 16 Delete
    volatile int g_sel = 0;                      // the build browsed (0..4)
    volatile bool g_holdApply = false, g_holdSave = false, g_holdDelete = false;   // D-pad down / up, LB held (in the mode)
    volatile bool g_lastKeyboard = true;         // without DDTM: keyboard / mouse used last (for the prompt)
    volatile DWORD g_gridTime = 0;               // the last fill call of the grid

    // ---- game side (the fill hook) -> prompt ----
    volatile bool g_applyOn = false, g_saveOn = false, g_deleteOn = false;   // a bar is running
    volatile DWORD g_applyStart = 0, g_saveStart = 0, g_deleteStart = 0;   // since when
    volatile int g_msg = 0;                      // 1 "saved", 2 "applied", 3 "deleted" (shown for kMessageMs from g_msgTime)
    volatile int g_msgBuild = 0;
    volatile DWORD g_msgTime = 0;
    volatile unsigned g_savedMask = 0;           // bit i: build i is saved
    volatile int g_worn = -1;                    // the build worn now (-1: none)
    volatile bool g_previewOn = false;
    volatile unsigned g_dimMask = 0;             // bit s: slot s shows an item that would not change (dimmed)
    volatile unsigned g_fillMask = 0;            // bit s: slot s would change (full brightness, blue background)
    volatile unsigned g_shownMask = 0;           // bit s: slot s shows an item picture (preview or real)
    struct Shown { int on; DWORD kid; DWORD count; };
    Shown g_pv[kSlots];
    int g_lastSel = -1;
    bool g_applyDone = false, g_saveDone = false, g_deleteDone = false;
    bool g_wasMode = false;
    DWORD g_nextLook = 0, g_lastCharLook = 0, g_nextWorn = 0;
    bool g_faultLogged = false;
    BYTE g_fakeList[0x400];                      // the "list" the equip step reads (+0x100 cursor, +0x108 scroll)

    bool gridFocused() { return g_installed && GetTickCount() - g_gridTime < kFocusMs; }
    // Frames shown (counted at Present) and the frame of the grid's last fill: the prompt and the start of the mode need
    // the grid in this frame or the last two, so nothing of it is left over on the next window (LB / RB to the
    // Inventory). gridFocused (300 ms) only keeps a mode running that already started.
    volatile LONG g_frame = 0, g_fillFrame = -100;
    bool modeOn() { return g_padMode || g_kbMode; }

    // ---------------------------------------------------------------------------------------------------------------
    // The game's functions
    // ---------------------------------------------------------------------------------------------------------------
    typedef void(__fastcall* FillFn)(DWORD self, DWORD unused);
    typedef DWORD(__fastcall* FindWidgetFn)(DWORD self, DWORD unused, DWORD id);
    typedef void(__fastcall* SetPropFn)(DWORD self, DWORD unused, DWORD prop, DWORD value, DWORD type);
    typedef BYTE(__stdcall* EquipFn)(DWORD list, DWORD slot, DWORD pos);
    typedef void(__stdcall* RemoveFn)(DWORD grid, DWORD slot, DWORD flag);
    typedef BYTE(__cdecl* CanChangeFn)();
    typedef BYTE(__fastcall* CountedFn)(DWORD kind, DWORD id);
    typedef void(__stdcall* SoundFn)(DWORD sound, DWORD type);

    __declspec(naked) DWORD __stdcall ringCheck(DWORD newPos, DWORD oldPos) {
        __asm {
            mov ecx, dword ptr[esp + 4]
            mov eax, dword ptr[esp + 8]
            call dword ptr[g_fnRingCheck]
            ret 8
        }
    }
    __declspec(naked) DWORD __stdcall armorCheck(DWORD kind, DWORD id, DWORD list) {
        __asm {
            mov eax, dword ptr[esp + 4]
            mov ecx, dword ptr[esp + 8]
            push dword ptr[esp + 12]
            call dword ptr[g_fnArmorCheck]     // it removes the list argument (ret 4)
            ret 12
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The look at the equipment and the inventory (a copy, so the searches read no game memory)
    // ---------------------------------------------------------------------------------------------------------------
    struct Snap { DWORD player; DWORD count; DWORD pos[kSlots]; DWORD id[kSlots]; BYTE inv[kInvMax * 28]; };
    Snap g_snap;

    bool takeSnap(Snap& s) {
        s.player = wasort::playerData();
        if (!s.player) return false;
        const DWORD inv = s.player + 0x1B8;
        DWORD count = rd32(inv + 0x110), split = rd32(inv + 0x118);
        const DWORD blockA = rd32(inv + 0x120), blockB = rd32(inv + 0x124);
        if (count > kInvMax) count = kInvMax;
        if (split > count) split = count;
        if (!blockA || (count > split && !blockB)) return false;
        if (!rdBlock(blockA, s.inv, split * 28) || !rdBlock(blockB + split * 28, s.inv + split * 28, (count - split) * 28)) return false;
        if (!rdBlock(s.player + kSlotPos, s.pos, sizeof(s.pos)) || !rdBlock(s.player + kSlotId, s.id, sizeof(s.id))) return false;
        s.count = count;
        return true;
    }
    DWORD snapKid(const Snap& s, DWORD i) { const BYTE* e = s.inv + i * 28; return *(const DWORD*)e | *(const DWORD*)(e + 4); }
    DWORD snapCount(const Snap& s, DWORD i) { return *(const DWORD*)(s.inv + i * 28 + 8); }
    bool snapUsed(const Snap& s, DWORD i) { return i < s.count && s.inv[i * 28 + 0x10] != 0; }

    // a slot with nothing in it: no position, or the game's stand-ins (Fists, no armor)
    bool emptySlot(int slot, DWORD id, DWORD pos) {
        if ((int)pos < 0 || id == kEmpty) return true;
        if (slot <= 3) return id == 900000;
        if (slot >= 8 && slot <= 11) return id >= 900000 && id < 904000;
        return false;
    }
    // the picture value of an empty slot (what the game itself has there)
    DWORD emptyValue(int slot) {
        if (slot <= 3) return 900000;
        if (slot >= 8 && slot <= 11) return 0x10000000 | (DWORD)(900000 + (slot - 8) * 1000);
        return kEmpty;
    }
    bool isAmmo(DWORD kid) { const DWORD id = kid & 0x0FFFFFFF; return (kid & 0xF0000000) == 0 && id >= 2000000 && id < 2200000; }
    // the same item at any level: weapons (not ammo) and armor without upgrade / infusion, the Estus Flask at any level
    DWORD itemKey(DWORD kid) {
        const DWORD kind = kid & 0xF0000000, id = kid & 0x0FFFFFFF;
        if ((kind == 0 && !isAmmo(kid)) || kind == 0x10000000) return kind | (id / 1000 * 1000);
        if (kind == 0x40000000 && id >= 200 && id <= 215) return kind | 200;
        return kid;
    }
    int upgradeOf(DWORD kid) {
        const DWORD kind = kid & 0xF0000000;
        if ((kind == 0 && !isAmmo(kid)) || kind == 0x10000000) return (int)((kid & 0x0FFFFFFF) % 100);
        return 0;
    }
    // the item can go in the slot (arrows 4 / 6, bolts 5 / 7; ammo and items need some held, the Estus Flask not)
    bool fits(int slot, DWORD kid, DWORD count) {
        const DWORD kind = kid & 0xF0000000, id = kid & 0x0FFFFFFF;
        if (slot <= 3) return kind == 0 && !isAmmo(kid);
        if (slot <= 7) return isAmmo(kid) && count > 0 && ((slot == 4 || slot == 6) ? id < 2100000 : id >= 2100000);
        if (slot <= 11) return kind == 0x10000000;
        if (slot == 13 || slot == 14) return kind == 0x20000000;
        if (slot >= 15) return kind == 0x40000000 && (count > 0 || (id >= 200 && id <= 215));
        return false;
    }

    // What the build puts in each slot: an inventory position, -1 empty, -2 leave the slot (the item is not held).
    void resolve(const Build& b, const Snap& s, int* target) {
        DWORD claimed[kSlots];
        int nClaimed = 0;
        for (int slot = 0; slot < kSlots; ++slot) {
            target[slot] = -2;
            if (slot == kHair || !b.used) continue;
            const DWORD kid = b.kid[slot];
            if (kid == kEmpty) { target[slot] = -1; continue; }
            int best = -1;
            const DWORD p = (DWORD)b.pos[slot];
            bool taken = false;
            for (int k = 0; k < nClaimed; ++k) if (claimed[k] == p) taken = true;
            if (!taken && snapUsed(s, p) && snapKid(s, p) == kid && fits(slot, kid, snapCount(s, p))) best = (int)p;
            else {
                const DWORD key = itemKey(kid);
                int bestUp = -1;
                bool bestHere = false;
                for (DWORD i = 0; i < s.count; ++i) {
                    if (!snapUsed(s, i)) continue;
                    const DWORD k = snapKid(s, i);
                    if (itemKey(k) != key || !fits(slot, k, snapCount(s, i))) continue;
                    bool used = false;
                    for (int c = 0; c < nClaimed; ++c) if (claimed[c] == i) used = true;
                    if (used) continue;
                    const int up = upgradeOf(k);
                    const bool here = s.pos[slot] == i;
                    if (up > bestUp || (up == bestUp && here && !bestHere)) { best = (int)i; bestUp = up; bestHere = here; }
                }
            }
            if (best >= 0) { target[slot] = best; claimed[nClaimed++] = (DWORD)best; }
        }
        // two copies of one ring: the game refuses the second
        if (target[13] >= 0 && target[14] >= 0 && snapKid(s, (DWORD)target[13]) == snapKid(s, (DWORD)target[14])) target[14] = -2;
    }

    // applying the build would change nothing
    bool isWorn(const Build& b, const Snap& s) {
        if (!b.used) return false;
        int t[kSlots];
        resolve(b, s, t);
        for (int slot = 0; slot < kSlots; ++slot) {
            if (slot == kHair || t[slot] == -2) continue;
            if (t[slot] == -1) { if (!emptySlot(slot, s.id[slot], s.pos[slot])) return false; }
            else if (s.pos[slot] != (DWORD)t[slot]) return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The character in play and its builds
    // ---------------------------------------------------------------------------------------------------------------
    void refreshCharacter() {
        const DWORD player = wasort::playerData();
        const DWORD gm = rd32(kGameMan);
        const DWORD saveSlot = gm ? rd32(gm + 0xA70) : 0xFFFFFFFF;
        const DWORD wcm = rd32(kWorldChrMan);
        const DWORD hero = wcm ? rd32(wcm + 0x3C) : 0;
        if (!player || !hero || saveSlot >= (DWORD)kRecentSlots) { g_active = -1; return; }
        wchar_t name[kNameLen];
        memset(name, 0, sizeof(name));
        if (!rdBlock(player + 0xA0, name, sizeof(wchar_t) * (kNameLen - 1)) || !name[0]) { g_active = -1; return; }
        CharSet& c = g_sets[saveSlot];
        if (g_active != (int)saveSlot || !c.used || wcsncmp(c.name, name, kNameLen) != 0) {
            EnterCriticalSection(&g_lock);
            if (!c.used || wcsncmp(c.name, name, kNameLen) != 0) {
                // another character in this save slot (or none yet): it starts with no builds
                memset(&c, 0, sizeof(c));
                c.used = 1;
                wcsncpy_s(c.name, kNameLen, name, _TRUNCATE);
            }
            LeaveCriticalSection(&g_lock);
            g_active = (int)saveSlot;
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The file: Character=<save slot> <name>, then Build=<1..5> and 20 times <kind | id in hex>:<inventory position>
    // ---------------------------------------------------------------------------------------------------------------
    bool filePath(wchar_t* dir1, wchar_t* dir2, wchar_t* file, wchar_t* tmp, DWORD size) {
        wchar_t base[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 64) return false;
        _snwprintf_s(dir1, size, _TRUNCATE, L"%s\\NBGI", base);
        _snwprintf_s(dir2, size, _TRUNCATE, L"%s\\NBGI\\DarkSouls", base);
        _snwprintf_s(file, size, _TRUNCATE, L"%s\\NBGI\\DarkSouls\\DSfixLoadouts.ini", base);
        _snwprintf_s(tmp, size, _TRUNCATE, L"%s\\NBGI\\DarkSouls\\DSfixLoadouts.ini.tmp", base);
        return true;
    }

    void writeFile() {
        if (!g_loaded) return;
        wchar_t dir1[MAX_PATH], dir2[MAX_PATH], file[MAX_PATH], tmp[MAX_PATH];
        if (!filePath(dir1, dir2, file, tmp, MAX_PATH)) return;
        HANDLE heap = GetProcessHeap();
        CharSet* sets = (CharSet*)HeapAlloc(heap, 0, sizeof(g_sets));
        if (!sets) return;
        EnterCriticalSection(&g_lock);
        memcpy(sets, g_sets, sizeof(g_sets));
        LeaveCriticalSection(&g_lock);
        const size_t cap = (size_t)kRecentSlots * (128 + kBuilds * (16 + kSlots * 20));
        char* text = (char*)HeapAlloc(heap, 0, cap);
        if (!text) { HeapFree(heap, 0, sets); return; }
        int len = 0;
        for (int k = 0; k < kRecentSlots && len >= 0; ++k) {
            const CharSet& c = sets[k];
            bool any = false;
            for (int i = 0; i < kBuilds; ++i) if (c.b[i].used) any = true;
            if (!c.used || !any) continue;
            char name[64] = { 0 };
            if (!WideCharToMultiByte(CP_UTF8, 0, c.name, -1, name, sizeof(name), NULL, NULL)) name[0] = 0;
            int m = _snprintf_s(text + len, cap - len, _TRUNCATE, "Character=%d %s\r\n", k, name);
            len = m < 0 ? -1 : len + m;
            for (int i = 0; i < kBuilds && len >= 0; ++i) {
                if (!c.b[i].used) continue;
                m = _snprintf_s(text + len, cap - len, _TRUNCATE, "Build=%d", i + 1);
                len = m < 0 ? -1 : len + m;
                for (int s = 0; s < kSlots && len >= 0; ++s) {
                    m = _snprintf_s(text + len, cap - len, _TRUNCATE, " %08X:%d", c.b[i].kid[s], c.b[i].pos[s]);
                    len = m < 0 ? -1 : len + m;
                }
                if (len >= 0) {
                    m = _snprintf_s(text + len, cap - len, _TRUNCATE, "\r\n");
                    len = m < 0 ? -1 : len + m;
                }
            }
        }
        HeapFree(heap, 0, sets);
        bool written = false;
        if (len >= 0) {
            CreateDirectoryW(dir1, NULL);
            CreateDirectoryW(dir2, NULL);
            HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD done = 0;
                const BOOL ok = (len == 0 || WriteFile(h, text, (DWORD)len, &done, NULL)) && done == (DWORD)len;
                CloseHandle(h);
                if (ok && MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) written = true;
                else DeleteFileW(tmp);
            }
        }
        HeapFree(heap, 0, text);
    }

    DWORD WINAPI saveThread(LPVOID) {
        do {
            while (InterlockedExchange(&g_saveDirty, 0)) writeFile();
            InterlockedExchange(&g_saveBusy, 0);
        } while (g_saveDirty && InterlockedCompareExchange(&g_saveBusy, 1, 0) == 0);
        return 0;
    }
    void requestSave() {
        InterlockedExchange(&g_saveDirty, 1);
        if (InterlockedCompareExchange(&g_saveBusy, 1, 0) != 0) return;
        HANDLE h = CreateThread(NULL, 0, saveThread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        else InterlockedExchange(&g_saveBusy, 0);
    }

    // One Build line: "<n> <kid>:<pos> x 20". False when anything in it is wrong (the line is then skipped).
    bool parseBuild(const char* v, int* n, Build* out) {
        char* end = NULL;
        const long k = strtol(v, &end, 10);
        if (end == v || k < 1 || k > kBuilds) return false;
        const char* p = end;
        Build b;
        memset(&b, 0, sizeof(b));
        for (int s = 0; s < kSlots; ++s) {
            while (*p == ' ') ++p;
            const unsigned long kid = strtoul(p, &end, 16);
            if (end == p || *end != ':') return false;
            p = end + 1;
            const long pos = strtol(p, &end, 10);
            if (end == p || pos < -1 || pos >= (long)kInvMax) return false;
            p = end;
            b.kid[s] = (DWORD)kid;
            b.pos[s] = (int)pos;
            if (kid == kEmpty) b.pos[s] = -1;
        }
        while (*p == ' ') ++p;
        if (*p) return false;
        b.used = 1;
        *n = (int)k - 1;
        *out = b;
        return true;
    }

    // With g_lock held. Bad lines are skipped, the rest is kept.
    void readFile(int* builds, int* bad) {
        *builds = 0; *bad = 0;
        wchar_t dir1[MAX_PATH], dir2[MAX_PATH], file[MAX_PATH], tmp[MAX_PATH];
        if (!filePath(dir1, dir2, file, tmp, MAX_PATH)) return;
        HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return;   // no file yet
        const DWORD size = GetFileSize(h, NULL);
        if (size == INVALID_FILE_SIZE || size > 1024 * 1024) { CloseHandle(h); *bad = 1; return; }
        HANDLE heap = GetProcessHeap();
        char* text = (char*)HeapAlloc(heap, 0, (SIZE_T)size + 1);
        if (!text) { CloseHandle(h); return; }
        DWORD got = 0;
        const BOOL ok = ReadFile(h, text, size, &got, NULL);
        CloseHandle(h);
        if (!ok) { HeapFree(heap, 0, text); *bad = 1; return; }
        text[got] = 0;
        int cur = -1;
        char* line = text;
        while (*line) {
            char* end = line;
            while (*end && *end != '\n' && *end != '\r') ++end;
            const char keep = *end;
            *end = 0;
            if (*line) {
                const char* v = NULL;
                int value = -1;
                if ((v = wasort::valueOf(line, "Character=")) != NULL) {
                    const char* space = strchr(v, ' ');
                    if (space && sscanf_s(v, "%d", &value) == 1 && value >= 0 && value < kRecentSlots) {
                        CharSet& c = g_sets[value];
                        memset(&c, 0, sizeof(c));
                        c.used = 1;
                        if (!MultiByteToWideChar(CP_UTF8, 0, space + 1, -1, c.name, kNameLen)) memset(c.name, 0, sizeof(c.name));
                        c.name[kNameLen - 1] = 0;
                        cur = value;
                    }
                    else { ++*bad; cur = -1; }
                }
                else if ((v = wasort::valueOf(line, "Build=")) != NULL) {
                    int n = 0;
                    Build b;
                    if (cur >= 0 && parseBuild(v, &n, &b)) { g_sets[cur].b[n] = b; ++*builds; }
                    else ++*bad;
                }
                else ++*bad;
            }
            if (!keep) break;
            line = end + 1;
        }
        HeapFree(heap, 0, text);
    }

    DWORD WINAPI loadThread(LPVOID) {
        int builds = 0, bad = 0;
        EnterCriticalSection(&g_lock);
        readFile(&builds, &bad);
        LeaveCriticalSection(&g_lock);
        InterlockedExchange(&g_loaded, 1);
        return 0;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Saving and putting on a build (game side, inside the grid's frame update)
    // ---------------------------------------------------------------------------------------------------------------
    void saveBuild(int sel) {
        if (g_active < 0 || !takeSnap(g_snap)) return;
        Build nb;
        memset(&nb, 0, sizeof(nb));
        nb.used = 1;
        for (int slot = 0; slot < kSlots; ++slot) {
            const DWORD pos = g_snap.pos[slot], id = g_snap.id[slot];
            nb.kid[slot] = kEmpty;
            nb.pos[slot] = -1;
            if (slot == kHair || emptySlot(slot, id, pos) || !snapUsed(g_snap, pos)) continue;
            nb.kid[slot] = snapKid(g_snap, pos);
            nb.pos[slot] = (int)pos;
        }
        EnterCriticalSection(&g_lock);
        g_sets[g_active].b[sel] = nb;
        LeaveCriticalSection(&g_lock);
        requestSave();
    }

    void deleteBuild(int sel) {
        if (g_active < 0) return;
        EnterCriticalSection(&g_lock);
        memset(&g_sets[g_active].b[sel], 0, sizeof(Build));
        LeaveCriticalSection(&g_lock);
        requestSave();
    }

    int applyBuild(DWORD grid, const Build& b) {
        if (!((CanChangeFn)g_fnCanChange)()) return 0;
        if (!takeSnap(g_snap)) return 0;
        int t[kSlots];
        resolve(b, g_snap, t);
        const DWORD player = g_snap.player;
        const DWORD mm = rd32(kMenuMan);
        const DWORD keep138 = mm ? rd32(mm + 0x138) : 0;
        int changed = 0;
        for (int slot = 0; slot < kSlots; ++slot) {
            if (slot == kHair) continue;
            const DWORD curPos = rd32(player + kSlotPos + slot * 4), curId = rd32(player + kSlotId + slot * 4);
            if (t[slot] == -2) continue;                      // missing: the slot stays as it is
            if (t[slot] == -1) {
                // emptied (not a ring the game would ask about)
                if (!emptySlot(slot, curId, curPos) && !((slot == 13 || slot == 14) && ringCheck(kEmpty, curPos) != 0)) {
                    ((RemoveFn)g_fnRemove)(grid, (DWORD)slot, 0);
                    ++changed;
                }
            }
            else if ((DWORD)t[slot] != curPos) {
                const DWORD pos = (DWORD)t[slot];
                const DWORD e = wasort::inventorySlot(player, pos);
                const DWORD kind = e ? rd32(e) : 0, id = e ? rd32(e + 4) : 0;
                if (!e || (kind | id) != snapKid(g_snap, pos)) continue;                              // the bag changed
                if (kind == 0x10000000 && armorCheck(kind, id, (DWORD)g_fakeList) != 0) continue;       // refused
                if ((slot == 13 || slot == 14) && rd32(player + kSlotId + (slot == 13 ? 14 : 13) * 4) == id &&
                    rd32(player + kSlotPos + (slot == 13 ? 14 : 13) * 4) != pos) continue;             // ring twice
                if ((slot == 13 || slot == 14) && ringCheck(pos, curPos) != 0) continue;                // ring asks
                const DWORD mem = rd32(kListMemory + slot * 4);
                *(DWORD*)(g_fakeList + 0x100) = mem & 0xFFFF;
                *(DWORD*)(g_fakeList + 0x108) = mem >> 16;
                ((EquipFn)g_fnEquip)((DWORD)g_fakeList, (DWORD)slot, pos);
                ++changed;
            }
        }
        if (mm) *(DWORD*)(mm + 0x138) = keep138;
        if (changed) ((SoundFn)g_fnSound)(0x64, 2);
        return changed;
    }

    // the worn build and the preview of the browsed one (4 times a second, at once after a change)
    // the build worn now, from g_snap (the one shown when it is worn too, else the first; -1 none)
    int wornBuild(int sel) {
        const CharSet& c = g_sets[g_active];
        int worn = -1;
        for (int i = 0; i < kBuilds; ++i) {
            if (!isWorn(c.b[i], g_snap)) continue;
            if (worn < 0 || i == sel) worn = i;
        }
        return worn;
    }

    void look(int sel) {
        if (!takeSnap(g_snap)) { g_previewOn = false; return; }
        const CharSet& c = g_sets[g_active];
        g_worn = wornBuild(sel);
        int t[kSlots];
        resolve(c.b[sel], g_snap, t);
        // Every picture is dimmed, except the slots the build would change: those are at full brightness with the blue
        // background (a slot the build would empty: the background alone). A slot whose item is gone does not change.
        unsigned dim = 0, fill = 0, shown = 0;
        for (int slot = 0; slot < kSlots; ++slot) {
            Shown& v = g_pv[slot];
            v.on = 0;
            const bool curEmpty = emptySlot(slot, g_snap.id[slot], g_snap.pos[slot]);
            if (slot == kHair) continue;
            const unsigned bit = 1u << slot;
            if (t[slot] >= 0) {
                const DWORD p = (DWORD)t[slot], kid = snapKid(g_snap, p);
                v.on = 1;
                v.kid = kid;
                v.count = ((CountedFn)g_fnCounted)(kid & 0xF0000000, kid & 0x0FFFFFFF) ? snapCount(g_snap, p) : 0;
                shown |= bit;
                if (p != g_snap.pos[slot]) fill |= bit;
            }
            else if (t[slot] == -1) {
                if (!curEmpty) { v.on = 1; v.kid = emptyValue(slot); v.count = 0; fill |= bit; }
            }
            else if (!curEmpty) shown |= bit;
        }
        // every other slot is dimmed: also an empty one, where the game still draws its stand-in (Fists, no armour)
        dim = 0xFFFFFu & ~fill & ~(1u << kHair);
        g_dimMask = dim;
        g_fillMask = fill;
        g_shownMask = shown;
    }

    // One hold action. ok: its button is held and it may run. The bar starts in this frame; true comes only in a later
    // frame, once it has run for 'ms' (the time is compared signed: never "full" before it started). Let go, or another
    // button held with it: the bar is gone and nothing happens. After it was done, the button must be let go first.
    bool bar(bool ok, bool held, volatile bool& on, volatile DWORD& start, bool& done, DWORD ms, DWORD now) {
        if (ok) {
            if (done) return false;
            if (!on) {
                start = now;
                on = true;
                return false;
            }
            if ((int)(now - start) < (int)ms) return false;
            on = false;
            done = true;
            return true;
        }
        on = false;
        if (!held) done = false;
        return false;
    }

    void tick(DWORD grid) {
        const DWORD now = GetTickCount();
        const bool had = now - g_gridTime < kFocusMs;
        g_gridTime = now;
        g_fillFrame = g_frame;
        if (!had || now - g_lastCharLook >= 500) { g_lastCharLook = now; refreshCharacter(); }
        if (!g_loaded || g_active < 0) { g_previewOn = false; g_dimMask = 0; g_fillMask = 0; return; }
        const CharSet& c = g_sets[g_active];
        unsigned m = 0;
        for (int i = 0; i < kBuilds; ++i) if (c.b[i].used) m |= 1u << i;
        g_savedMask = m;
        if (!modeOn()) {
            g_applyOn = false; g_saveOn = false; g_deleteOn = false;
            g_applyDone = false; g_saveDone = false; g_deleteDone = false;
            g_previewOn = false; g_dimMask = 0; g_fillMask = 0;
            g_lastSel = -1;
            g_wasMode = false;
            // the worn build, so holding RS starts on it at once
            if ((int)(now - g_nextWorn) >= 0 || !g_nextWorn) {
                g_nextWorn = now + kWornMs;
                if (takeSnap(g_snap)) g_worn = wornBuild(g_sel);
            }
            return;
        }
        if (!g_wasMode) {
            // the first frame of the mode: the worn build exactly as it is now (the input side used the last look)
            g_wasMode = true;
            if (takeSnap(g_snap)) {
                const int worn = wornBuild(g_sel);
                g_worn = worn;
                if (worn >= 0 && worn != g_sel) g_sel = worn;
            }
        }
        const int sel = g_sel;
        if (sel != g_lastSel) {
            g_lastSel = sel;
            // another build: a bar still held starts again from empty
            g_applyOn = false; g_saveOn = false; g_deleteOn = false;
            g_applyDone = false; g_saveDone = false; g_deleteDone = false;
            g_nextLook = 0;
        }
        const bool used = c.b[sel].used != 0;
        const bool hs = g_holdSave, ha = g_holdApply, hd = g_holdDelete;
        // one action at a time, and only with its own button held alone (two held together start nothing)
        const bool okSave = hs && !ha && !hd && !g_applyOn && !g_deleteOn;
        const bool okApply = ha && !hs && !hd && used && !g_saveOn && !g_deleteOn;
        const bool okDelete = hd && !hs && !ha && used && !g_saveOn && !g_applyOn;
        if (bar(okSave, hs, g_saveOn, g_saveStart, g_saveDone, kSaveMs, now)) {
            saveBuild(sel);
            g_msgTime = now; g_msgBuild = sel; g_msg = 1;
            g_nextLook = 0;
        }
        if (bar(okApply, ha, g_applyOn, g_applyStart, g_applyDone, kApplyMs, now)) {
            applyBuild(grid, c.b[sel]);
            g_msgTime = now; g_msgBuild = sel; g_msg = 2;
            g_nextLook = 0;
        }
        if (bar(okDelete, hd, g_deleteOn, g_deleteStart, g_deleteDone, kDeleteMs, now)) {
            deleteBuild(sel);
            g_msgTime = now; g_msgBuild = sel; g_msg = 3;
            g_nextLook = 0;
        }
        if (g_msg && ((int)(now - g_msgTime) >= (int)kMessageMs || g_msgBuild != sel)) g_msg = 0;
        if ((int)(now - g_nextLook) >= 0 || !g_nextLook) { g_nextLook = now + kLookMs; look(sel); }
        g_previewOn = !g_holdSave && c.b[sel].used;
        if (!g_previewOn) { g_dimMask = 0; g_fillMask = 0; }
    }

    // right after the game filled the grid: the slots get the build's items
    void preview(DWORD grid) {
        if (!g_previewOn || !modeOn()) return;
        for (int slot = 0; slot < kSlots; ++slot) {
            if (!g_pv[slot].on) continue;
            const DWORD w = ((FindWidgetFn)g_fnFindWidget)(grid, 0, rd32(kWidgetIds + slot * 4));
            if (!w) continue;
            const DWORD vt = rd32(w);
            const DWORD fn = vt ? rd32(vt + 0x60) : 0;
            if (!fn) continue;
            ((SetPropFn)fn)(w, 0, kPropItem, g_pv[slot].kid, 2);
            ((SetPropFn)fn)(w, 0, kPropCount, g_pv[slot].count, 2);
        }
    }

    void fault(const char* where) {
        g_previewOn = false;
        g_dimMask = 0;
        g_fillMask = 0;
        if (!g_faultLogged) { g_faultLogged = true; SDLOG(0, "EquipmentLoadouts: an error in %s was caught, the step was skipped\n", where); }
    }
    void tickSafe(DWORD grid) {
        __try { tick(grid); }
        __except (EXCEPTION_EXECUTE_HANDLER) { fault("the frame step"); }
    }
    void previewSafe(DWORD grid) {
        __try { preview(grid); }
        __except (EXCEPTION_EXECUTE_HANDLER) { fault("the preview"); }
    }

    // the grid's method 36 (thiscall, no arguments): DSfix's step, the game's fill, then the preview
    void __fastcall onFill(DWORD grid, DWORD unused) {
        tickSafe(grid);
        ((FillFn)g_fnFill)(grid, unused);
        previewSafe(grid);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Input
    // ---------------------------------------------------------------------------------------------------------------
    bool ready() { return gridFocused() && g_loaded && g_active >= 0; }
    // the slot grid is on screen now: filled in this frame or the last two, and no item list has focus (the sorting's
    // list focus, when WeaponsArmorSort is on)
    bool onScreen() { return ready() && g_frame - g_fillFrame <= 2 && !wasort::focusedKind(); }
    // for the prompt: the grid was updated in this very frame (the game updates it before drawing the frame), so the
    // hint is never drawn on the next window's first frame (LB / RB to the Inventory)
    bool updatedThisFrame() { return onScreen() && g_fillFrame == g_frame; }
    void beginMode() {
        g_holdApply = false; g_holdSave = false; g_holdDelete = false;
        g_msg = 0;
        if (g_worn >= 0) g_sel = g_worn;          // start on the build worn now
    }
    void endMode() {
        g_holdApply = false; g_holdSave = false; g_holdDelete = false;
    }
    void stepBuild(int dir) {
        g_sel = (g_sel + dir + kBuilds) % kBuilds;
        g_msg = 0;
    }

    void __stdcall onPad(DWORD idx, void* state) {
        PadState* s = (PadState*)state;
        if (!s || idx >= 4) return;
        const WORD raw = s->buttons;
        const WORD prev = g_padPrev[idx];
        g_padPrev[idx] = raw;
        if ((raw & (WORD)~prev) || s->lt > 30 || s->rt > 30 || s->lx > 8000 || s->lx < -8000 || s->ly > 8000 ||
            s->ly < -8000 || s->rx > 8000 || s->rx < -8000 || s->ry > 8000 || s->ry < -8000) g_lastKeyboard = false;
        const bool grid = ready();
        if (!g_padMode && !g_kbMode && onScreen() && (raw & kPadRS)) {
            g_padMode = true;
            g_padIdx = idx;
            beginMode();
        }
        if (g_padMode && idx == g_padIdx) {
            if (!(raw & kPadRS) || !grid) {
                g_padMode = false;
                g_padWaitUp = raw;
                endMode();
            }
            else {
                if ((raw & kPadLeft) && !(prev & kPadLeft)) stepBuild(-1);
                if ((raw & kPadRight) && !(prev & kPadRight)) stepBuild(+1);
                g_holdApply = (raw & kPadDown) != 0;
                g_holdSave = (raw & kPadUp) != 0;
                g_holdDelete = (raw & kPadLB) != 0;
            }
        }
        if (g_padMode) {
            // everything is hidden from the game while RS is held (every pad)
            s->buttons = 0; s->lt = 0; s->rt = 0; s->lx = 0; s->ly = 0; s->rx = 0; s->ry = 0;
            return;
        }
        // LALT held: the D-pad too (DDTMControllerFix can give the arrow keys to the game as a D-pad)
        if (g_kbMode) s->buttons &= (WORD)~(kPadUp | kPadDown | kPadLeft | kPadRight);
        if (g_padWaitUp && idx == g_padIdx) {
            g_padWaitUp &= raw;
            s->buttons &= (WORD)~g_padWaitUp;
        }
    }

    void __stdcall onKeyboard(LONG hr, DWORD reader) {
        if (hr < 0 || !reader) return;
        BYTE* keys = (BYTE*)(reader + 0xDC);
        const bool focus = wasort::gameHasFocus();
        const bool alt = focus && (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0;
        const bool left = focus && (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
        const bool right = focus && (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
        const bool up = focus && (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
        const bool down = focus && (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
        const bool del = focus && (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
        if (focus && ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000)) g_lastKeyboard = true;
        for (int k = 0; k < 256 && !g_lastKeyboard; ++k) if (keys[k] & 0x80) g_lastKeyboard = true;
        const bool grid = ready();
        if (!g_kbMode && !g_padMode && onScreen() && alt) {
            g_kbMode = true;
            beginMode();
        }
        if (g_kbMode) {
            if (!alt || !grid) {
                g_kbMode = false;
                g_kbWaitUp = (left ? 1 : 0) | (right ? 2 : 0) | (up ? 4 : 0) | (down ? 8 : 0) | (del ? 16 : 0);
                endMode();
            }
            else {
                if (left && !g_kbPrevLeft) stepBuild(-1);
                if (right && !g_kbPrevRight) stepBuild(+1);
                g_holdApply = down;
                g_holdSave = up;
                g_holdDelete = del;
            }
        }
        g_kbPrevLeft = left;
        g_kbPrevRight = right;
        if (!left) g_kbWaitUp &= ~1;
        if (!right) g_kbWaitUp &= ~2;
        if (!up) g_kbWaitUp &= ~4;
        if (!down) g_kbWaitUp &= ~8;
        if (g_kbMode || (g_kbWaitUp & 1)) keys[kDikLeft] = 0;
        if (g_kbMode || (g_kbWaitUp & 2)) keys[kDikRight] = 0;
        if (g_kbMode || (g_kbWaitUp & 4)) keys[kDikUp] = 0;
        if (g_kbMode || (g_kbWaitUp & 8)) keys[kDikDown] = 0;
        if (!del) g_kbWaitUp &= ~16;
        if (g_kbMode || (g_kbWaitUp & 16)) keys[kDikDelete] = 0;
    }
}

bool installEquipmentLoadouts() {
    using namespace wasort;
    namespace lo = loadouts;
    if (lo::g_installed) return true;
    if (!Settings::get().getEquipmentLoadouts()) return false;
    const DWORD keyboardSite = 0x006447C8, xinputSite = 0x00ABA542;
    const DWORD entry = lo::kVtGrid + lo::kFillMethod * 4;
    bool ok = rd32(entry) == lo::kFillOriginal;
    for (int i = 0; i < 9 && ok; ++i)
        if (memcmp((const void*)lo::kCode[i].address, lo::kCode[i].bytes, sizeof(lo::kCode[i].bytes)) != 0) ok = false;
    if (!g_keyboardPatched && memcmp((const void*)keyboardSite, kExpKeyboard, sizeof(kExpKeyboard)) != 0) ok = false;
    if (!g_xinputPatched && memcmp((const void*)xinputSite, kExpXInput, sizeof(kExpXInput)) != 0) ok = false;
    if (!ok) {
        SDLOG(0, "EquipmentLoadouts: the game code is not what this build should have, it stays off\n");
        return false;
    }
    if (!lo::g_lockReady) { InitializeCriticalSection(&lo::g_lock); lo::g_lockReady = true; }
    memset(lo::g_fakeList, 0, sizeof(lo::g_fakeList));
    // the input hooks first: without the grid hook they do nothing (the grid never has focus)
    g_loadPad = &lo::onPad;
    g_loadKeyboard = &lo::onKeyboard;
    if (!g_xinputPatched) {
        if (!writeJump(xinputSite, sizeof(kExpXInput), (const void*)&sortXInputGetState)) {
            g_loadPad = NULL; g_loadKeyboard = NULL;
            SDLOG(0, "EquipmentLoadouts: could not write at 0x%08X, it stays off\n", xinputSite);
            return false;
        }
        g_xinputPatched = true;
    }
    if (!g_keyboardPatched) {
        if (!writeJump(keyboardSite, sizeof(kExpKeyboard), (const void*)&keyboard_stub)) {
            g_loadPad = NULL; g_loadKeyboard = NULL;   // the XInput hook stays, but does nothing for this feature
            SDLOG(0, "EquipmentLoadouts: could not write at 0x%08X, it stays off\n", keyboardSite);
            return false;
        }
        g_keyboardPatched = true;
    }
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)entry, 4, PAGE_READWRITE, &old)) {
        g_loadPad = NULL; g_loadKeyboard = NULL;
        SDLOG(0, "EquipmentLoadouts: could not write the slot grid's method, it stays off\n");
        return false;
    }
    lo::g_installed = true;
    *(DWORD*)entry = (DWORD)&lo::onFill;
    DWORD ignored = 0;
    VirtualProtect((LPVOID)entry, 4, old, &ignored);
    HANDLE h = CreateThread(NULL, 0, lo::loadThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedExchange(&lo::g_loaded, 1);   // no file then: the builds live while the game runs
    SDLOG(0, "EquipmentLoadouts: on (Equipment window: hold RS / LALT, D-pad / arrows)\n");
    return true;
}

bool equipmentLoadoutsActive() {
    return loadouts::g_installed;
}

bool equipmentLoadoutsPrompt(LoadoutPromptInfo* out) {
    namespace lo = loadouts;
    if (!out || !lo::updatedThisFrame()) return false;
    const DWORD now = GetTickCount();
    out->held = lo::g_padMode ? 1 : (lo::g_kbMode ? 2 : 0);
    out->ownKeyboard = lo::g_lastKeyboard ? 1 : 0;
    out->build = lo::g_sel;
    out->saved = lo::g_savedMask;
    out->worn = lo::g_worn;
    out->state = 0;
    out->progress = 0.0f;
    // elapsed times are signed (a start a moment after 'now' counts as 0)
    if (out->held && lo::g_saveOn) {
        out->state = 2;
        out->progress = (float)(int)(now - lo::g_saveStart) / (float)lo::kSaveMs;
    }
    else if (out->held && lo::g_deleteOn) {
        out->state = 5;
        out->progress = (float)(int)(now - lo::g_deleteStart) / (float)lo::kDeleteMs;
    }
    else if (out->held && lo::g_applyOn) {
        out->state = 1;
        out->progress = (float)(int)(now - lo::g_applyStart) / (float)lo::kApplyMs;
    }
    else if (out->held && lo::g_msg) {
        out->state = lo::g_msg == 1 ? 3 : (lo::g_msg == 2 ? 4 : 6);
        out->build = lo::g_msgBuild;
        out->progress = 1.0f - (float)(int)(now - lo::g_msgTime) / (float)lo::kMessageMs;
    }
    if (out->progress < 0.0f) out->progress = 0.0f;
    if (out->progress > 1.0f) out->progress = 1.0f;
    return true;
}

unsigned equipmentLoadoutsDimMask(unsigned* shown, unsigned* fill) {
    namespace lo = loadouts;
    const bool on = lo::onScreen() && lo::modeOn() && lo::g_previewOn;
    if (shown) *shown = on ? lo::g_shownMask : 0;
    if (fill) *fill = on ? lo::g_fillMask : 0;
    return on ? lo::g_dimMask : 0;
}

// for RenderstateManager (at Present): one more frame shown
void equipmentLoadoutsNewFrame() { InterlockedIncrement(&loadouts::g_frame); }

// "Most recent" sorting: the look at what the hero gains, equips and attunes (WeaponsArmorSort, above). Called every frame
// from RSManager::redirectPresent; it works 4 times a second, and does nothing when WeaponsArmorSort is off.
void wasortFrameTick() { wasort::recentTick(); }

// =====================================================================================================================
// NewItemMarkers: a glowing orange "!" on the items the hero got and has not looked at yet, in the Inventory, the
// Equipment "select item" lists and the Bottomless Box, and on the tab icons of the Inventory and the Bottomless Box
// whose tab holds such an item (drawn by RenderstateManager.cpp, picture Resources\NewItem.png).
//   - new: an inventory slot that is new, or holds more of its item (not the Estus Flask filling). Not when the same item
//     left the Bottomless Box in that moment (taken out of the box). An upgrade or infusion keeps a mark it had.
//   - seen: when the cursor leaves its row (selected, then deselected), a mark fades out over 1 s, then goes (also when
//     the cursor comes back to the row meanwhile). Opening a list, changing tab and closing take no mark.
//   - the marks are kept per character (save slot + name) in %LOCALAPPDATA%\NBGI\DarkSouls\DSfixNewItems.ini.
// Own look at the inventory (4 times a second at Present), own file: nothing shared with WeaponsArmorSort but the two list
// hooks (1 and 3), which it puts in place itself when they are not there yet.
// =====================================================================================================================
namespace wasort {
namespace newitems {
    struct NewSet {
        int used;
        wchar_t name[kNameLen];
        BYTE flag[kPositions];                    // 1 = the item at this inventory position is new
        DWORD kid[kPositions];                    // the kind | id it was new for
    };
    NewSet g_sets[kRecentSlots];
    CRITICAL_SECTION g_lock;                      // the save thread copies the sets while the game changes them
    bool g_lockReady = false;
    bool g_installed = false;
    volatile LONG g_loaded = 0;                   // the file was read: nothing is written before
    volatile LONG g_dirty = 0;                    // marks changed since the last write
    volatile LONG g_saveBusy = 0;
    volatile LONG g_saveDirty = 0;
    DWORD g_dirtySince = 0;
    int g_active = -1;                            // the save slot of the character in play
    signed char g_tab[kPositions];                // the tab of the marked item (-2 not known yet, -1 none); game thread
    DWORD g_markTime[kPositions];                 // when the mark came (0: from the file)

    // the look (own copies: independent of "Most recent")
    BYTE g_inv[kInvMax * 28];
    BYTE g_box[kBoxMax * 32];
    Seen g_seen[kInvMax];
    DWORD g_seenCount = 0;
    DWORD g_boxKid[kBoxMax], g_boxCnt[kBoxMax];
    DWORD g_boxSeenN = 0, g_boxN = 0;
    bool g_have = false;
    DWORD g_havePlayer = 0;
    DWORD g_lastPoll = 0;

    // the list with focus (hook 3, game thread)
    volatile DWORD g_shownList = 0;
    volatile unsigned g_tabMask = 0;              // bit i: tab i of the shown Inventory / Bottomless Box list has a mark
    volatile BYTE g_tabAlpha[8];                  // the most visible mark of each tab (255; less while it fades)
    DWORD g_lastList = 0, g_lastTick = 0;
    int g_lastWindow = W_NONE, g_lastCategory = -99, g_lastIndex = -1, g_lastPos = -1;
    DWORD g_lastKid = 0xFFFFFFFF;                 // the item of the row under the cursor (last tick)
    // marks the cursor left: they fade out over 1 s, then go (so a mark passed while scrolling is still seen); with g_lock
    const DWORD kSeenDelayMs = 1000;
    struct Later { int pos; DWORD kid; DWORD due; };
    Later g_later[64];
    volatile int g_nLater = 0;
    DWORD g_cleanAt = 0;                          // when the open Inventory tab is checked for marks it cannot show

    void setDirty() {
        if (!g_dirty) g_dirtySince = GetTickCount();
        InterlockedExchange(&g_dirty, 1);
    }

    void mark(int pos, DWORD kid) {
        if (g_active < 0 || pos < 0 || pos >= kPositions) return;
        NewSet& s = g_sets[g_active];
        if (s.flag[pos] && s.kid[pos] == kid) return;
        EnterCriticalSection(&g_lock);
        s.flag[pos] = 1;
        s.kid[pos] = kid;
        LeaveCriticalSection(&g_lock);
        g_tab[pos] = -2;
        g_markTime[pos] = GetTickCount() | 1;
        setDirty();
    }
    void clear(int pos) {
        if (g_active < 0 || pos < 0 || pos >= kPositions) return;
        NewSet& s = g_sets[g_active];
        if (!s.flag[pos]) return;
        EnterCriticalSection(&g_lock);
        s.flag[pos] = 0;
        LeaveCriticalSection(&g_lock);
        setDirty();
    }
    // the character in play: its block (an empty one when the slot has none or another name)
    void activate(int slot, const wchar_t* name) {
        NewSet& s = g_sets[slot];
        if (!s.used || wcsncmp(s.name, name, kNameLen) != 0) {
            s.used = 1;
            wcsncpy_s(s.name, kNameLen, name, _TRUNCATE);
            memset(s.flag, 0, sizeof(s.flag));
            memset(s.kid, 0, sizeof(s.kid));
        }
        g_active = slot;
        g_nLater = 0;
        memset(g_tab, 0xFE, sizeof(g_tab));       // -2: not known
        memset(g_markTime, 0, sizeof(g_markTime));
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The file: Character=<save slot> <name>, then one New=<inventory position> <kind | id in hex> per mark.
    // ---------------------------------------------------------------------------------------------------------------
    bool filePath(wchar_t* dir1, wchar_t* dir2, wchar_t* file, wchar_t* tmp, DWORD size) {
        wchar_t base[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 64) return false;
        _snwprintf_s(dir1, size, _TRUNCATE, L"%s\\NBGI", base);
        _snwprintf_s(dir2, size, _TRUNCATE, L"%s\\NBGI\\DarkSouls", base);
        _snwprintf_s(file, size, _TRUNCATE, L"%s\\NBGI\\DarkSouls\\DSfixNewItems.ini", base);
        _snwprintf_s(tmp, size, _TRUNCATE, L"%s\\NBGI\\DarkSouls\\DSfixNewItems.ini.tmp", base);
        return true;
    }

    void writeFile() {
        if (!g_loaded) return;
        wchar_t dir1[MAX_PATH], dir2[MAX_PATH], file[MAX_PATH], tmp[MAX_PATH];
        if (!filePath(dir1, dir2, file, tmp, MAX_PATH)) return;
        HANDLE heap = GetProcessHeap();
        NewSet* sets = (NewSet*)HeapAlloc(heap, 0, sizeof(g_sets));
        if (!sets) return;
        EnterCriticalSection(&g_lock);
        memcpy(sets, g_sets, sizeof(g_sets));
        LeaveCriticalSection(&g_lock);
        size_t cap = 256;
        for (int k = 0; k < kRecentSlots; ++k) if (sets[k].used) cap += 128 + (size_t)kPositions * 24;
        char* text = (char*)HeapAlloc(heap, 0, cap);
        if (!text) { HeapFree(heap, 0, sets); return; }
        int len = 0;
        text[0] = 0;
        for (int k = 0; k < kRecentSlots && len >= 0; ++k) {
            const NewSet& s = sets[k];
            if (!s.used) continue;
            char name[64] = { 0 };
            if (!WideCharToMultiByte(CP_UTF8, 0, s.name, -1, name, sizeof(name), NULL, NULL)) name[0] = 0;
            int m = _snprintf_s(text + len, cap - len, _TRUNCATE, "Character=%d %s\r\n", k, name);
            len = m < 0 ? -1 : len + m;
            for (int i = 0; i < kPositions && len >= 0; ++i) {
                if (!s.flag[i]) continue;
                m = _snprintf_s(text + len, cap - len, _TRUNCATE, "New=%d %08X\r\n", i, s.kid[i]);
                len = m < 0 ? -1 : len + m;
            }
        }
        HeapFree(heap, 0, sets);
        if (len >= 0) {
            CreateDirectoryW(dir1, NULL);
            CreateDirectoryW(dir2, NULL);
            HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                const BOOL ok = (len == 0 || WriteFile(h, text, (DWORD)len, &written, NULL)) && written == (DWORD)len;
                CloseHandle(h);
                if (!ok || !MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) DeleteFileW(tmp);
            }
        }
        HeapFree(heap, 0, text);
    }

    DWORD WINAPI saveThread(LPVOID) {
        do {
            while (InterlockedExchange(&g_saveDirty, 0)) writeFile();
            InterlockedExchange(&g_saveBusy, 0);
        } while (g_saveDirty && InterlockedCompareExchange(&g_saveBusy, 1, 0) == 0);
        return 0;
    }
    void requestSave() {
        InterlockedExchange(&g_saveDirty, 1);
        if (InterlockedCompareExchange(&g_saveBusy, 1, 0) != 0) return;
        HANDLE h = CreateThread(NULL, 0, saveThread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        else InterlockedExchange(&g_saveBusy, 0);
    }

    // Returns false when the file is there but something in it is wrong (it is then written anew). With g_lock held.
    bool readFile(int* marks) {
        *marks = 0;
        wchar_t dir1[MAX_PATH], dir2[MAX_PATH], file[MAX_PATH], tmp[MAX_PATH];
        if (!filePath(dir1, dir2, file, tmp, MAX_PATH)) return true;
        HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return true;   // no file yet: nothing to write either
        const DWORD size = GetFileSize(h, NULL);
        if (size == INVALID_FILE_SIZE || size > 4 * 1024 * 1024) { CloseHandle(h); return false; }
        HANDLE heap = GetProcessHeap();
        char* text = (char*)HeapAlloc(heap, 0, (SIZE_T)size + 1);
        if (!text) { CloseHandle(h); return false; }
        DWORD got = 0;
        const BOOL ok = ReadFile(h, text, size, &got, NULL);
        CloseHandle(h);
        if (!ok) { HeapFree(heap, 0, text); return false; }
        text[got] = 0;
        bool good = true;
        int cur = -1;
        char* line = text;
        while (*line) {
            char* end = line;
            while (*end && *end != '\n' && *end != '\r') ++end;
            const char keep = *end;
            *end = 0;
            if (*line) {
                const char* v = NULL;
                int value = -1;
                unsigned int a = 0, b = 0;
                char extra = 0;
                if ((v = valueOf(line, "Character=")) != NULL) {
                    const char* space = strchr(v, ' ');
                    if (space && sscanf_s(v, "%d", &value) == 1 && value >= 0 && value < kRecentSlots) {
                        NewSet& s = g_sets[value];
                        s.used = 1;
                        memset(s.flag, 0, sizeof(s.flag));
                        memset(s.kid, 0, sizeof(s.kid));
                        memset(s.name, 0, sizeof(s.name));
                        if (!MultiByteToWideChar(CP_UTF8, 0, space + 1, -1, s.name, kNameLen)) memset(s.name, 0, sizeof(s.name));
                        s.name[kNameLen - 1] = 0;
                        cur = value;
                    }
                    else { good = false; cur = -1; }
                }
                else if ((v = valueOf(line, "New=")) != NULL) {
                    if (cur >= 0 && sscanf_s(v, "%u %x%c", &a, &b, &extra, 1) == 2 && a < (unsigned)kPositions) {
                        g_sets[cur].flag[a] = 1;
                        g_sets[cur].kid[a] = b;
                        ++*marks;
                    }
                    else good = false;
                }
                else good = false;
            }
            if (!keep) break;
            line = end + 1;
        }
        HeapFree(heap, 0, text);
        return good;
    }

    DWORD WINAPI loadThread(LPVOID) {
        int marks = 0;
        EnterCriticalSection(&g_lock);
        const bool good = readFile(&marks);
        LeaveCriticalSection(&g_lock);
        InterlockedExchange(&g_loaded, 1);
        if (!good) SDLOG(0, "NewItemMarkers: DSfixNewItems.ini had wrong lines, it is written anew\n");
        if (!good) requestSave();
        return 0;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The look (Present, 4 times a second)
    // ---------------------------------------------------------------------------------------------------------------
    void slotAt(DWORD i, DWORD count, Seen& out) {
        if (i >= count) { out.kid = 0; out.count = 0; out.used = 0; return; }
        const BYTE* s = g_inv + i * 28;
        out.used = s[0x10] ? 1 : 0;
        out.kid = *(const DWORD*)s | *(const DWORD*)(s + 4);
        out.count = *(const DWORD*)(s + 8);
    }
    void boxAt(DWORD i, DWORD boxN, DWORD& kid, DWORD& cnt) {
        const BYTE* e = g_box + i * 32;
        const bool used = i < boxN && (e[0xA] & 1);
        kid = used ? *(const DWORD*)e : 0;
        cnt = used ? *(const WORD*)(e + 8) : 0;
    }
    // some of it is in the Bottomless Box (a slot can stay at 0 held after all of it went into the box)
    bool inBox(DWORD kid) {
        for (DWORD i = 0; i < g_boxN; ++i) {
            DWORD k, c;
            boxAt(i, g_boxN, k, c);
            if (k == kid && c > 0) return true;
        }
        return false;
    }

    void runLater();
    void frameTick() {
        if (!g_installed || !g_loaded) return;
        runLater();
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
        // the marks are written 10 s after a change, or at once when play stops (loading screen, title)
        if (g_dirty && (!inPlay || now - g_dirtySince >= kFlushMs)) {
            InterlockedExchange(&g_dirty, 0);
            requestSave();
        }
        if (!inPlay) { g_have = false; return; }
        wchar_t name[kNameLen];
        memset(name, 0, sizeof(name));
        if (!rdBlock(player + 0xA0, name, sizeof(wchar_t) * (kNameLen - 1)) || !name[0]) { g_have = false; return; }
        if (g_active != (int)saveSlot || wcsncmp(g_sets[saveSlot].name, name, kNameLen) != 0) {
            EnterCriticalSection(&g_lock);
            activate((int)saveSlot, name);
            LeaveCriticalSection(&g_lock);
            g_have = false;
        }
        if (player != g_havePlayer) { g_have = false; g_havePlayer = player; }

        const DWORD inv = player + 0x1B8;
        DWORD count = rd32(inv + 0x110), split = rd32(inv + 0x118);
        const DWORD blockA = rd32(inv + 0x120), blockB = rd32(inv + 0x124);
        if (count > kInvMax) count = kInvMax;
        if (split > count) split = count;
        if (!blockA || (count > split && !blockB) || !rdBlock(blockA, g_inv, split * 28) ||
            !rdBlock(blockB + split * 28, g_inv + split * 28, (count - split) * 28)) { g_have = false; return; }
        const DWORD box = rd32(player + 0x420);
        DWORD boxN = box ? rd32(box + 0x10008) + 1 : 0;
        if (boxN > kBoxMax) boxN = kBoxMax;
        if (boxN && !rdBlock(box + 8, g_box, boxN * 32)) boxN = 0;
        g_boxN = boxN;
        NewSet& s = g_sets[g_active];

        if (!g_have) {
            // the first look: a mark stays only where its item still is
            for (int i = 0; i < kPositions; ++i) {
                if (!s.flag[i]) continue;
                Seen cur;
                slotAt((DWORD)i, count, cur);
                if (!cur.used || cur.kid != s.kid[i] || (cur.count == 0 && !inBox(cur.kid))) clear(i);
            }
            for (DWORD i = 0; i < kInvMax; ++i) slotAt(i, count, g_seen[i]);
            g_seenCount = count;
            for (DWORD i = 0; i < kBoxMax; ++i) boxAt(i, boxN, g_boxKid[i], g_boxCnt[i]);
            g_boxSeenN = boxN;
            g_have = true;
            return;
        }

        // what left the box in this moment
        DWORD leftBox[256];
        int nLeft = 0;
        const DWORD boxUpto = boxN > g_boxSeenN ? boxN : g_boxSeenN;
        for (DWORD i = 0; i < boxUpto; ++i) {
            DWORD kid, cnt;
            boxAt(i, boxN, kid, cnt);
            if (g_boxKid[i] && (kid != g_boxKid[i] || cnt < g_boxCnt[i]) && nLeft < 256) leftBox[nLeft++] = g_boxKid[i];
            g_boxKid[i] = kid;
            g_boxCnt[i] = cnt;
        }
        g_boxSeenN = boxN;

        DWORD found[32];
        int foundPos[32];
        int nFound = 0, nGains = 0;
        const DWORD upto = count > g_seenCount ? count : g_seenCount;
        for (DWORD i = 0; i < upto; ++i) {
            Seen cur;
            slotAt(i, count, cur);
            const Seen was = g_seen[i];
            g_seen[i] = cur;
            // a mark follows its item
            if (i < (DWORD)kPositions && s.flag[i]) {
                if (!cur.used) clear((int)i);
                else if (cur.kid != s.kid[i]) {
                    if (was.used && was.kid == s.kid[i]) {   // upgraded or infused: the mark stays
                        EnterCriticalSection(&g_lock);
                        s.kid[i] = cur.kid;
                        LeaveCriticalSection(&g_lock);
                        g_tab[i] = -2;
                        setDirty();
                    }
                    else clear((int)i);
                }
                else if (cur.count == 0 && was.count > 0 && !inBox(cur.kid)) clear((int)i);
            }
            if (!cur.used) continue;
            const bool gained = !was.used || (cur.kid == was.kid && cur.count > was.count && !isEstus(cur.kid));
            if (!gained) continue;
            bool fromBox = false;
            for (int k = 0; k < nLeft; ++k) if (leftBox[k] == cur.kid) fromBox = true;
            if (fromBox) continue;                    // taken out of the Bottomless Box: not new
            ++nGains;
            if (nFound < 32) { found[nFound] = cur.kid; foundPos[nFound] = (int)i; ++nFound; }
        }
        g_seenCount = count;
        if (nGains > kMassGains) return;          // the game loading: not marked
        for (int k = 0; k < nFound; ++k) mark(foundPos[k], found[k]);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The tab of an item, as the game's classifier (0x00C68390, normal branch) gives it: 0 consumables, 1 materials,
    // 2 key items, 3 spells, 4 weapons and shields, 5 arrows / bolts, 6 armor, 7 rings, -1 none. Game thread only.
    // ---------------------------------------------------------------------------------------------------------------
    __declspec(naked) void __stdcall goodsLookup(DWORD id, DWORD* out) {
        __asm {
            pushad
            mov edi, dword ptr[esp + 0x24]      // id (the lookup at 0x00D1E3A0 takes it in edi)
            push dword ptr[esp + 0x28]          // out (the function removes it): +4 the goods row
            mov eax, 0x00D1E3A0
            call eax
            popad
            ret 8
        }
    }
    int tabOf(DWORD kid) {
        const DWORD kind = kid & 0xF0000000, id = kid & 0x0FFFFFFF;
        if (kind == 0x10000000) return 6;
        if (kind == 0x20000000) return 7;
        if (kind == 0) {
            DWORD out[8] = { 0 };
            gameLookup(kFnWeaponLookup, id, out);
            if (!out[1]) return 4;
            const DWORD category = rd32(out[1] + kWepCategory) & 0xFF;
            return (category == 13 || category == 14) ? 5 : 4;
        }
        if (kind == 0x40000000) {
            DWORD out[8] = { 0 };
            goodsLookup(id, out);
            if (!out[1]) return -1;
            switch (rd32(out[1] + 0x3A) & 0xFF) {
            case 0: case 3: case 4: return 0;
            case 2: return 1;
            case 1: return 2;
            case 5: return 3;
            default: return -1;
            }
        }
        return -1;
    }

    // the inventory position (-1: none) and kind | id of the row 'index' of a list
    bool rowAt(DWORD list, int window, DWORD player, int index, int* pos, DWORD* kid) {
        *pos = -1;
        *kid = 0xFFFFFFFF;
        const DWORD entriesVec = rd32(list + kListEntries);
        const DWORD orderVec = rd32(list + kListOrder);
        if (!player || !entriesVec || !orderVec || index < 0) return false;
        const DWORD eBegin = rd32(entriesVec + 4), eEnd = rd32(entriesVec + 8);
        const DWORD oBegin = rd32(orderVec + 4), oEnd = rd32(orderVec + 8);
        if (!eBegin || !oBegin || eEnd < eBegin || oEnd < oBegin) return false;
        const int entries = (int)((eEnd - eBegin) / 4);
        const int n = (int)((oEnd - oBegin) / 4);
        if (index >= n) return false;
        const int e = (int)rd32(oBegin + 4 * (DWORD)index);
        if (e < 0 || e >= entries) return false;
        *kid = entryKid(window, player, rd32(eBegin + 4 * (DWORD)e), pos);
        if (*pos >= kPositions) *pos = -1;
        return true;
    }

    // the cursor left the row of a mark: it fades out over 1 s, then goes. A fade that started goes on to the end (the
    // cursor coming back to the row does not stop it).
    void seenLater(int pos, DWORD kid) {
        if (g_active < 0 || pos < 0 || pos >= kPositions) return;
        const NewSet& s = g_sets[g_active];
        if (!s.flag[pos] || s.kid[pos] != kid) return;
        const DWORD due = GetTickCount() + kSeenDelayMs;
        bool full = false;
        EnterCriticalSection(&g_lock);
        int k = 0;
        while (k < g_nLater && g_later[k].pos != pos) ++k;
        if (k < g_nLater && g_later[k].kid == kid) {   // already fading: it keeps its end
            LeaveCriticalSection(&g_lock);
            return;
        }
        if (k < g_nLater || g_nLater < 64) {
            g_later[k].pos = pos;
            g_later[k].kid = kid;
            g_later[k].due = due;
            if (k == g_nLater) ++g_nLater;
        }
        else full = true;
        LeaveCriticalSection(&g_lock);
        if (full) clear(pos);
    }
    // the marks whose fade has ended (from the list tick and from Present, so also after the list closed)
    void runLater() {
        if (!g_nLater || g_active < 0) return;
        const DWORD now = GetTickCount();
        Later due[64];
        int n = 0;
        EnterCriticalSection(&g_lock);
        for (int k = 0; k < g_nLater; )
            if ((LONG)(now - g_later[k].due) >= 0) { due[n++] = g_later[k]; g_later[k] = g_later[--g_nLater]; }
            else ++k;
        LeaveCriticalSection(&g_lock);
        const NewSet& s = g_sets[g_active];
        for (int k = 0; k < n; ++k)
            if (s.flag[due[k].pos] && s.kid[due[k].pos] == due[k].kid) clear(due[k].pos);
    }
    // how visible the mark at 'pos' is: 255, less while it fades out (a few entries at most; any thread)
    BYTE alphaOf(int pos, DWORD now) {
        const int n = g_nLater;
        for (int k = 0; k < n && k < 64; ++k) {
            if (g_later[k].pos != pos) continue;
            const LONG left = (LONG)(g_later[k].due - now);
            if (left <= 0) return 1;
            if (left >= (LONG)kSeenDelayMs) return 255;
            const DWORD a = (DWORD)left * 255 / kSeenDelayMs;
            return (BYTE)(a < 1 ? 1 : a);
        }
        return 255;
    }

    // The open Inventory tab lists every item of it the hero holds: a mark on an item of that tab that is not in the list
    // (an item the game hides) could never be seen, so it goes. Only for marks older than 3 s, half a second after the
    // tab opened (the list is rebuilt by then).
    void cleanTab(DWORD list, int category, DWORD now) {
        const NewSet& s = g_sets[g_active];
        const DWORD player = playerData();
        const DWORD entriesVec = rd32(list + kListEntries);
        if (!player || !entriesVec) return;
        const DWORD eBegin = rd32(entriesVec + 4), eEnd = rd32(entriesVec + 8);
        if (!eBegin || eEnd < eBegin) return;
        const int entries = (int)((eEnd - eBegin) / 4);
        if (entries <= 0 || entries > 4096) return;
        for (int i = 0; i < kPositions; ++i) {
            if (!s.flag[i] || g_tab[i] != category) continue;
            if (g_markTime[i] && now - g_markTime[i] < 3000) continue;
            const DWORD slot = inventorySlot(player, (DWORD)i);
            if (!slot || rd32(slot + 8) == 0) continue;   // none held (all in the box): the Inventory does not list it
            bool listed = false;
            for (int e = 0; e < entries && !listed; ++e) if ((int)rd32(eBegin + 4 * (DWORD)e) == i) listed = true;
            if (!listed) clear(i);
        }
    }

    // hook 3: every frame the list with focus takes input (game thread)
    void __stdcall tick(DWORD list) {
        const int window = windowOf(list);
        const DWORD mm = rd32(kMenuMan);
        if (g_active < 0 || window == W_NONE || (window == W_INVENTORY && (!mm || rd32(mm + 0x1F0) != 0))) {
            g_shownList = 0;                      // a shop or another list: no marks
            g_tabMask = 0;
            return;
        }
        const DWORD now = GetTickCount();
        const int category = window == W_EQUIP ? (mm ? (int)rd32(mm + 0x134) : -1) : cachedCategory(list);
        const int index = (int)rd32(list + kListCursor) + (int)rd32(list + kListScroll);
        const DWORD player = playerData();
        int pos = -1;
        DWORD kid = 0xFFFFFFFF;
        rowAt(list, window, player, index, &pos, &kid);
        const bool open = list != g_lastList || window != g_lastWindow || category != g_lastCategory || now - g_lastTick > 150;
        // a mark goes when the cursor leaves its row: the row was selected, then deselected (opening, changing tab and
        // closing take nothing)
        if (open) g_cleanAt = (now + 500) | 1;
        else if (index != g_lastIndex) seenLater(g_lastPos, g_lastKid);
        runLater();
        g_lastList = list;
        g_lastWindow = window;
        g_lastCategory = category;
        g_lastIndex = index;
        g_lastPos = pos;
        g_lastKid = kid;
        g_lastTick = now;
        g_shownList = list;

        // the tab icons (Inventory and Bottomless Box)
        if (window == W_EQUIP) { g_tabMask = 0; return; }
        if (window == W_INVENTORY && g_cleanAt && now - g_cleanAt < 0x80000000u && category >= 0 && category <= 7) {
            g_cleanAt = 0;
            cleanTab(list, category, now);
        }
        unsigned mask = 0;
        BYTE alpha[8] = { 0 };
        const NewSet& s = g_sets[g_active];
        const DWORD aNow = GetTickCount();
        for (int i = 0; i < kPositions; ++i) {
            if (!s.flag[i]) continue;
            if (g_tab[i] == -2) g_tab[i] = (signed char)tabOf(s.kid[i]);
            if (g_tab[i] < 0 || g_tab[i] > 7) continue;
            mask |= 1u << g_tab[i];
            const BYTE a = alphaOf(i, aNow);      // a tab's mark fades with the last mark of the tab
            if (a > alpha[g_tab[i]]) alpha[g_tab[i]] = a;
        }
        for (int t = 0; t < 8; ++t) g_tabAlpha[t] = alpha[t];
        g_tabMask = mask;
    }
}
}

bool installNewItemMarkers() {
    using namespace wasort;
    if (newitems::g_installed) return true;
    if (!Settings::get().getNewItemMarkers()) return false;
    const DWORD refreshSite = 0x00CA0985, tickSite = 0x00C8AFAA;
    if ((!g_refreshPatched && memcmp((const void*)refreshSite, kExpRefresh, sizeof(kExpRefresh)) != 0) ||
        (!g_tickPatched && memcmp((const void*)tickSite, kExpTick, sizeof(kExpTick)) != 0)) {
        SDLOG(0, "NewItemMarkers: the game code is not what this build should have, markers stay off\n");
        return false;
    }
    if (!newitems::g_lockReady) {
        InitializeCriticalSection(&newitems::g_lock);
        newitems::g_lockReady = true;
    }
    memset(newitems::g_tab, 0xFE, sizeof(newitems::g_tab));
    g_newTick = &newitems::tick;                 // before the hooks: hook 3 may already be live
    if (!g_refreshPatched) {
        if (!writeJump(refreshSite, sizeof(kExpRefresh), (const void*)&refresh_stub)) {
            g_newTick = NULL;
            SDLOG(0, "NewItemMarkers: could not write at 0x%08X, markers stay off\n", refreshSite);
            return false;
        }
        g_refreshPatched = true;
    }
    if (!g_tickPatched) {
        if (!writeJump(tickSite, sizeof(kExpTick), (const void*)&tick_stub)) {
            g_newTick = NULL;                    // hook 1 stays (it only notes the lists)
            SDLOG(0, "NewItemMarkers: could not write at 0x%08X, markers stay off\n", tickSite);
            return false;
        }
        g_tickPatched = true;
    }
    newitems::g_installed = true;
    HANDLE h = CreateThread(NULL, 0, newitems::loadThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedExchange(&newitems::g_loaded, 1);
    SDLOG(0, "NewItemMarkers: on\n");
    return true;
}

bool newItemMarkersActive() {
    return wasort::newitems::g_installed;
}

void newItemsFrameTick() { wasort::newitems::frameTick(); }

int newItemMarkerRows(unsigned char* flags, int max) {
    using namespace wasort;
    namespace ni = wasort::newitems;
    if (!ni::g_installed || !flags || max <= 0 || ni::g_active < 0) return 0;
    const DWORD list = ni::g_shownList;
    const int window = list ? windowOf(list) : W_NONE;
    if (window == W_NONE || !(rd32(list + 0x98) & 0xFF)) return 0;   // +0x98: the list is shown
    const DWORD player = playerData();
    const int scroll = (int)rd32(list + kListScroll);
    if (!player || scroll < 0) return 0;
    const ni::NewSet& s = ni::g_sets[ni::g_active];
    const DWORD now = GetTickCount();
    int rows = 0;
    for (; rows < max; ++rows) {
        int pos = -1;
        DWORD kid = 0xFFFFFFFF;
        if (!ni::rowAt(list, window, player, scroll + rows, &pos, &kid)) break;
        flags[rows] = (pos >= 0 && s.flag[pos] && s.kid[pos] == kid) ? ni::alphaOf(pos, now) : 0;
    }
    return rows;
}

unsigned newItemTabMask(unsigned char* alpha8) {
    using namespace wasort;
    namespace ni = wasort::newitems;
    if (!ni::g_installed) return 0;
    const DWORD list = ni::g_shownList;
    const int window = list ? windowOf(list) : W_NONE;
    if ((window != W_INVENTORY && window != W_BOX) || !(rd32(list + 0x98) & 0xFF)) return 0;
    if (alpha8) for (int t = 0; t < 8; ++t) alpha8[t] = ni::g_tabAlpha[t];
    return ni::g_tabMask;
}
