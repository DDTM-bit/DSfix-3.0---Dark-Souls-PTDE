#include "BulkItemUse.h"
#include "Settings.h" // SDLOG reads the log level when RELEASE_VER is not defined
#include "main.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

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
// "reward" is how many souls one item gives. It is used for two things: to work out the largest number the picker may
// offer (so souls can never be wasted), and as a safety check - if the game awards a different amount than the one
// here, the correction is skipped and the item behaves normally.
// =====================================================================================================================
namespace {
    struct SoulItem { uint32_t id; uint32_t reward; };
    const SoulItem kSoulItems[] = {
        { 400,   200 }, // Soul of a Lost Undead
        { 401,   400 }, // Large Soul of a Lost Undead
        { 402,   800 }, // Soul of a Nameless Soldier
        { 403,  1000 }, // Large Soul of a Nameless Soldier
        { 404,  2000 }, // Soul of a Proud Knight
        { 405,  3000 }, // Large Soul of a Proud Knight
        { 406,  5000 }, // Soul of a Brave Warrior
        { 407,  8000 }, // Large Soul of a Brave Warrior
        { 408, 10000 }, // Soul of a Hero
        { 409, 20000 }, // Soul of a Great Hero
        // Humanity (500) and Twin Humanities (501) are deliberately NOT here. Humanity has its own counter with its
        // own limit of 99 and needs a second correction that this file does not do. Adding the two lines here without
        // that correction would lose humanity points. Leave them out until that is built and tested.
    };
    const uint32_t kSoulItemCount = sizeof(kSoulItems) / sizeof(kSoulItems[0]);

    // The largest number of souls the game can hold. The picker never offers more than fits under this.
    const uint32_t kMaxSouls = 999999999;

    // Text shown in the number picker. The game has no "how many souls will you use?" line, so a message that already
    // exists is borrowed. This only changes the words in the dialog, nothing else. If the words look wrong in the game,
    // tell me what they say and this number gets changed.
    const uint32_t kPickerMessageId = 20020;
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

    // The player's numbers. [[kGameDataMan] + 8] + 0x8C is the soul count.
    const DWORD kGameDataMan = 0x01378700;

    // ---------------------------------------------------------------------------------------------------------------
    // The eight places that are hooked. Every one is checked against these bytes before anything is written; if even
    // one does not match, nothing at all is patched and the feature stays off.
    // ---------------------------------------------------------------------------------------------------------------
    struct HookSite {
        const char* name;
        DWORD address;
        DWORD length;             // bytes the jump replaces (5 for the jump, the rest filled with NOP)
        const BYTE* expected;
    };

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
    volatile DWORD g_countForSouls = 0;     // how many to multiply the souls by (used once, then cleared)
    volatile DWORD g_heldQuantity = 0;      // how many of the highlighted item the player has

    volatile DWORD g_forcedUseActive = 0;   // a use that WE asked for is running right now
    volatile DWORD g_skipWhiff = 0;         // swallow the next "cannot use item" animation

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
    const SoulItem* findSoulItem(uint32_t itemId) {
        for (uint32_t i = 0; i < kSoulItemCount; ++i) {
            if (kSoulItems[i].id == itemId) return &kSoulItems[i];
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

    // The player's current soul count, or 0 when it cannot be read.
    DWORD currentSouls() {
        const DWORD top = readPointer(kGameDataMan);
        if (!top) return 0;
        const DWORD stats = readPointer(top + 8);
        if (!stats) return 0;
        return readPointer(stats + 0x8C);
    }

    void forgetPending() {
        g_forcedUseActive = 0;
        g_skipWhiff = 0;
        g_pendingItem = 0;
        g_waitingResult = 0;
        g_useAgain = 0;
        g_forceUse = 0;
        g_countForDecrement = 0;
        g_countForSouls = 0;
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
    DWORD pickerMaximum(const SoulItem* item, DWORD held) {
        if (!item || item->reward == 0) return 1;
        const DWORD souls = currentSouls();
        if (souls >= kMaxSouls) return 1;
        const DWORD room = kMaxSouls - souls;
        const DWORD fits = room / item->reward;
        DWORD maximum = held;
        if (fits < maximum) maximum = fits;
        if (maximum < 1) maximum = 1;
        return maximum;
    }

    // Opens the game's number picker.
    void openPicker(DWORD maximum) {
        const DWORD menu = menuObject();
        if (!menu) return;
        __try {
            *(DWORD*)kDialogMessageId = kPickerMessageId;
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
    if (g_pendingItem || g_countForDecrement || g_countForSouls || g_forcedUseActive) {
        SDLOG(1, "bulkItemUse: dropping left-over state (item %u, count %u) before a normal use\n",
            g_pendingItem, g_countForDecrement);
        forgetPending();
    }

    const SoulItem* item = findSoulItem(itemId);
    if (!item) return itemId;              // not one of the ten soul items

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
    g_countForSouls = 0;
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
    g_countForDecrement = chosen;
    g_countForSouls = chosen;
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
    const DWORD wanted = g_countForSouls;
    if (wanted < 2) return newSouls;
    if (!g_forcedUseActive) return newSouls; // not the use we asked for: never touch it

    const SoulItem* item = findSoulItem(g_pendingItem);
    if (!item) return newSouls;

    // Safety check: this must be the award for our item and no other. If the game just gave a different number of
    // souls (an enemy died, the amount was cut short at the limit), nothing is changed.
    if (newSouls <= oldSouls) return newSouls;
    const DWORD reward = newSouls - oldSouls;
    if (reward != item->reward) return newSouls;

    g_countForSouls = 0;                    // one correction only

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

    // -----------------------------------------------------------------------------------------------------------------
    // Installing
    // -----------------------------------------------------------------------------------------------------------------
    const HookSite kSites[8] = {
        { "item use",           0x00E53E47, 13, kExpectUse       },
        { "inventory count",    0x00C09753,  6, kExpectDec       },
        { "dialog answer",      0x00C7F8E6,  6, kExpectDlgResult },
        { "dialog closed",      0x00C7FE47,  5, kExpectDlgCancel },
        { "use item now",       0x00E1DA05,  6, kExpectForce     },
        { "soul count",         0x00BFC4E2,  6, kExpectSouls     },
        { "inventory quantity", 0x00C0AA80,  5, kExpectInvQty    },
        { "cannot-use animation",0x00E2B2CF,  6, kExpectWhiff     },
    };

    const void* kStubs[8] = {
        (const void*)&use_stub,
        (const void*)&dec_stub,
        (const void*)&dlg_result_stub,
        (const void*)&dlg_cancel_stub,
        (const void*)&force_stub,
        (const void*)&souls_stub,
        (const void*)&inv_qty_stub,
        (const void*)&whiff_stub,
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

    // Check every site first. One wrong byte means a different game build, and then nothing is patched at all.
    for (int i = 0; i < 8; ++i) {
        DWORD expectedLength = kSites[i].length;
        if (memcmp((const void*)kSites[i].address, kSites[i].expected, expectedLength) != 0) {
            SDLOG(0, "bulkItemUse: the code for '%s' at 0x%08X is not what this build should have, "
                     "bulkItemUse stays off\n", kSites[i].name, kSites[i].address);
            return false;
        }
    }

    for (int i = 0; i < 8; ++i) {
        if (!writeJump(kSites[i].address, kSites[i].length, kStubs[i])) {
            SDLOG(0, "bulkItemUse: could not write at 0x%08X, bulkItemUse stays off\n", kSites[i].address);
            return false; // earlier hooks stay in place but do nothing, because g_installed is still false
        }
    }

    g_installed = true;
    SDLOG(0, "bulkItemUse: on (%u soul items, inventory menu only)\n", kSoulItemCount);
    return true;
}

bool bulkItemUseActive() {
    return g_installed;
}
