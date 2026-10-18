#include "BulkItemUse.h"
#include "Settings.h" // SDLOG reads the log level when RELEASE_VER is not defined
#include "main.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

// =====================================================================================================================
// bulkItemUse - use several soul items at once.
//
// Research credit: the Dark Souls Overhaul team (metal-crow, Sean Pesce). The addresses below and the idea of the
// technique ("Multi-Consume") come from their work. The code in this file is DSfix's own.
//
// What happens when you use a soul item in the inventory menu:
//   1. The game is about to use one item. We stop that use, remember which item it was, and open the game's own number
//      picker (the same dialog the game uses for "how many to discard").
//   2. When you press OK we remember the number, then tell the game to use the item again - this time without the
//      dialog.
//   3. While that single use runs, two small corrections are made: the inventory count goes down by the chosen number
//      instead of by 1, and the souls you receive are multiplied by the chosen number.
//
// So the game still performs exactly one normal item use (one animation, one sound). Only the two numbers are
// corrected. Nothing is invented: the souls per item come from the game's own award, read at the moment it happens.
//
// Only works from the pause Inventory menu. The quick-item button during normal play is left completely alone.
// =====================================================================================================================

// =====================================================================================================================
// WHICH ITEMS - the only items that get the number picker. Everything else in the game behaves exactly as before.
// Boss Souls, Fire Keeper Souls, Estus, keys, spells and gestures are not in this list and are never touched.
//
// Each row says what ONE of that item gives: souls, or humanity points, never both. That number is used twice:
//  * to work out the largest number the picker may offer, so nothing can be wasted at the game's limits;
//  * as a safety check - if the game grants a different amount than the row says, the correction is skipped and the
//    item behaves exactly as it always did.
//
// The two groups have their own settings (standardSoulsBulk and humanityBulk), so each can be turned off on its own.
// A group that is off is not in the list at all, and the hook that corrects it is never even written into the game.
// =====================================================================================================================
namespace {
    struct BulkItem { uint32_t id; uint32_t souls; uint32_t humanity; };
    const BulkItem kBulkItems[] = {
        // ordinary soul items - standardSoulsBulk
        { 400,   200, 0 }, // Soul of a Lost Undead
        { 401,   400, 0 }, // Large Soul of a Lost Undead
        { 402,   800, 0 }, // Soul of a Nameless Soldier
        { 403,  1000, 0 }, // Large Soul of a Nameless Soldier
        { 404,  2000, 0 }, // Soul of a Proud Knight
        { 405,  3000, 0 }, // Large Soul of a Proud Knight
        { 406,  5000, 0 }, // Soul of a Brave Warrior
        { 407,  8000, 0 }, // Large Soul of a Brave Warrior
        { 408, 10000, 0 }, // Soul of a Hero
        { 409, 20000, 0 }, // Soul of a Great Hero
        // humanity items - humanityBulk
        { 500,     0, 1 }, // Humanity
        { 501,     0, 2 }, // Twin Humanities
        // Fire Keeper Souls (390-396) and Boss Souls (700+) are deliberately absent: they ask their own Yes/No
        // question, which the picker would replace.
    };
    const uint32_t kBulkItemCount = sizeof(kBulkItems) / sizeof(kBulkItems[0]);

    // The largest number of souls the game can hold. The picker never offers more than fits under this.
    const uint32_t kMaxSouls = 999999999;
    // The most humanity the counter holds. The game clamps to this itself; the picker uses it so that none is wasted.
    const uint32_t kMaxHumanity = 99;

    // ---------------------------------------------------------------------------------------------------------------
    // TEXT SHOWN ABOVE THE NUMBER PICKER
    // The game has no "how many will you use?" line of its own, and new text cannot be added to its message table.
    // So one line that the table already has is borrowed: while our picker is on screen its text is replaced with
    // ours, and the moment the dialog closes the original is written back. Nothing else can show that line in
    // between, because our dialog is the only thing on screen while it is open.
    //
    // The borrowed line is message 12030, "Insufficient strength to wield this weapon with one hand...". It was
    // picked because it is the longest line in the table in every language: our words are shorter than it in all ten
    // (the shortest margin is Chinese, 37 characters against 10), so the replacement always fits where the original
    // stood and no memory has to be moved.
    //
    // The language looks after itself: the game's own text for all ten languages is listed below, and whichever one
    // is found in memory tells us which language the player is using, so overrideLanguage and the game's own
    // language setting both work with no extra setting.
    //
    // To change the wording, edit the second string of a pair. It must not be longer than the first one.
    // ---------------------------------------------------------------------------------------------------------------
    struct PickerText { const wchar_t* gameText; const wchar_t* ourText; };
    const PickerText kPickerText[] = {
        // ENGLISH: host 98 characters, ours 26
        { L"Insufficient strength to wield this weapon with one hand.\nHold with both hands to use effectively.",
          L"Use Items\nSpecify quantity" },
        // FRENCH: host 109 characters, ours 35
        { L"Vous n'avez pas assez de force pour utiliser cette arme \u00E0 une\nmain. Vous pouvez vous en \u00E9quiper \u00E0 deux mains.",
          L"Utiliser objets\nChoisir la quantit\u00E9" },
        // GERMAN: host 84 characters, ours 25
        { L"Zu schwach, um die Waffe einh\u00E4ndig zu f\u00FChren.\nDu musst sie mit beiden H\u00E4nden halten.",
          L"Benutzen\nAnzahl festlegen" },
        // ITALIAN: host 85 characters, ours 30
        { L"Forza insufficiente per usare l'arma con una mano.\nEquipaggiala con entrambe le mani.",
          L"Usa oggetti\nScegli la quantit\u00E0" },
        // SPANISH: host 86 characters, ours 30
        { L"No tienes fuerza para usar esto con una mano.\nPero puedes equiparla con las dos manos.",
          L"Usar objetos\nElige la cantidad" },
        // POLISH: host 98 characters, ours 28
        { L"Masz za ma\u0142o si\u0142y,\nby w\u0142ada\u0107 t\u0105 broni\u0105\njednor\u0105cz. Trzymaj j\u0105\nw obu r\u0119kach, aby\nwalczy\u0107 skutecznie.",
          L"U\u017Cyj przedmiot\u00F3w\nPodaj ilo\u015B\u0107" },
        // RUSSIAN: host 76 characters, ours 31
        { L"\u041D\u0435\u0434\u043E\u0441\u0442\u0430\u0442\u043E\u0447\u043D\u043E \u0441\u0438\u043B\u044B \u0434\u043B\u044F \u0438\u0441\u043F\u043E\u043B\u044C\u0437\u043E\u0432\u0430\u043D\u0438\u044F \u043E\u0434\u043D\u043E\u0439 \u0440\u0443\u043A\u043E\u0439. \u0412\u043E\u0437\u044C\u043C\u0438\u0442\u0435 \u043E\u0440\u0443\u0436\u0438\u0435 \u0432 \u043E\u0431\u0435 \u0440\u0443\u043A\u0438.",
          L"\u0418\u0441\u043F\u043E\u043B\u044C\u0437\u043E\u0432\u0430\u0442\u044C\n\u0423\u043A\u0430\u0436\u0438\u0442\u0435 \u043A\u043E\u043B\u0438\u0447\u0435\u0441\u0442\u0432\u043E" },
        // JAPANESE: host 45 characters, ours 17
        { L"\u3053\u306E\u6B66\u5668\u3092\u7247\u624B\u3067\u4F7F\u7528\u3059\u308B\u306B\u306F\u3001\u7B4B\u529B\u304C\u8DB3\u308A\u307E\u305B\u3093\n\u4E21\u624B\u6301\u3061\u306E\u5834\u5408\u306E\u307F\u3001\u5341\u5206\u306A\u6027\u80FD\u3092\u767A\u63EE\u3057\u307E\u3059",
          L"\u30A2\u30A4\u30C6\u30E0\u4F7F\u7528\n\u6570\u3092\u9078\u629E\u3057\u3066\u304F\u3060\u3055\u3044" },
        // KOREAN: host 44 characters, ours 16
        { L"\uD55C \uC190\uC73C\uB85C \uC0AC\uC6A9\uD560 \uADFC\uB825\uC774 \uBD80\uC871\uD569\uB2C8\uB2E4.\n\uC591\uC190\uC73C\uB85C \uC0AC\uC6A9\uD574\uC57C \uBAA8\uB4E0 \uC131\uB2A5\uC744 \uBC1C\uD718\uD569\uB2C8\uB2E4.",
          L"\uC544\uC774\uD15C \uC0AC\uC6A9\n\uC218\uB7C9\uC744 \uC120\uD0DD\uD558\uC138\uC694" },
        // TCHINESE: host 37 characters, ours 10
        { L"\u76EE\u524D\u7684\u529B\u6C23\u4E0D\u8DB3\u4EE5\u55AE\u624B\u4F7F\u7528\u6240\u9078\u64C7\u7684\u6B66\u5668\uFF0C\n\u9808\u96D9\u624B\u62FF\u624D\u80FD\u5145\u5206\u767C\u63EE\u6B64\u6B66\u5668\u7684\u6027\u80FD\u3002",
          L"\u4F7F\u7528\u9053\u5177\n\u8ACB\u9078\u64C7\u6578\u91CF" },
    };
    const uint32_t kPickerTextCount = sizeof(kPickerText) / sizeof(kPickerText[0]);

    // The message the picker asks the game to show. 12030 is the line borrowed above. If its text could not be found
    // in memory, 11000 is used instead: the game's own "Drop item. / Specify quantity." Its second line fits, the
    // first one does not, but it is readable in every language and is only a fallback.
    const uint32_t kPickerMessageId = 12030;
    const uint32_t kPickerMessageFallbackId = 11000;
    const uint32_t kPickerLeftButton = 1;  // OK
    const uint32_t kPickerRightButton = 2; // Cancel
}
// =====================================================================================================================

namespace {
    // ---------------------------------------------------------------------------------------------------------------
    // Addresses in DARKSOULS.exe (Steam build, PE timestamp 0x557F2FD0). Research credit: Dark Souls Overhaul team.
    // ---------------------------------------------------------------------------------------------------------------
    // The menu object. [kMenuManager] is a pointer to it.
    //   +0x024 not zero while the inventory menu is open
    //   +0x060 which dialog to show (3 = the number picker)
    //   +0x1B4 the button that was pressed
    //   +0x204 / +0x208 / +0x20C the picker's largest / smallest / starting value
    const DWORD kMenuManager = 0x013786D0;
    const DWORD kMenuDialogKind = 0x060;
    const DWORD kMenuPickerMax = 0x204;
    const DWORD kMenuPickerMin = 0x208;
    const DWORD kMenuPickerStart = 0x20C;
    // The running total behind the floating "+N souls" text. At 0xE4737B the game does
    //   mov eax,[0x13786D0] / add dword ptr [eax+0x1F8], ebp
    // with ebp holding the reward for ONE item. That line runs just after the soul count is written, so the extra
    // items' worth is added here first and the game's own addition lands on top of it.
    const DWORD kMenuSoulPopup = 0x1F8;
    const DWORD kDialogKindNumberPicker = 3;

    // The three words of the dialog: which text, and the two button labels.
    const DWORD kDialogMessageId = 0x012E33E0;
    const DWORD kDialogLeftButtonId = 0x012E33E4;
    const DWORD kDialogRightButtonId = 0x012E33E8;

    // Where the number you picked is stored.
    const DWORD kPickerResult = 0x012E3400;

    // The player. [[kPlayerPointer]] is the player object.
    //   +0x1C4 the "use the item now" flag
    //   +0x1D8 / +0x628 the id of the item being used
    const DWORD kPlayerPointer = 0x012E29E8;
    const DWORD kPlayerUseFlag = 0x1C4;
    const DWORD kPlayerItemIdA = 0x1D8;
    const DWORD kPlayerItemIdB = 0x628;

    // The player's numbers: [[kGameDataMan] + 8] is the block, with the soul count and the humanity count in it.
    const DWORD kGameDataMan = 0x01378700;
    const DWORD kStatsSouls = 0x8C;
    const DWORD kStatsHumanity = 0x7C;

    // ---------------------------------------------------------------------------------------------------------------
    // The eight places that are hooked. Every one is checked against these bytes before anything is written; if even
    // one does not match, nothing at all is patched and the feature stays off.
    // ---------------------------------------------------------------------------------------------------------------
    // 'group' says which setting puts this hook in: 0 = always (the picker itself), 1 = standardSoulsBulk,
    // 2 = humanityBulk. A hook whose group is switched off is never written into the game at all.
    struct HookSite {
        const char* name;
        DWORD address;
        DWORD length;             // bytes the jump replaces (5 for the jump, the rest filled with NOP)
        const BYTE* expected;
        int group;
    };
    const int kGroupAlways = 0, kGroupSouls = 1, kGroupHumanity = 2;

    // 1. The game is about to use an item.      cmp byte ptr [eax+0xC7],0 / jne / mov edx,[esp+0x14]
    const BYTE kExpectUse[13] = { 0x80,0xB8,0xC7,0x00,0x00,0x00,0x00, 0x75,0x0A, 0x8B,0x54,0x24,0x14 };
    // 2. The inventory count changes.           add edx,esi / cmp edx,eax / jle
    const BYTE kExpectDec[6] = { 0x03,0xD6, 0x3B,0xD0, 0x7E,0x0C };
    // 3. A dialog button was pressed.           mov [ecx+0x1B4],eax
    const BYTE kExpectDlgResult[6] = { 0x89,0x81,0xB4,0x01,0x00,0x00 };
    // 4. A dialog closed without a button.      call 0xC7F7F0
    const BYTE kExpectDlgCancel[5] = { 0xE8,0xA4,0xF9,0xFF,0xFF };
    // 5. The "use the item now" flag is set.    or byte ptr [ebx+0x1C4],cl
    const BYTE kExpectForce[6] = { 0x08,0x8B,0xC4,0x01,0x00,0x00 };
    // 6. The soul count is written.             mov [esi+0x8C],eax
    const BYTE kExpectSouls[6] = { 0x89,0x86,0x8C,0x00,0x00,0x00 };
    // 7. The highlighted inventory item read.   mov eax,[eax+4] / pop esi / ret
    const BYTE kExpectInvQty[5] = { 0x8B,0x40,0x04, 0x5E, 0xC3 };
    // 8. The "can't use item" animation is set.  mov [esi+0x60],ecx / mov edx,[eax+4]
    const BYTE kExpectWhiff[6] = { 0x89,0x4E,0x60, 0x8B,0x50,0x04 };
    // 9. The humanity count is written.       mov [esi+0x7C],eax / pop esi / pop ebx
    const BYTE kExpectHumanity[5] = { 0x89,0x46,0x7C, 0x5E, 0x5B };

    // Addresses the stubs jump back to. Held in variables because inline assembly cannot jump to a number directly.
    DWORD g_useReturn = 0x00E53E54;      // after the replaced instructions
    DWORD g_useOtherBranch = 0x00E53E5A; // where the original jne went
    DWORD g_decReturn = 0x00C09759;      // after the replaced instructions
    DWORD g_decOtherBranch = 0x00C09765; // where the original jle went
    DWORD g_dlgResultReturn = 0x00C7F8EC;
    DWORD g_dlgCancelCall = 0x00C7F7F0;  // the call that was replaced
    DWORD g_dlgCancelReturn = 0x00C7FE4C;
    DWORD g_forceReturn = 0x00E1DA0B;
    DWORD g_soulsReturn = 0x00BFC4E8;
    DWORD g_whiffReturn = 0x00E2B2D5;
    DWORD g_humanityReturn = 0x00E67A2B; // after the write and the two pops the stub repeats

    // The animation the game plays when an item cannot be used (the shoulder shrug). While the number picker is open
    // the item use has been held back on purpose, so the game thinks the item failed and wants to play this. It is
    // suppressed once, for that one moment only.
    const DWORD kCannotUseAnimation = 0x1D56; // 7510

    bool g_installed = false;

    // ---------------------------------------------------------------------------------------------------------------
    // What the feature is doing right now. Written and read by the game's own thread inside the hooks.
    // ---------------------------------------------------------------------------------------------------------------
    volatile DWORD g_pendingItem = 0;   // the soul item the picker is open for (0 = nothing going on)
    volatile DWORD g_waitingResult = 0; // the next dialog answer belongs to us
    volatile DWORD g_useAgain = 0;      // the next use is the one we asked for, do not show the picker again
    volatile DWORD g_forceUse = 0;      // make the game use the item now
    volatile DWORD g_countForDecrement = 0; // how many to take off the stack (used once, then cleared)
    volatile DWORD g_countForReward = 0;    // how many to multiply the reward by, souls or humanity (used once)
    volatile DWORD g_heldQuantity = 0;      // how many of the highlighted item the player has

    volatile DWORD g_forcedUseActive = 0;   // a use that WE asked for is running right now
    volatile DWORD g_skipWhiff = 0;         // swallow the next "cannot use item" animation
    bool g_soulsOn = false;                 // standardSoulsBulk, read once when the feature installs
    bool g_humanityOn = false;              // humanityBulk, read once when the feature installs

    // Copies of the borrowed message line found in memory. The game can hold more than one, so every copy found is
    // replaced and restored together. 'original' keeps the game's own characters so they can be written back exactly.
    struct TextSlot { wchar_t* at; const wchar_t* ours; std::vector<wchar_t> original; };
    std::vector<TextSlot> g_textSlots;      // filled once by the search thread, then only read
    volatile bool g_textReady = false;      // true once the search has found at least one copy
    volatile bool g_textApplied = false;    // our words are in place right now

    // How we tell an inventory use from a quick-item press. Only the inventory menu's own read of the stack size
    // records anything here, and the reading is thrown away as soon as it is used or when it gets old, so a
    // quick-item press during play finds nothing and is left alone. The menu's "inventory is open" flag cannot be
    // used for this: by the time the game actually uses the item the menu has already closed, so it always reads 0
    // at that point (confirmed in the game).
    volatile DWORD g_lastSlotItem = 0;      // the item the reading belonged to
    volatile DWORD g_lastSlotTime = 0;      // when it was taken (GetTickCount)
    const DWORD kReadingValidMs = 1000;     // older than this and the reading is stale

    // ---------------------------------------------------------------------------------------------------------------
    // Small helpers
    // ---------------------------------------------------------------------------------------------------------------
    // The row for an item, or nothing at all when that group is switched off.
    const BulkItem* findBulkItem(uint32_t itemId) {
        for (uint32_t i = 0; i < kBulkItemCount; ++i) {
            if (kBulkItems[i].id != itemId) continue;
            if (kBulkItems[i].souls && !g_soulsOn) return NULL;
            if (kBulkItems[i].humanity && !g_humanityOn) return NULL;
            return &kBulkItems[i];
        }
        return NULL;
    }

    // Reads a pointer, giving 0 when the address cannot be read.
    DWORD readPointer(DWORD address) {
        DWORD value = 0;
        __try { value = *(const DWORD*)address; }
        __except (EXCEPTION_EXECUTE_HANDLER) { value = 0; }
        return value;
    }

    DWORD menuObject() { return readPointer(kMenuManager); }

    // The block holding the player's numbers, or 0 when it cannot be read.
    DWORD playerStats() {
        const DWORD top = readPointer(kGameDataMan);
        if (!top) return 0;
        return readPointer(top + 8);
    }

    // The player's current soul count, or 0 when it cannot be read.
    DWORD currentSouls() {
        const DWORD stats = playerStats();
        return stats ? readPointer(stats + kStatsSouls) : 0;
    }

    // The player's current humanity count (the number in the top-left corner), or 0 when it cannot be read.
    DWORD currentHumanity() {
        const DWORD stats = playerStats();
        return stats ? readPointer(stats + kStatsHumanity) : 0;
    }

    void restorePickerText(); // defined below, with the rest of the borrowed-text handling

    void forgetPending() {
        restorePickerText();
        g_forcedUseActive = 0;
        g_skipWhiff = 0;
        g_pendingItem = 0;
        g_waitingResult = 0;
        g_useAgain = 0;
        g_forceUse = 0;
        g_countForDecrement = 0;
        g_countForReward = 0;
    }

    // Tells the game which item is being used.
    void setPlayerItemId(DWORD itemId) {
        const DWORD top = readPointer(kPlayerPointer);
        if (!top) return;
        const DWORD player = readPointer(top);
        if (!player) return;
        __try {
            *(DWORD*)(player + kPlayerItemIdA) = itemId;
            *(DWORD*)(player + kPlayerItemIdB) = itemId;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }

    // The largest number the picker may offer: never more than the player has, and never so many that souls would be
    // thrown away at the 999,999,999 limit. Always at least 1.
    DWORD pickerMaximum(const BulkItem* item, DWORD held) {
        if (!item) return 1;
        DWORD have, limit, each;
        if (item->souls) { have = currentSouls(); limit = kMaxSouls; each = item->souls; }
        else if (item->humanity) { have = currentHumanity(); limit = kMaxHumanity; each = item->humanity; }
        else return 1;
        if (have >= limit) return 1;         // already at the limit: behave exactly as before
        const DWORD fits = (limit - have) / each;
        DWORD maximum = held;
        if (fits < maximum) maximum = fits;
        if (maximum < 1) maximum = 1;
        return maximum;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Finding and swapping the borrowed message line
    // ---------------------------------------------------------------------------------------------------------------
    bool textReadableProtect(DWORD p) {
        if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
            PAGE_EXECUTE_WRITECOPY)) != 0;
    }

    // Scans one block of memory for 'text' and writes the address of every copy into 'out'. memchr does the hunting:
    // it is the fastest way to skip over memory that cannot match, which matters because this looks through the
    // whole of the game's memory. The whole block is inside one __try, so a block that is released while being read
    // costs us that block and nothing more. No C++ objects live in here, which is what __try requires.
    int scanRegion(const BYTE* start, size_t size, const wchar_t* text, wchar_t** out, int maxOut) {
        int n = 0;
        __try {
            const size_t chars = wcslen(text);
            const size_t bytes = chars * sizeof(wchar_t);
            if (size < bytes || maxOut <= 0) return 0;
            const int firstByte = (int)(text[0] & 0xFF);
            const BYTE* p = start;
            const BYTE* end = start + size - bytes;
            while (p <= end && n < maxOut) {
                const BYTE* hit = (const BYTE*)memchr(p, firstByte, (size_t)(end - p) + 1);
                if (!hit) break;
                // text is 16 bit, so a copy can only start on an even address
                if ((((UINT_PTR)hit) & 1) == 0 && memcmp(hit, text, bytes) == 0) {
                    out[n++] = (wchar_t*)hit;
                    p = hit + bytes;
                }
                else {
                    p = hit + 1;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // the block went away while being read: whatever was found before it stays good
        }
        return n;
    }

    // Looks through the game's memory for the borrowed line, in any of the ten languages. Called from the background
    // thread below, never from the game's own threads.
    void searchPickerText() {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const BYTE* p = (const BYTE*)si.lpMinimumApplicationAddress;
        const BYTE* maxAddr = (const BYTE*)si.lpMaximumApplicationAddress;
        const int kMaxCopies = 16;
        wchar_t* hits[kMaxCopies];
        int found = 0;
        int language = -1;
        while (p < maxAddr && found < kMaxCopies) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
            const BYTE* regionStart = (const BYTE*)mbi.BaseAddress;
            const BYTE* regionEnd = regionStart + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Type != MEM_IMAGE && textReadableProtect(mbi.Protect)) {
                if (language < 0) {
                    // not known yet: try every language until one of them is seen
                    for (uint32_t i = 0; i < kPickerTextCount && found < kMaxCopies; ++i) {
                        const int n = scanRegion(regionStart, mbi.RegionSize, kPickerText[i].gameText,
                            hits + found, kMaxCopies - found);
                        if (n > 0) { language = (int)i; found += n; break; }
                    }
                }
                else {
                    // the language is known: only that one line is looked for from here on
                    found += scanRegion(regionStart, mbi.RegionSize, kPickerText[language].gameText,
                        hits + found, kMaxCopies - found);
                }
            }
            if (regionEnd <= p) break;
            p = regionEnd;
        }
        if (language < 0 || found == 0) return; // nothing yet: the thread tries again in a moment

        const wchar_t* game = kPickerText[language].gameText;
        const size_t chars = wcslen(game);
        for (int i = 0; i < found; ++i) {
            TextSlot slot;
            slot.at = hits[i];
            slot.ours = kPickerText[language].ourText;
            slot.original.assign(game, game + chars + 1); // with the closing zero
            g_textSlots.push_back(slot);
        }
        g_textReady = true; // published last: the slots are complete before anything may use them
        SDLOG(0, "bulkItemUse: picker text ready (language %d, %d copies)\n", language, found);
    }

    DWORD WINAPI pickerTextThread(LPVOID) {
        // The game loads its text a few seconds into start-up, so this tries for a while before giving up.
        for (int attempt = 0; attempt < 30 && !g_textReady; ++attempt) {
            Sleep(2000);
            searchPickerText();
        }
        if (!g_textReady) {
            SDLOG(0, "bulkItemUse: picker text not found, the picker uses the game's own 'Drop item.' line\n");
        }
        return 0;
    }

    // Writes 'text' over the line at 'at'. 'guard' is how many bytes of that line the game owns, so the page is made
    // writable for exactly that much. No C++ objects live here, which is what __try requires.
    void writeText(wchar_t* at, const wchar_t* text, size_t textBytes, size_t guardBytes) {
        DWORD oldProtect;
        if (!VirtualProtect(at, guardBytes, PAGE_READWRITE, &oldProtect)) return;
        __try { memcpy(at, text, textBytes); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
        VirtualProtect(at, guardBytes, oldProtect, &oldProtect);
    }

    // Writes our words over the borrowed line. Called just before the picker is shown.
    void applyPickerText() {
        if (!g_textReady || g_textApplied) return;
        for (size_t i = 0; i < g_textSlots.size(); ++i) {
            const size_t guard = g_textSlots[i].original.size() * sizeof(wchar_t);
            const size_t n = (wcslen(g_textSlots[i].ours) + 1) * sizeof(wchar_t); // with the closing zero
            writeText(g_textSlots[i].at, g_textSlots[i].ours, n, guard);
        }
        g_textApplied = true;
    }

    // Puts the game's own words back. Called the moment the dialog is finished with, whatever the outcome.
    void restorePickerText() {
        if (!g_textApplied) return;
        g_textApplied = false;
        for (size_t i = 0; i < g_textSlots.size(); ++i) {
            const size_t guard = g_textSlots[i].original.size() * sizeof(wchar_t);
            writeText(g_textSlots[i].at, &g_textSlots[i].original[0], guard, guard);
        }
    }

    // Opens the game's number picker.
    void openPicker(DWORD maximum) {
        const DWORD menu = menuObject();
        if (!menu) return;
        applyPickerText(); // our words go in just before the dialog is built, and come out again when it closes
        __try {
            *(DWORD*)kDialogMessageId = g_textReady ? kPickerMessageId : kPickerMessageFallbackId;
            *(DWORD*)kDialogLeftButtonId = kPickerLeftButton;
            *(DWORD*)kDialogRightButtonId = kPickerRightButton;
            *(DWORD*)(menu + kMenuPickerMax) = maximum;
            *(DWORD*)(menu + kMenuPickerMin) = 1;
            *(DWORD*)(menu + kMenuPickerStart) = 1;
            *(DWORD*)(menu + kMenuDialogKind) = kDialogKindNumberPicker;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
}

// =====================================================================================================================
// The parts the hooks call. These run on the game's own thread, so they stay short and do no allocation.
// =====================================================================================================================

// The game is about to use the item with this id. Returns the id the game should actually use:
// the same id for normal behaviour, or 0 to cancel this use while the picker is open.
DWORD _stdcall bulkOnItemUse(DWORD itemId) {
    if (!g_installed) return itemId;


    // This is the second pass, the one we asked for. Put our item back in and let it run.
    if (g_useAgain) {
        g_useAgain = 0;
        g_forcedUseActive = 1;   // from here until the use finishes, the corrections may apply
        if (g_pendingItem) return g_pendingItem;
        return itemId;
    }

    // Any other use means whatever we were doing before is over. If something is still left over from an earlier
    // attempt (the forced use never completed, for instance), it is thrown away HERE, before this use runs. Without
    // this, a left-over count would be applied to an ordinary use and take the wrong number of items.
    if (g_pendingItem || g_countForDecrement || g_countForReward || g_forcedUseActive) {
        SDLOG(1, "bulkItemUse: dropping left-over state (item %u, count %u) before a normal use\n",
            g_pendingItem, g_countForDecrement);
        forgetPending();
    }

    const BulkItem* item = findBulkItem(itemId);
    if (!item) return itemId;              // not a listed item, or its group is switched off

    // Where did this use come from? Only the pause Inventory menu gets the picker. The marker is cleared below so
    // that every use has to be identified fresh; a stale marker can never make the quick-item button open a picker.
    // Only the inventory read sets this, and it is cleared after every use, so a quick-item press during play finds
    // nothing remembered and is left alone. "Which of the two reads ran last" is NOT used: the quick-slot read runs
    // every frame while the HUD is drawn, so it would race with the inventory read and the picker would appear only
    // sometimes (seen in testing).
    const DWORD knownItem = g_lastSlotItem;
    const DWORD held = g_heldQuantity;
    const DWORD age = GetTickCount() - g_lastSlotTime;
    g_lastSlotItem = 0;
    g_heldQuantity = 0;

    if (knownItem != itemId) return itemId;      // no fresh inventory reading for this item: not an inventory use
    if (age > kReadingValidMs) return itemId;    // the reading is old: this use is not the one it belonged to
    if (held < 2) return itemId;           // only one owned: no dialog, use it straight away

    const DWORD maximum = pickerMaximum(item, held);
    if (maximum < 2) return itemId;        // at the soul limit: behave as before

    g_pendingItem = itemId;
    g_countForDecrement = 0;
    g_countForReward = 0;
    g_waitingResult = 1;
    g_skipWhiff = 1;   // the use we are about to cancel must not make the character shrug
    openPicker(maximum);
    SDLOG(1, "bulkItemUse: picker opened for item %u (has %u, max %u)\n", itemId, held, maximum);
    return 0;                              // cancel this single use; it happens again after the picker
}

// A dialog button was pressed. 1 means OK.
void _stdcall bulkOnDialogResult(DWORD button) {
    if (!g_installed || !g_waitingResult) return;
    g_waitingResult = 0;

    if (button != 1) { // cancelled
        SDLOG(1, "bulkItemUse: picker cancelled\n");
        forgetPending();
        return;
    }
    DWORD chosen = readPointer(kPickerResult);
    if (chosen == 0xFFFFFFFF || chosen == 0) {
        forgetPending();
        return;
    }
    restorePickerText(); // the dialog is done with: the game's own line goes back before anything else happens
    g_countForDecrement = chosen;
    g_countForReward = chosen;
    g_useAgain = 1;
    g_forceUse = 1;
    setPlayerItemId(g_pendingItem);
    SDLOG(1, "bulkItemUse: using %u of item %u\n", chosen, g_pendingItem);
}

// A dialog closed without a button being pressed.
void _stdcall bulkOnDialogCancel(void) {
    if (!g_installed) return;
    if (g_pendingItem) SDLOG(1, "bulkItemUse: dialog closed, nothing used\n");
    forgetPending();
}

// Should the game be told to use the item right now? Returns 1 for yes.
DWORD _stdcall bulkForceUse(void) {
    if (!g_installed || !g_forceUse) return 0;
    g_forceUse = 0;
    setPlayerItemId(g_pendingItem);
    return 1;
}

// The inventory count is about to change. 'count' is what is in the slot, 'delta' how much is being added
// (negative when something is used). Returns the new count the game should store.
DWORD _stdcall bulkAdjustDecrement(DWORD slot, DWORD count, DWORD delta) {
    const DWORD normal = count + delta; // what the original instruction would have produced
    if (!g_installed) return normal;
    const DWORD wanted = g_countForDecrement;
    if (wanted < 2) return normal;
    if (!g_forcedUseActive) return normal;  // not the use we asked for: never touch it
    if ((int)delta >= 0) return normal;     // the count is going up, not a use
    if (!slot) return normal;
    if (readPointer(slot + 4) != g_pendingItem) return normal; // a different item
    g_countForDecrement = 0;                // one correction only
    SDLOG(1, "bulkItemUse: stack %u -> %u\n", count, count > wanted ? count - wanted : 0);
    if (count <= wanted) return 0;
    return count - wanted;
}

// The soul count is about to be written. 'newSouls' is what the game worked out for one item, 'oldSouls' what the
// player had before. Returns the number that should really be stored.
DWORD _stdcall bulkAdjustSouls(DWORD newSouls, DWORD oldSouls) {
    if (!g_installed) return newSouls;
    const DWORD wanted = g_countForReward;
    if (wanted < 2) return newSouls;
    if (!g_forcedUseActive) return newSouls; // not the use we asked for: never touch it

    const BulkItem* item = findBulkItem(g_pendingItem);
    if (!item || !item->souls) return newSouls;

    // Safety check: this must be the award for our item and no other. If the game just gave a different number of
    // souls (an enemy died, the amount was cut short at the limit), nothing is changed.
    if (newSouls <= oldSouls) return newSouls;
    const DWORD reward = newSouls - oldSouls;
    if (reward != item->souls) return newSouls;

    g_countForReward = 0;                    // one correction only

    unsigned __int64 total = (unsigned __int64)oldSouls + (unsigned __int64)reward * (unsigned __int64)wanted;
    if (total > (unsigned __int64)kMaxSouls) total = kMaxSouls;
    // Correct the floating "+N souls" text. The game is about to add one item's reward to its running total, so only
    // the extra items are added here.
    const DWORD granted = (DWORD)(total - (unsigned __int64)oldSouls);
    if (granted > reward) {
        const DWORD menu = menuObject();
        if (menu) {
            __try { *(DWORD*)(menu + kMenuSoulPopup) += (granted - reward); }
            __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
    }
    SDLOG(1, "bulkItemUse: souls %u -> %u\n", newSouls, (DWORD)total);
    return (DWORD)total;
}

// The humanity count is about to be written. 'newValue' is what the game worked out after granting one item's worth
// (already clamped to 99 by the game), 'oldValue' what the counter held a moment ago. Returns what should be stored.
//
// This write is shared: the game uses it when humanity is lost on death, when reverse hollowing is paid for at a
// bonfire, and for covenant tributes. The three tests below are what keep those untouched - a use we started, only
// once, and only when the change the game just made is exactly what this item gives. Anything else passes straight
// through. (The Overhaul mod's own version of this hook broke reverse hollowing; that is the trap being avoided.)
DWORD _stdcall bulkAdjustHumanity(DWORD newValue, DWORD oldValue) {
    if (!g_installed) return newValue;
    const DWORD wanted = g_countForReward;
    if (wanted < 2) return newValue;
    if (!g_forcedUseActive) return newValue; // not the use we asked for: never touch it

    const BulkItem* item = findBulkItem(g_pendingItem);
    if (!item || !item->humanity) return newValue;

    // It must be a gain, and exactly the gain this item gives. Losing humanity, or any other amount, is left alone.
    if (newValue <= oldValue) return newValue;
    const DWORD gained = newValue - oldValue;
    if (gained != item->humanity) return newValue;

    g_countForReward = 0;                    // one correction only

    unsigned __int64 total = (unsigned __int64)oldValue + (unsigned __int64)gained * (unsigned __int64)wanted;
    if (total > (unsigned __int64)kMaxHumanity) total = kMaxHumanity;
    SDLOG(1, "bulkItemUse: humanity %u -> %u\n", newValue, (DWORD)total);
    return (DWORD)total;
}

// Should this animation be swallowed? Only the "cannot use item" one, only once, only right after we held a use
// back to show the picker.
DWORD _stdcall bulkSkipWhiff(DWORD animationId) {
    if (!g_installed || !g_skipWhiff) return 0;
    if (animationId != kCannotUseAnimation) return 0;
    g_skipWhiff = 0;
    return 1;
}

// Remembers how many of the highlighted inventory item the player has.
void _stdcall bulkNoteInventoryQuantity(DWORD slot) {
    if (!g_installed || !slot) return;
    g_heldQuantity = readPointer(slot + 8);
    g_lastSlotItem = readPointer(slot + 4);
    g_lastSlotTime = GetTickCount();
}


// =====================================================================================================================
// The stubs the game jumps into. Each one saves every register, calls the matching function above, puts the registers
// back, performs the instructions that the jump replaced, and continues where the game left off.
// =====================================================================================================================
namespace {
    // After PUSHAD + PUSHFD the stack holds 36 (0x24) bytes, so a value that was at [esp+N] is now at [esp+0x24+N],
    // and the registers PUSHAD saved sit at: edi+0x04 esi+0x08 ebp+0x0C esp+0x10 ebx+0x14 edx+0x18 ecx+0x1C eax+0x20.

    __declspec(naked) void use_stub() {
        __asm {
            pushad
            pushfd
            push dword ptr [esp + 0x38]   // the item id, originally at [esp+0x14]
            call bulkOnItemUse
            mov dword ptr [esp + 0x38], eax
            popfd
            popad
            // the instructions the jump replaced
            cmp byte ptr [eax + 0x000000C7], 0
            jne other
            mov edx, dword ptr [esp + 0x14]
            jmp dword ptr [g_useReturn]
        other:
            jmp dword ptr [g_useOtherBranch]
        }
    }

    __declspec(naked) void dec_stub() {
        __asm {
            pushad
            pushfd
            push esi                      // how much is being added (negative when used)
            push edx                      // what is in the slot now
            push ecx                      // the inventory slot
            call bulkAdjustDecrement
            mov dword ptr [esp + 0x18], eax // becomes edx again at POPAD
            popfd
            popad
            // the instructions the jump replaced, with edx already corrected
            cmp edx, eax
            jle other
            jmp dword ptr [g_decReturn]
        other:
            jmp dword ptr [g_decOtherBranch]
        }
    }

    __declspec(naked) void dlg_result_stub() {
        __asm {
            mov dword ptr [ecx + 0x000001B4], eax // the instruction the jump replaced
            pushad
            pushfd
            push eax                      // which button
            call bulkOnDialogResult
            popfd
            popad
            jmp dword ptr [g_dlgResultReturn]
        }
    }

    __declspec(naked) void dlg_cancel_stub() {
        __asm {
            pushad
            pushfd
            call bulkOnDialogCancel
            popfd
            popad
            call dword ptr [g_dlgCancelCall]        // the call the jump replaced
            jmp dword ptr [g_dlgCancelReturn]
        }
    }

    __declspec(naked) void force_stub() {
        __asm {
            pushad
            pushfd
            call bulkForceUse
            test eax, eax
            jz leave_alone
            mov byte ptr [esp + 0x1C], 1  // becomes cl = 1 at POPAD, which is what "use it now" means
        leave_alone:
            popfd
            popad
            or byte ptr [ebx + 0x000001C4], cl // the instruction the jump replaced
            jmp dword ptr [g_forceReturn]
        }
    }

    __declspec(naked) void souls_stub() {
        __asm {
            pushad
            pushfd
            push edi                      // the soul count before
            push eax                      // the soul count the game worked out
            call bulkAdjustSouls
            mov dword ptr [esp + 0x20], eax // becomes eax again at POPAD
            popfd
            popad
            mov dword ptr [esi + 0x0000008C], eax // the instruction the jump replaced
            jmp dword ptr [g_soulsReturn]
        }
    }

    __declspec(naked) void inv_qty_stub() {
        __asm {
            pushad
            pushfd
            push eax                      // the inventory slot
            call bulkNoteInventoryQuantity
            popfd
            popad
            // the instructions the jump replaced; this one ends the game's function itself
            mov eax, dword ptr [eax + 4]
            pop esi
            ret
        }
    }


    __declspec(naked) void whiff_stub() {
        __asm {
            pushad
            pushfd
            push ecx                      // the animation the game wants to play
            call bulkSkipWhiff
            test eax, eax
            jnz swallow
            popfd
            popad
            mov dword ptr [esi + 0x60], ecx // the instruction the jump replaced
            jmp tail
        swallow:
            popfd
            popad                         // the animation is simply not stored
        tail:
            mov edx, dword ptr [eax + 4]  // the second instruction the jump replaced
            jmp dword ptr [g_whiffReturn]
        }
    }

    __declspec(naked) void humanity_stub() {
        __asm {
            pushad
            pushfd
            push dword ptr [esi + 0x7C]   // what the counter held a moment ago (the write has not happened yet)
            push eax                      // what the game worked out, already clamped to 99 by its own code
            call bulkAdjustHumanity
            mov dword ptr [esp + 0x20], eax // becomes eax again at POPAD
            popfd
            popad
            // the three instructions the jump replaced
            mov dword ptr [esi + 0x0000007C], eax
            pop esi
            pop ebx
            jmp dword ptr [g_humanityReturn]
        }
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Installing
    // -----------------------------------------------------------------------------------------------------------------
    const HookSite kSites[9] = {
        { "item use",            0x00E53E47, 13, kExpectUse,       kGroupAlways   },
        { "inventory count",     0x00C09753,  6, kExpectDec,       kGroupAlways   },
        { "dialog answer",       0x00C7F8E6,  6, kExpectDlgResult, kGroupAlways   },
        { "dialog closed",       0x00C7FE47,  5, kExpectDlgCancel, kGroupAlways   },
        { "use item now",        0x00E1DA05,  6, kExpectForce,     kGroupAlways   },
        { "inventory quantity",  0x00C0AA80,  5, kExpectInvQty,    kGroupAlways   },
        { "cannot-use animation",0x00E2B2CF,  6, kExpectWhiff,     kGroupAlways   },
        { "soul count",          0x00BFC4E2,  6, kExpectSouls,     kGroupSouls    },
        { "humanity count",      0x00E67A26,  5, kExpectHumanity,  kGroupHumanity },
    };

    const void* kStubs[9] = {
        (const void*)&use_stub,
        (const void*)&dec_stub,
        (const void*)&dlg_result_stub,
        (const void*)&dlg_cancel_stub,
        (const void*)&force_stub,
        (const void*)&inv_qty_stub,
        (const void*)&whiff_stub,
        (const void*)&souls_stub,
        (const void*)&humanity_stub,
    };

    // Writes a jump to 'target' over 'length' bytes at 'address', filling the rest with NOP.
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

bool installBulkItemUse() {
    if (g_installed) return true;
    if (!Settings::get().getBulkItemUse()) return false;

    // The two groups are read once, here, so the hooks never have to look at a setting while the game is running.
    g_soulsOn = Settings::get().getStandardSoulsBulk();
    g_humanityOn = Settings::get().getHumanityBulk();
    if (!g_soulsOn && !g_humanityOn) {
        SDLOG(0, "bulkItemUse: on, but both standardSoulsBulk and humanityBulk are off, so nothing is patched\n");
        return false;
    }

    // Which hooks this run needs. A group that is switched off is not checked and not written.
    bool wanted[9];
    int count = 0;
    for (int i = 0; i < 9; ++i) {
        wanted[i] = (kSites[i].group == kGroupAlways) ||
                    (kSites[i].group == kGroupSouls && g_soulsOn) ||
                    (kSites[i].group == kGroupHumanity && g_humanityOn);
        if (wanted[i]) ++count;
    }

    // Check them all first. One wrong byte means a different game build, and then nothing is patched at all.
    for (int i = 0; i < 9; ++i) {
        if (!wanted[i]) continue;
        if (memcmp((const void*)kSites[i].address, kSites[i].expected, kSites[i].length) != 0) {
            SDLOG(0, "bulkItemUse: the code for '%s' at 0x%08X is not what this build should have, "
                     "bulkItemUse stays off\n", kSites[i].name, kSites[i].address);
            return false;
        }
    }

    for (int i = 0; i < 9; ++i) {
        if (!wanted[i]) continue;
        if (!writeJump(kSites[i].address, kSites[i].length, kStubs[i])) {
            SDLOG(0, "bulkItemUse: could not write at 0x%08X, bulkItemUse stays off\n", kSites[i].address);
            return false; // earlier hooks stay in place but do nothing, because g_installed is still false
        }
    }

    g_installed = true;
    // Look for the borrowed message line in the background: the game loads its text a few seconds after start-up.
    // This only ever runs when bulkItemUse is on, because the setting is checked at the top of installBulkItemUse.
    {
        HANDLE h = CreateThread(NULL, 0, pickerTextThread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    SDLOG(0, "bulkItemUse: on (soul items %s, humanity %s, %d hooks, inventory menu only)\n",
        g_soulsOn ? "yes" : "no", g_humanityOn ? "yes" : "no", count);
    return true;
}

bool bulkItemUseActive() {
    return g_installed;
}
