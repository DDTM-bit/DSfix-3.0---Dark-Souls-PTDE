#include "CovenantOffer.h"
#include "Settings.h" // SDLOG reads the log level when RELEASE_VER is not defined
#include "main.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

// =====================================================================================================================
// covenantOfferings - offer several items, or several humanity, to a covenant at once.
//
// Research credit: the Dark Souls Overhaul team (metal-crow, Sean Pesce). The addresses below and the idea of offering
// more than one at a time come from their work. The code in this file is DSfix's own.
//
// What happens normally: you talk to a covenant altar or leader, the game asks "Offer Sunlight Medal?" with Yes and
// No, and if you answer Yes it takes one item and raises your devotion by one. To go from 0 to 80 you answer that
// question eighty times.
//
// What happens with this setting on:
//   1. The game is about to show that question. We leave the question exactly as it is - it already says which item,
//      in every language - and only change the kind of window: instead of Yes/No it becomes the game's own number
//      picker, the same one used for "how many to discard".
//   2. You choose a number and confirm. The game then does its normal single offering: one item away, one devotion
//      point up.
//   3. At the moment it writes that one point, we write the chosen number of points instead, and take the rest of
//      the items (or the rest of the humanity) at the same time.
//
// So the game still performs exactly one normal offering. Only the two numbers are corrected, in a single step, so
// the items taken and the devotion gained can never disagree.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHY THE PICKER STOPS SHORT OF THE NEXT RANK
//
// Covenant rewards (the miracle or item you get at devotion 10, 30 and 80) are not handed out by the game's own
// program: they come from its script files, which cannot be read from here. Testing showed two things:
//
//   * standing at devotion 15 without ever having received the rank-1 reward and offering one more gave nothing. So
//     the game does not think "you are past 10 and never got it, here it is".
//   * going from 9 to 10 gave the reward and the message "Rank gained".
//
// In other words the reward belongs to the step, not to the number you hold. Whether a step that jumps straight over
// 10 would still count cannot be known without changing the game, so this feature never makes such a step. The
// picker never offers enough to reach the next rank: below rank 1 it stops at 9, between 10 and 30 at 29, and
// between 30 and 80 at 79. The offering that actually ranks you up is the game's own Yes/No question, unchanged, so
// the reward arrives exactly as it always did. Above 80 the picker goes up to 100, the highest the game stores.
//
// That costs three single answers over a whole covenant - one at 10, one at 30, one at 80 - and in exchange no
// reward can ever be lost.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHAT IS NEVER TOUCHED
//
// Only the six covenants that take offerings are involved, and each is recognised by the message number of its own
// question, which nothing else in the game uses. Forest Hunter counts invader kills instead of offerings and has no
// such question, so it is not in the list. Way of White and Princess's Guard have no devotion at all.
//
// The correction is written only when all of this is true at once: a picker we opened ourselves was answered with a
// number, the covenant being changed is the one that question belonged to, and the game is raising devotion by
// exactly one. Anything else - loading a save, joining a covenant, the halving that follows a betrayal, reverse
// hollowing, kindling a bonfire - never matches, so none of it can be affected.
// =====================================================================================================================

namespace {

    // -----------------------------------------------------------------------------------------------------------------
    // THE SIX COVENANTS THAT TAKE OFFERINGS
    //
    // 'messageId' is the message number of that covenant's own "Offer ...?" question. 'covenantId' is the number the
    // game stores for the covenant you belong to. 'itemId' is the item offered, or 0 when humanity is offered
    // instead (Darkwraith and Chaos Servant).
    // -----------------------------------------------------------------------------------------------------------------
    struct OfferCovenant {
        uint32_t messageId;
        uint32_t covenantId;
        uint32_t itemId;
        const char* name;
    };
    const OfferCovenant kCovenants[] = {
        { 10020200, 3,  375, "Warriors of Sunlight" },  // Sunlight Medal
        { 10020201, 4,    0, "Darkwraith" },            // humanity
        { 10020202, 5, 1110, "Path of the Dragon" },    // Dragon Scale
        { 10020203, 6,  109, "Gravelord Servant" },     // Eye of Death
        { 10020204, 8,  374, "Blade of the Darkmoon" }, // Souvenir of Reprisal
        { 10020205, 9,    0, "Chaos Servant" },         // humanity
        // Forest Hunter (covenant 7) ranks up by killing invaders, so it has no offering question and no entry here.
    };
    const uint32_t kCovenantCount = sizeof(kCovenants) / sizeof(kCovenants[0]);

    // The three devotion numbers at which a covenant rank, and its reward, is gained.
    const uint32_t kRankAt[3] = { 10, 30, 80 };
    // The highest devotion the game stores. Its own code clamps to this as well.
    const uint32_t kDevotionCap = 100;
    // The covenant numbers the game accepts for devotion run from this one upward.
    const uint32_t kFirstCovenantId = 3;

    // How long after the question was turned into a picker the answer may still be used. Generous, because you may
    // leave the picker open; a number can be used only once in any case.
    const DWORD kPendingValidMs = 120000;

    // -----------------------------------------------------------------------------------------------------------------
    // Addresses in DARKSOULS.exe (Steam build, PE timestamp 0x557F2FD0). Research credit: Dark Souls Overhaul team.
    // -----------------------------------------------------------------------------------------------------------------
    // The menu object. [kMenuManager] is a pointer to it.
    //   +0x204 / +0x208 / +0x20C the number picker's largest / smallest / starting value
    const DWORD kMenuManager = 0x013786D0;
    const DWORD kMenuPickerMax = 0x204;
    const DWORD kMenuPickerMin = 0x208;
    const DWORD kMenuPickerStart = 0x20C;
    // The kind of window the game is about to show. 3 is the number picker.
    const DWORD kDialogKindNumberPicker = 3;
    // Which message the window will show, and where the number you picked ends up.
    const DWORD kDialogMessageId = 0x012E33E0;
    const DWORD kPickerResult = 0x012E3400;
    // The value the picker holds when there is no answer in it.
    const DWORD kNoPickerResult = 0xFFFFFFFF;

    // The player's numbers: [[kGameDataMan] + 8] is the block.
    //   +0x07C  humanity
    //   +0x0E5  devotion of covenant 3, then one byte each for covenants 4 to 9
    //   +0x10B  which covenant you belong to
    //   +0x1B8  your inventory
    const DWORD kGameDataMan = 0x01378700;
    const DWORD kStatsHumanity = 0x7C;
    const DWORD kStatsDevotion = 0xE5;
    const DWORD kStatsCovenantId = 0x10B;
    const DWORD kStatsInventory = 0x1B8;

    // The inventory, as the game's own item search reads it (the search at 0x00C0AC80).
    //   +0x110 how many places there are at most
    //   +0x118 where the second block of places begins
    //   +0x120 / +0x124 the two blocks
    //   +0x13C how many places are in use
    // One place is 28 bytes: what kind of thing it is, then the item number, then how many.
    const DWORD kInvLimit = 0x110;
    const DWORD kInvSplit = 0x118;
    const DWORD kInvFirstBlock = 0x120;
    const DWORD kInvSecondBlock = 0x124;
    const DWORD kInvUsed = 0x13C;
    const DWORD kSlotSize = 28;
    const DWORD kSlotKind = 0;
    const DWORD kSlotItemId = 4;
    const DWORD kSlotCount = 8;
    // The kind number the game uses for consumables, which is what every covenant offering is.
    const DWORD kKindGoods = 0x40000000;
    // Never walk further than this many places, whatever the numbers in the inventory say.
    const DWORD kSlotScanLimit = 2048;

    // -----------------------------------------------------------------------------------------------------------------
    // The two places that are hooked. Both are checked against these bytes before anything is written; if either one
    // does not match, nothing at all is patched and the feature stays off.
    // -----------------------------------------------------------------------------------------------------------------
    struct HookSite {
        const char* name;
        DWORD address;
        DWORD length;             // bytes the jump replaces (5 for the jump, the rest filled with NOP)
        const BYTE* expected;
    };

    // 1. A window is about to be shown.   push ecx / mov ecx,[0x13786D0]
    const BYTE kExpectDialog[7] = { 0x51, 0x8B,0x0D,0xD0,0x86,0x37,0x01 };
    // 2. Devotion is about to be stored.  push ecx / mov eax,[esp+8]
    const BYTE kExpectDevotion[5] = { 0x51, 0x8B,0x44,0x24,0x08 };

    // Where each hook carries on afterwards. These are variables rather than numbers written into the hooks so that
    // the jumps below are plain indirect jumps, exactly as in BulkItemUse.cpp.
    DWORD g_dialogReturn = 0x00C7EE07;
    DWORD g_devotionReturn = 0x00BFC0A5;
    // The same address as kMenuManager, kept in a variable the hook can read.
    DWORD g_menuManagerAddress = 0x013786D0;

    // -----------------------------------------------------------------------------------------------------------------
    // What is remembered between the question and the moment the game stores devotion
    // -----------------------------------------------------------------------------------------------------------------
    bool g_installed = false;
    volatile DWORD g_pending = 0;        // 1 while a picker of ours is waiting to be acted on
    volatile DWORD g_pendingIndex = 0;   // which row of kCovenants it belongs to
    volatile DWORD g_pendingMax = 0;     // the largest number the picker was allowed to offer
    volatile DWORD g_pendingHeld = 0;    // how many items, or how much humanity, you had when it opened
    volatile DWORD g_pendingTick = 0;    // when it opened

    // -----------------------------------------------------------------------------------------------------------------
    // Reading and writing the game's memory without ever risking a crash. These helpers hold no C++ objects, so the
    // guard around them is allowed.
    // -----------------------------------------------------------------------------------------------------------------
    bool readDword(DWORD address, DWORD* out) {
        __try { *out = *(DWORD*)address; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool readByte(DWORD address, DWORD* out) {
        __try { *out = *(BYTE*)address; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool writeDword(DWORD address, DWORD value) {
        __try { *(DWORD*)address = value; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // The block with your numbers in it, or 0 while there is no game loaded.
    DWORD statsBlock() {
        DWORD manager = 0, block = 0;
        if (!readDword(kGameDataMan, &manager) || !manager) return 0;
        if (!readDword(manager + 8, &block)) return 0;
        return block;
    }

    DWORD menuObject() {
        DWORD menu = 0;
        if (!readDword(kMenuManager, &menu)) return 0;
        return menu;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Finding an item in the inventory
    //
    // This walks the places the same way the game's own item search does, and trusts a place only when both the kind
    // and the item number match. Returns the address of the place, or 0.
    // -----------------------------------------------------------------------------------------------------------------
    DWORD findGoodsSlot(DWORD itemId) {
        const DWORD stats = statsBlock();
        if (!stats) return 0;
        const DWORD inv = stats + kStatsInventory;

        DWORD limit = 0, split = 0, first = 0, second = 0, used = 0;
        if (!readDword(inv + kInvLimit, &limit)) return 0;
        if (!readDword(inv + kInvSplit, &split)) return 0;
        if (!readDword(inv + kInvFirstBlock, &first)) return 0;
        if (!readDword(inv + kInvSecondBlock, &second)) return 0;
        if (!readDword(inv + kInvUsed, &used)) return 0;
        if (!first || !second) return 0;

        DWORD scan = used + 1;
        if (split > scan) scan = split;
        if (scan > limit) scan = limit;
        if (scan > kSlotScanLimit) scan = kSlotScanLimit;

        for (DWORD i = 0; i < scan; ++i) {
            const DWORD slot = (i < split ? first : second) + i * kSlotSize;
            DWORD kind = 0, id = 0;
            if (!readDword(slot + kSlotKind, &kind)) continue;
            if (kind != kKindGoods) continue;
            if (!readDword(slot + kSlotItemId, &id)) continue;
            if (id == itemId) return slot;
        }
        return 0;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // The rank rule
    //
    // The highest devotion one offering may reach from 'devotion', so that the next rank is never crossed.
    // -----------------------------------------------------------------------------------------------------------------
    DWORD reachableLimit(DWORD devotion) {
        for (int i = 0; i < 3; ++i) {
            if (devotion < kRankAt[i]) return kRankAt[i] - 1;
        }
        return kDevotionCap;
    }

    const OfferCovenant* covenantForMessage(DWORD messageId, DWORD* indexOut) {
        for (DWORD i = 0; i < kCovenantCount; ++i) {
            if (kCovenants[i].messageId == messageId) {
                *indexOut = i;
                return &kCovenants[i];
            }
        }
        return 0;
    }

    void forgetPending() {
        g_pending = 0;
        g_pendingIndex = 0;
        g_pendingMax = 0;
        g_pendingHeld = 0;
    }

    // The number you picked, or 0 when there is nothing usable there. A number can be used only once, because the
    // picker is emptied as soon as it is taken, and it must lie inside what the picker was allowed to offer.
    DWORD chosenNumber() {
        DWORD n = 0;
        if (!readDword(kPickerResult, &n)) return 0;
        if (n == kNoPickerResult) return 0;
        if (n < 2 || n > g_pendingMax) return 0;
        return n;
    }

    // How many items, or how much humanity, you have right now for the covenant in 'c'.
    bool suppliesHeld(const OfferCovenant* c, DWORD* heldOut) {
        if (c->itemId == 0) {
            const DWORD stats = statsBlock();
            if (!stats) return false;
            return readDword(stats + kStatsHumanity, heldOut);
        }
        const DWORD slot = findGoodsSlot(c->itemId);
        if (!slot) return false;
        return readDword(slot + kSlotCount, heldOut);
    }

    // Takes up to 'extra' items, or up to 'extra' humanity, on top of the one the game takes itself.
    //
    // 'took' receives how many were really taken, which is what decides how many devotion points the offering
    // gives, so the two numbers can never disagree. Returns false and changes nothing at all when something does
    // not add up; the offering is then left as a plain single one.
    //
    // The one case where less is taken than asked: the game had already taken its own item before this moment, and
    // taking all of the rest would leave a place in the inventory holding none of that item. Emptying a place is
    // the game's own job, not ours, so one is left for it and one fewer is offered.
    bool takeExtraSupply(const OfferCovenant* c, DWORD extra, DWORD* took) {
        *took = 0;
        if (extra == 0) return true;

        const bool isHumanity = (c->itemId == 0);
        DWORD address = 0, now = 0;
        if (isHumanity) {
            const DWORD stats = statsBlock();
            if (!stats) return false;
            address = stats + kStatsHumanity;
            if (!readDword(address, &now)) return false;
        }
        else {
            const DWORD slot = findGoodsSlot(c->itemId);
            if (!slot) return false;
            address = slot + kSlotCount;
            if (!readDword(address, &now)) return false;
        }

        // The game takes its own one either just before this moment or just after it, so what is left now must be
        // exactly what you had, or one less. Anything else means this is not the offering we prepared.
        const bool gameAlreadyTookOne = (now + 1 == g_pendingHeld);
        if (now != g_pendingHeld && !gameAlreadyTookOne) {
            SDLOG(0, "covenantOfferings: %s, you had %u and %u is there now, left as a single offering\n",
                c->name, g_pendingHeld, now);
            return false;
        }

        DWORD take = extra;
        if (take > now) take = now;
        if (!isHumanity && gameAlreadyTookOne && take == now && take > 0) take -= 1;
        if (take == 0) return true;

        if (!writeDword(address, now - take)) return false;
        *took = take;
        return true;
    }

} // namespace

// =====================================================================================================================
// The two things the hooks call
// =====================================================================================================================

// A window is about to be shown. 'kind' is the kind the game asked for; the kind that is really used is returned.
DWORD _stdcall covOnDialogRequest(DWORD kind) {
    if (!g_installed) return kind;

    DWORD messageId = 0;
    if (!readDword(kDialogMessageId, &messageId)) return kind;

    DWORD index = 0;
    const OfferCovenant* c = covenantForMessage(messageId, &index);
    if (!c) return kind;

    // From here on this is a covenant offering question, so anything left over from an earlier one is dropped. This
    // is what makes sure a question we decide to leave alone can never pick up an old number.
    forgetPending();

    const DWORD stats = statsBlock();
    if (!stats) return kind;

    DWORD belongsTo = 0;
    if (!readByte(stats + kStatsCovenantId, &belongsTo)) return kind;
    if (belongsTo != c->covenantId) {
        SDLOG(0, "covenantOfferings: the question is %s but you belong to covenant %u, left alone\n",
            c->name, belongsTo);
        return kind;
    }

    DWORD devotion = 0;
    if (!readByte(stats + kStatsDevotion + (c->covenantId - kFirstCovenantId), &devotion)) return kind;

    DWORD held = 0;
    if (!suppliesHeld(c, &held)) {
        SDLOG(0, "covenantOfferings: %s, could not read how many you have, left alone\n", c->name);
        return kind;
    }

    const DWORD limit = reachableLimit(devotion);
    const DWORD allowed = (limit > devotion) ? (limit - devotion) : 0;
    DWORD maximum = (allowed < held) ? allowed : held;
    if (maximum > kDevotionCap) maximum = kDevotionCap;

    if (maximum < 2) {
        SDLOG(0, "covenantOfferings: %s, devotion %u, you have %u, room to %u - the game's own question is used\n",
            c->name, devotion, held, limit);
        return kind;
    }

    const DWORD menu = menuObject();
    if (!menu) return kind;

    // The question itself, and its two buttons, are left exactly as the game set them: it already names the item and
    // is translated into every language. Only the kind of window changes.
    writeDword(menu + kMenuPickerMax, maximum);
    writeDword(menu + kMenuPickerMin, 1);
    writeDword(menu + kMenuPickerStart, 1);
    writeDword(kPickerResult, kNoPickerResult); // an old answer must never be mistaken for this one

    g_pendingIndex = index;
    g_pendingMax = maximum;
    g_pendingHeld = held;
    g_pendingTick = GetTickCount();
    g_pending = 1;

    SDLOG(0, "covenantOfferings: %s, devotion %u, you have %u, picker offers 1 to %u (window kind %u -> %u)\n",
        c->name, devotion, held, maximum, kind, kDialogKindNumberPicker);
    return kDialogKindNumberPicker;
}

// Devotion is about to be stored. 'covIndex' counts from 0 for covenant 3. The value that is really stored is
// returned.
DWORD _stdcall covAdjustDevotion(DWORD object, DWORD covIndex, DWORD requested) {
    if (!g_installed || !g_pending) return requested;
    if (GetTickCount() - g_pendingTick > kPendingValidMs) { forgetPending(); return requested; }
    if (covIndex > 6 || g_pendingIndex >= kCovenantCount) return requested;

    const OfferCovenant* c = &kCovenants[g_pendingIndex];
    if (covIndex + kFirstCovenantId != c->covenantId) return requested; // a different covenant is being changed

    DWORD old = 0;
    if (!readByte(object + kStatsDevotion + covIndex, &old)) return requested;
    if (requested != old + 1) return requested; // only ever a plain single offering, never anything else

    const DWORD chosen = chosenNumber();
    if (chosen < 2) return requested;

    // The rank rule is worked out again from the number the game holds at this very moment, so even if something
    // unexpected happened in between, one offering can never cross a rank.
    const DWORD limit = reachableLimit(old);
    DWORD wanted = old + chosen;
    if (wanted > limit) wanted = limit;
    if (wanted > kDevotionCap) wanted = kDevotionCap;
    if (wanted < old + 2) return requested;
    if (g_pendingHeld < wanted - old) return requested;

    // Take what belongs to the extra points, and then grant exactly as many points as were really paid for.
    DWORD took = 0;
    if (!takeExtraSupply(c, wanted - old - 1, &took)) {
        forgetPending();
        return requested;
    }

    forgetPending();
    writeDword(kPickerResult, kNoPickerResult); // the same answer can never be used twice

    if (took == 0) return requested;            // nothing extra could be paid: an ordinary single offering

    const DWORD target = old + took + 1;
    SDLOG(0, "covenantOfferings: %s, you asked for %u, offered %u, devotion %u -> %u\n",
        c->name, chosen, took + 1, old, target);
    return target;
}

namespace {

    // -----------------------------------------------------------------------------------------------------------------
    // The hooks themselves
    // -----------------------------------------------------------------------------------------------------------------
    __declspec(naked) void dialog_stub() {
        __asm {
            pushad
            pushfd
            push edx                        // the kind of window the game asked for
            call covOnDialogRequest
            mov dword ptr [esp + 0x18], eax // becomes edx again at POPAD
            popfd
            popad
            // the two instructions the jump replaced
            push ecx
            mov ecx, dword ptr [g_menuManagerAddress]
            mov ecx, dword ptr [ecx]
            jmp dword ptr [g_dialogReturn]
        }
    }

    __declspec(naked) void devotion_stub() {
        __asm {
            pushad
            pushfd
            push dword ptr [esp + 0x28]     // the devotion the game wants to store, originally at [esp+4]
            push edx                        // which covenant, counting from 0 for covenant 3
            push ecx                        // the block with the devotion bytes in it
            call covAdjustDevotion
            mov dword ptr [esp + 0x28], eax
            popfd
            popad
            // the two instructions the jump replaced
            push ecx
            mov eax, dword ptr [esp + 8]
            jmp dword ptr [g_devotionReturn]
        }
    }

    const HookSite kSites[2] = {
        { "offering question", 0x00C7EE00, 7, kExpectDialog   },
        { "devotion",          0x00BFC0A0, 5, kExpectDevotion },
    };

    const void* kStubs[2] = {
        (const void*)&dialog_stub,
        (const void*)&devotion_stub,
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

bool installCovenantOffer() {
    if (g_installed) return true;
    if (!Settings::get().getCovenantOfferings()) return false;

    // Check both places first. One wrong byte means a different game build, and then nothing is patched at all.
    for (int i = 0; i < 2; ++i) {
        if (memcmp((const void*)kSites[i].address, kSites[i].expected, kSites[i].length) != 0) {
            SDLOG(0, "covenantOfferings: the code for '%s' at 0x%08X is not what this build should have, "
                     "covenantOfferings stays off\n", kSites[i].name, kSites[i].address);
            return false;
        }
    }

    for (int i = 0; i < 2; ++i) {
        if (!writeJump(kSites[i].address, kSites[i].length, kStubs[i])) {
            SDLOG(0, "covenantOfferings: could not write at 0x%08X, covenantOfferings stays off\n",
                kSites[i].address);
            return false; // an earlier hook stays in place but does nothing, because g_installed is still false
        }
    }

    g_installed = true;
    SDLOG(0, "covenantOfferings: on (6 covenants, the picker never reaches the next rank)\n");
    return true;
}

bool covenantOfferActive() {
    return g_installed;
}
