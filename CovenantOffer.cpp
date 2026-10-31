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

// =====================================================================================================================
// BonfireCovenants - switch to a covenant you already joined, at any bonfire.
//
// The bonfire list gets the leaders' own line "Enter Covenant" (right after Attune Magic). It opens a list of the
// covenants you joined in this playthrough, in game order: your current one marked "* ", a broken one "x " (picking it
// tells why). Picking another one shows the leaders' own question "Join Covenant? (abandons former Covenant)". Yes plays
// the leaders' oath sound (s123456789, common event 840) while the screen fades to black and back (3 s, drawn by
// DSfix); in the dark the game's BreakCovenantPledge runs (the devotion of the covenant you leave is halved) and the new
// covenant is set, as in every leader's talk script. "Covenant established" is opened at once, under the fade: some game
// window must stay open while you sit, or the hero's sitting pose is lost (he stands, then plays "stand up" on leaving).
// After that window, the bonfire list again. Oswald is never involved.
//
// All of it comes from the game's own talk scripts (script\talk\*.talkesdbnd) and event scripts (event\*.emevd):
//   * every leader: AddTalkListData(.., 15000200 "Enter Covenant", ..) -> already in it: 10010726 / phantom present:
//     10010747 / else OpenGenericDialog(8, 10010745, 3, 4, 2) -> Yes: BreakCovenantPledge(), ChangePlayerStats(11, set,
//     id), a flag 841..849 that only plays the kneeling animation (common event 840, not used here: you sit), and on
//     the first join the "joined once" flags below (they hand out the first gift) -> 10010729 "Covenant established".
//   * "joined once": the first-join flags. They are cleared by New Game+, so the list starts again like Remastered.
//     Way of White has none, so DSfix sets the unused flag 11020596 the first time it sees you in Way of White.
//   * not available ("broken"): the leaders' own checks - leader dead or hostile, Kaathe gone, banished from the
//     Forest Hunters until Oswald's absolution, ...
//
// How it is done (the bonfire talk script itself is never changed):
//   1. AddTalkListData (0x00FCF7E2): the bonfire list is recognised by its first line (9, 15000005 "Leave"); our line
//      (index 20, unused by every bonfire) is added after Attune Magic. Every line is remembered, to rebuild the list.
//   2. GetTalkListEntryResult (0x00D49A26): the bonfire script asks for the picked line every frame. When it is ours,
//      DSfix runs the covenant list and the windows itself while the script only ever gets "nothing picked".
//   3. ClearTalkListData (0x00FCF811): the bonfire script leaves its list (menu, attacked, ...): our steps stop.
//   4. Text lookup (0x00D175E0, every game text goes through it): our own text ids 15009901..15009919 - the covenant
//      names (the status screen's own texts, every language) with their mark, and the reasons for broken covenants
//      (DSfix's English text).
// =====================================================================================================================
#include <cstdio>
#include <d3d9.h>

namespace bonfirecov {
    // Addresses in DARKSOULS.exe (Steam build, PE timestamp 0x557F2FD0)
    const DWORD kMenuMan = 0x013786D0;           // [kMenuMan] = menu manager
    const DWORD kGameDataMan = 0x01378700;       // [[kGameDataMan] + 8] = the player's numbers
    const DWORD kEventFlagMan = 0x0137D7D4;
    const DWORD kTalkerMan = 0x013784A0;         // ShowShopMessage notes the talker at [this] + 0xB04
    const DWORD kNetMan = 0x0137D790;            // BreakCovenantPledge refreshes the online data with it
    const DWORD kDialogKindOverride = 0x0137D7C8; // [this] + 0x7F6: 0xFF = use the kind the script asked for
    const DWORD kFnIsShowMenu = 0x00C2ABE0;
    const DWORD kFnTalkListAdd = 0x00C2BB50;     // eax = &{text, index}
    const DWORD kFnTalkListClear = 0x00C2B140;
    const DWORD kFnTalkMenuOpen = 0x00C2A650;    // ecx = &{byte 0, -1, 0}
    const DWORD kFnTalkerIndex = 0x00D45D90;     // eax = talker -> index or -1
    const DWORD kFnTalkerNote = 0x00C40650;      // edx = that index
    const DWORD kFnDialogSet = 0x00C7EDC0;       // edx = text, [esp+4] buttons, [esp+8] 4th value
    const DWORD kFnDialogShow = 0x00C7EE00;      // edx = kind of window
    const DWORD kFnFlagGet = 0x00D38C40;         // eax = event flag man, cl = 0, [esp+4] flag
    const DWORD kFnFlagSet = 0x00D38CB0;         // eax = event flag man, cl = 0, [esp+4] flag, [esp+8] on
    const DWORD kFnMultiplayer = 0x00D2A4B0;     // a phantom is in the world
    const DWORD kFnDevotionGet = 0x00BFBE90;     // ecx = numbers, [esp+4] covenant - 3
    const DWORD kFnDevotionSet = 0x00BFC0A0;     // ecx = numbers, edx = covenant - 3, [esp+4] value
    const DWORD kFnNetRefresh = 0x00D74A60;      // eax = [kNetMan]
    const DWORD kFnSetStat = 0x00D4B1E0;         // cdecl [esp+4] numbers, edx = stat (11 = covenant), eax = value
    const DWORD kFnMenuText = 0x00D17860;        // edi = text id -> text (menu texts)
    const DWORD kMenuSound = 0x013786DC;         // the menu sounds: [this] + 8 + slot * 16, +0x98 stop first
    const DWORD kFnSoundId = 0x004578B0;         // cdecl ecx = sound id, [esp+4] out, [esp+8] kind -> out
    const DWORD kFnSoundStop = 0x006D10C0;       // ecx = slot
    const DWORD kFnSoundSet = 0x006D1020;        // ecx = slot, [esp+4] sound id: the slot plays it

    // Menu manager
    const DWORD kMmTalkMenu = 0x40;              // != 0: the talk list is open
    const DWORD kMmDialogKind = 0x60;            // != 0: a window (Yes/No, message) is open
    const DWORD kMmDialog = 0x9E8;               // the window: +0x10 shown, +0x18 the button answer
    const DWORD kMmDialogNear = 0x16C;
    const DWORD kMmTalkResult = 0xB04;           // the picked line's index (-1 nothing yet, 0 = closed with B)
    const DWORD kMmTalker = 0xB08;
    const DWORD kStatsCovenant = 0x10B;

    // Texts (event texts / menu texts of the game, every language)
    const DWORD kTextLeave = 15000005;           // the bonfire list's first line
    const DWORD kTextAttune = 15000130;          // Attune Magic (index 5)
    const DWORD kTextEnter = 15000200;           // "Enter Covenant"
    const DWORD kTextSame = 10010726;            // "Already belong to this Covenant"
    const DWORD kTextJoin = 10010745;            // "Join Covenant? (abandons former Covenant)"
    const DWORD kTextDone = 10010729;            // "Covenant established"
    const DWORD kTextPhantom = 10010747;         // "Cannot enter covenant with phantom present."
    const DWORD kTextNames = 105100;             // + covenant: its name
    const DWORD kCovTextBase = 15009900;         // our list lines: + covenant (no game text uses these)
    const DWORD kReasonTextBase = 15009910;      // the window for a broken covenant: + covenant
    const DWORD kOathSound = 123456789;          // the leaders' oath sound (common event 840: s123456789)
    const DWORD kSoundKindSfx = 5;               // 's'
    const DWORD kSoundSlot = 4;                  // a rarely used menu sound slot (the game's id 999999999)
    // The fade after Yes: out, black (the switch happens at the start of it), in
    const DWORD kFadeOutMs = 1000, kFadeHoldMs = 1000, kFadeInMs = 1000;

    const int kLineIndex = 20;                   // our bonfire line (bonfires use 0..12)
    const int kCovIndexBase = 20;                // our covenant lines: 21..29

    // Covenants: 1 Way of White, 2 Princess's Guard, 3 Warrior of Sunlight, 4 Darkwraith, 5 Path of the Dragon,
    // 6 Gravelord Servant, 7 Forest Hunter, 8 Blade of the Darkmoon, 9 Chaos Servant.
    const DWORD kWayOfWhiteMark = 11020596;      // DSfix's own "joined once" flag for Way of White (unused by the game)
    struct Joined { DWORD a, b; };               // first-join flags (either one = joined once)
    const Joined kJoined[10] = {
        { 0, 0 },
        { kWayOfWhiteMark, 0 },
        { 11510595, 0 },
        { 11010594, 0 },
        { 11600594, 0 },
        { 11320581, 11320592 },
        { 11310580, 11310592 },
        { 11200592, 71200047 },
        { 11510580, 11510596 },
        { 11400596, 0 },
    };

    bool g_installed = false;

    // -----------------------------------------------------------------------------------------------------------------
    // Calls into the game (they take their values in registers)
    // -----------------------------------------------------------------------------------------------------------------
    __declspec(naked) DWORD __stdcall callPlain(DWORD fn) {
        __asm {
            pushad
            call dword ptr[esp + 0x24]
            mov dword ptr[esp + 0x1C], eax
            popad
            ret 4
        }
    }
    __declspec(naked) DWORD __stdcall callEax(DWORD fn, DWORD eaxValue) {
        __asm {
            pushad
            mov eax, dword ptr[esp + 0x28]
            call dword ptr[esp + 0x24]
            mov dword ptr[esp + 0x1C], eax
            popad
            ret 8
        }
    }
    __declspec(naked) DWORD __stdcall callEcx(DWORD fn, DWORD ecxValue) {
        __asm {
            pushad
            mov ecx, dword ptr[esp + 0x28]
            call dword ptr[esp + 0x24]
            mov dword ptr[esp + 0x1C], eax
            popad
            ret 8
        }
    }
    __declspec(naked) DWORD __stdcall callEdx(DWORD fn, DWORD edxValue) {
        __asm {
            pushad
            mov edx, dword ptr[esp + 0x28]
            call dword ptr[esp + 0x24]
            mov dword ptr[esp + 0x1C], eax
            popad
            ret 8
        }
    }
    __declspec(naked) DWORD __stdcall callEdi(DWORD fn, DWORD ediValue) {
        __asm {
            pushad
            mov edi, dword ptr[esp + 0x28]
            call dword ptr[esp + 0x24]
            mov dword ptr[esp + 0x1C], eax
            popad
            ret 8
        }
    }
    // edx = value, two stack values the function removes itself
    __declspec(naked) DWORD __stdcall callEdxPush2(DWORD fn, DWORD edxValue, DWORD first, DWORD second) {
        __asm {
            pushad
            mov edx, dword ptr[esp + 0x28]
            push dword ptr[esp + 0x30]          // second
            push dword ptr[esp + 0x30]          // first (one push further down now)
            call dword ptr[esp + 0x2C]
            mov dword ptr[esp + 0x1C], eax
            popad
            ret 16
        }
    }
    // ecx = value, one stack value the function removes itself
    __declspec(naked) DWORD __stdcall callEcxPush1(DWORD fn, DWORD ecxValue, DWORD first) {
        __asm {
            pushad
            mov ecx, dword ptr[esp + 0x28]
            push dword ptr[esp + 0x2C]
            call dword ptr[esp + 0x28]
            mov dword ptr[esp + 0x1C], eax
            popad
            ret 12
        }
    }
    // ecx, edx = values, one stack value the function removes itself
    __declspec(naked) DWORD __stdcall callEcxEdxPush1(DWORD fn, DWORD ecxValue, DWORD edxValue, DWORD first) {
        __asm {
            pushad
            mov ecx, dword ptr[esp + 0x28]
            mov edx, dword ptr[esp + 0x2C]
            push dword ptr[esp + 0x30]
            call dword ptr[esp + 0x28]
            mov dword ptr[esp + 0x1C], eax
            popad
            ret 16
        }
    }
    // eax, edx = values, one stack value the caller removes (cdecl)
    __declspec(naked) DWORD __stdcall callEaxEdxCdecl1(DWORD fn, DWORD eaxValue, DWORD edxValue, DWORD first) {
        __asm {
            pushad
            mov eax, dword ptr[esp + 0x28]
            mov edx, dword ptr[esp + 0x2C]
            push dword ptr[esp + 0x30]
            call dword ptr[esp + 0x28]
            add esp, 4
            mov dword ptr[esp + 0x1C], eax
            popad
            ret 16
        }
    }
    // ecx = value, two stack values the caller removes (cdecl)
    __declspec(naked) DWORD __stdcall callEcxCdecl2(DWORD fn, DWORD ecxValue, DWORD first, DWORD second) {
        __asm {
            pushad
            mov ecx, dword ptr[esp + 0x28]
            push dword ptr[esp + 0x30]          // second
            push dword ptr[esp + 0x30]          // first (one push further down now)
            call dword ptr[esp + 0x2C]
            add esp, 8
            mov dword ptr[esp + 0x1C], eax
            popad
            ret 16
        }
    }
    // event flags: eax = flag man, cl = 0
    __declspec(naked) DWORD __stdcall callFlagGet(DWORD fn, DWORD man, DWORD flag) {
        __asm {
            pushad
            mov eax, dword ptr[esp + 0x28]
            push dword ptr[esp + 0x2C]
            xor ecx, ecx
            call dword ptr[esp + 0x28]
            movzx eax, al
            mov dword ptr[esp + 0x1C], eax
            popad
            ret 12
        }
    }
    __declspec(naked) void __stdcall callFlagSet(DWORD fn, DWORD man, DWORD flag, DWORD on) {
        __asm {
            pushad
            mov eax, dword ptr[esp + 0x28]
            push dword ptr[esp + 0x30]          // on
            push dword ptr[esp + 0x30]          // flag (one push further down now)
            xor ecx, ecx
            call dword ptr[esp + 0x2C]
            popad
            ret 16
        }
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Safe reads and small helpers
    // -----------------------------------------------------------------------------------------------------------------
    DWORD rd32(DWORD a) {
        DWORD v = 0;
        __try { v = *(const DWORD*)a; }
        __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
        return v;
    }
    DWORD rd8(DWORD a) {
        DWORD v = 0;
        __try { v = *(const BYTE*)a; }
        __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
        return v;
    }
    DWORD playerData() {
        const DWORD top = rd32(kGameDataMan);
        return top ? rd32(top + 8) : 0;
    }
    int currentCovenant() {
        const DWORD p = playerData();
        return p ? (int)rd8(p + kStatsCovenant) : -1;
    }
    bool flag(DWORD id) {
        const DWORD man = rd32(kEventFlagMan);
        return id && man && callFlagGet(kFnFlagGet, man, id) != 0;
    }
    void setFlag(DWORD id) {
        const DWORD man = rd32(kEventFlagMan);
        if (man) callFlagSet(kFnFlagSet, man, id, 1);
    }
    bool dialogOpen() {
        return (callPlain(kFnIsShowMenu) & 8) != 0;
    }

    // Joined once in this playthrough (the current covenant always counts)
    bool joinedOnce(int c, int current) {
        if (c < 1 || c > 9) return false;
        if (c == current) return true;
        return flag(kJoined[c].a) || flag(kJoined[c].b);
    }
    // Not broken: the leader could still take you (the leaders' own checks)
    bool available(int c) {
        switch (c) {
        case 1: // Petrus or Reah alive and friendly
            return !(flag(1195) || flag(1196) || flag(1197) || flag(1198)) ||
                   !(flag(1176) || flag(1177) || flag(1179) || flag(1180));
        case 2: return !(flag(1231) || flag(1232));             // Gwynevere not hostile / not killed
        case 3: return true;                                     // altar
        case 4: return !flag(1677);                              // Kaathe not gone
        case 5: return !flag(1781);                              // the Everlasting Dragon still talks
        case 6: return true;                                     // coffin
        case 7: return !(flag(1712) || flag(746));               // not banished (until absolution)
        case 8: return !(flag(1241) || flag(1242));              // Gwyndolin not hostile / not dead
        case 9: return !(flag(1271) || flag(1272));              // Daughter of Chaos not hostile / not killed
        }
        return false;
    }
    // In the list: the current covenant and every joined one (broken ones are shown marked "x ")
    bool listed(int c, int current) {
        return joinedOnce(c, current);
    }

    // -----------------------------------------------------------------------------------------------------------------
    // The talk list (MenuMan + 0xAFC) and the windows
    // -----------------------------------------------------------------------------------------------------------------
    struct Line { DWORD text; DWORD index; };
    const int kMaxLines = 24;
    Line g_lines[kMaxLines];                     // the bonfire list as its script built it (ours included)
    int g_lineCount = 0;
    DWORD g_listEv = 0;                          // the talk script that built the list
    bool g_listIsBonfire = false;
    bool g_lineAdded = false;

    void listAdd(DWORD text, DWORD index) {
        Line l = { text, index };
        callEax(kFnTalkListAdd, (DWORD)&l);
    }
    void listClear() {
        callPlain(kFnTalkListClear);
    }
    // What ShowShopMessage does: opens the talk list for this talker. False while another menu is in the way.
    bool listOpen(DWORD ev) {
        const DWORD mm = rd32(kMenuMan);
        if (!mm || !ev) return false;
        if (callPlain(kFnIsShowMenu) & 0xEFFFFB33) return false;
        *(DWORD*)(mm + kMmTalker) = rd32(ev + 0xC);
        struct { BYTE b; DWORD a; DWORD c; } open = { 0, 0xFFFFFFFF, 0 };
        callEcx(kFnTalkMenuOpen, (DWORD)&open);
        const DWORD idx = callEax(kFnTalkerIndex, rd32(ev + 8));
        if (idx != 0xFFFFFFFF) {
            callEdx(kFnTalkerNote, idx);
            const DWORD tm = rd32(kTalkerMan);
            if (tm) *(DWORD*)(tm + kMmTalkResult) = idx + 1000;
        }
        return true;
    }
    // What OpenGenericDialog(kind, text, buttons, fourth, near) does
    void dialogShow(DWORD kind, DWORD text, DWORD buttons, DWORD fourth, DWORD nearValue) {
        const DWORD mm = rd32(kMenuMan);
        if (!mm) return;
        callEdxPush2(kFnDialogSet, text, buttons, fourth);
        const DWORD dlg = rd32(mm + kMmDialog);
        if (dlg) *(BYTE*)(dlg + 0x10) = 1;
        if (nearValue == 2) *(DWORD*)(mm + kMmDialogNear) = 1;
        else if (nearValue == 1) *(DWORD*)(mm + kMmDialogNear) = 0;
        const DWORD ov = rd32(kDialogKindOverride);
        const signed char o = ov ? (signed char)rd8(ov + 0x7F6) : (signed char)-1;
        callEdx(kFnDialogShow, o == -1 ? kind : (DWORD)(int)o);
        *(DWORD*)(mm + 0x154) = 0xFFFFFFFF;
        *(DWORD*)(mm + 0x158) = 0xFFFFFFFF;
    }
    int dialogAnswer() {
        const DWORD mm = rd32(kMenuMan);
        const DWORD dlg = mm ? rd32(mm + kMmDialog) : 0;
        return dlg ? (int)rd32(dlg + 0x18) : -1;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Our texts: the covenant names ("Warrior of Sunlight", current "* Warrior of Sunlight", broken "x Darkwraith") and
    // the reason a broken covenant cannot be joined
    // -----------------------------------------------------------------------------------------------------------------
    wchar_t g_names[10][96];
    bool g_namesReady[10];
    const wchar_t* g_reason[10];

    void prepareName(int c, const wchar_t* mark) {
        g_namesReady[c] = false;
        const wchar_t* name = (const wchar_t*)callEdi(kFnMenuText, kTextNames + (DWORD)c);
        if (!name) return;
        __try {
            _snwprintf_s(g_names[c], 96, _TRUNCATE, L"%s%s", mark, name);
            g_namesReady[c] = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // Why a covenant is broken (the same checks as available())
    const wchar_t* reasonFor(int c) {
        switch (c) {
        case 1: return L"Petrus and Reah can no longer take your oath.";
        case 2: return flag(1232) ? L"Gwynevere is gone." : L"Gwynevere is hostile.";
        case 4: return L"Darkstalker Kaathe is gone.";
        case 5: return L"The Everlasting Dragon is silent.";
        case 7: return L"You were banished. Seek absolution.";
        case 8: return flag(1242) ? L"Dark Sun Gwyndolin is dead." : L"Dark Sun Gwyndolin is hostile.";
        case 9: return flag(1272) ? L"The Daughter of Chaos is dead." : L"The Daughter of Chaos is hostile.";
        }
        return L"This covenant cannot be joined.";
    }
    // Hook 4: our text ids (every game text lookup comes here first)
    const wchar_t* __stdcall customText(DWORD id) {
        if (id > kCovTextBase && id <= kCovTextBase + 9) {
            const int c = (int)(id - kCovTextBase);
            return g_namesReady[c] ? g_names[c] : 0;
        }
        if (id > kReasonTextBase && id <= kReasonTextBase + 9) return g_reason[id - kReasonTextBase];
        return 0;
    }

    // The leaders' oath sound, through a menu sound slot (as the game's own menu sounds are played)
    void playOathSound() {
        const DWORD ms = rd32(kMenuSound);
        if (!ms) return;
        const DWORD slot = ms + 8 + kSoundSlot * 16;
        DWORD soundId[8] = { 0 };
        if (rd8(ms + 0x98)) callEcx(kFnSoundStop, slot);
        const DWORD out = callEcxCdecl2(kFnSoundId, kOathSound, (DWORD)soundId, kSoundKindSfx);
        if (out) callEcxPush1(kFnSoundSet, slot, out);
    }

    // The fade (drawn at Present by bonfireCovenantsDraw)
    volatile DWORD g_fadeStart = 0;              // 0 = no fade

    // -----------------------------------------------------------------------------------------------------------------
    // The steps after "Enter Covenant" (driven by the bonfire script's own "which line?" question, every frame)
    // -----------------------------------------------------------------------------------------------------------------
    enum Step { S_IDLE, S_OPEN_LIST, S_LIST, S_CONFIRM, S_CEREMONY, S_MSG_TO_LIST, S_MSG_TO_BONFIRE, S_OPEN_BONFIRE };
    int g_step = S_IDLE;
    bool g_built = false;                        // the list for the open step is built (it may wait to open)
    DWORD g_stepEv = 0;
    DWORD g_stepTime = 0;
    int g_pick = 0;
    bool g_switched = false;

    // The covenant list: our lines only, in game order (current "* ", broken "x ")
    void buildCovenantList() {
        const int current = currentCovenant();
        listClear();
        for (int c = 1; c <= 9; ++c) {
            if (!listed(c, current)) continue;
            const bool broken = c != current && !available(c);
            g_reason[c] = broken ? reasonFor(c) : 0;
            prepareName(c, c == current ? L"* " : (broken ? L"x " : L""));
            listAdd(kCovTextBase + (DWORD)c, (DWORD)(kCovIndexBase + c));
        }
    }
    // The bonfire list again, exactly as its script built it
    void buildBonfireList() {
        listClear();
        for (int i = 0; i < g_lineCount; ++i) listAdd(g_lines[i].text, g_lines[i].index);
    }

    // BreakCovenantPledge (talk command 60): the devotion of the covenant you leave is halved, the online data refreshed
    void breakPledge(DWORD stats) {
        const int c = (int)rd8(stats + kStatsCovenant);
        if (c >= 3 && c <= 9) {
            const DWORD v = callEcxPush1(kFnDevotionGet, stats, (DWORD)(c - 3)) & 0xFF;
            callEcxEdxPush1(kFnDevotionSet, stats, (DWORD)(c - 3), v / 2);   // the game: (int)(v * 0.5)
        }
        callEax(kFnNetRefresh, rd32(kNetMan));
    }
    void switchTo(int c) {
        const DWORD stats = playerData();
        if (!stats) return;
        breakPledge(stats);
        callEaxEdxCdecl1(kFnSetStat, (DWORD)c, 11, stats);                    // ChangePlayerStats(11, set, c)
    }

    void stopSteps() {
        g_step = S_IDLE;
        g_built = false;
        g_fadeStart = 0;
    }

    void runStep(int result) {
        switch (g_step) {
        case S_OPEN_LIST:
            if (!g_built) { buildCovenantList(); g_built = true; }
            if (listOpen(g_stepEv)) { g_step = S_LIST; g_built = false; }
            break;
        case S_LIST: {
            const DWORD mm = rd32(kMenuMan);
            if (result == -1 && mm && rd32(mm + kMmTalkMenu) != 0) break;  // still choosing
            if (result > kCovIndexBase && result <= kCovIndexBase + 9) {
                g_pick = result - kCovIndexBase;
                const int current = currentCovenant();
                if (g_pick == current) { dialogShow(7, kTextSame, 1, 0, 2); g_step = S_MSG_TO_LIST; }
                else if (g_reason[g_pick]) { dialogShow(7, kReasonTextBase + (DWORD)g_pick, 1, 0, 2); g_step = S_MSG_TO_LIST; }
                else if (callPlain(kFnMultiplayer) & 0xFF) { dialogShow(7, kTextPhantom, 1, 0, 2); g_step = S_MSG_TO_LIST; }
                else { dialogShow(8, kTextJoin, 3, 4, 2); g_step = S_CONFIRM; }
            }
            else g_step = S_OPEN_BONFIRE;                                // closed with B
            break;
        }
        case S_CONFIRM:
            if (dialogOpen()) break;
            if (dialogAnswer() == 1) {
                playOathSound();
                g_switched = false;
                g_fadeStart = GetTickCount();
                dialogShow(7, kTextDone, 1, 0, 2);                       // at once: a window stays open while you sit
                g_step = S_CEREMONY;
            }
            else g_step = S_OPEN_LIST;                                   // No (or closed)
            break;
        case S_CEREMONY: {
            const DWORD t = GetTickCount() - g_fadeStart;
            if (!g_switched && t >= kFadeOutMs) {                       // fully dark: the switch itself
                switchTo(g_pick);
                g_switched = true;
            }
            // "Covenant established" closed (the fade ends by itself): the bonfire list at once, no gap without a window
            if (!dialogOpen()) {
                if (!g_switched) { switchTo(g_pick); g_switched = true; }   // closed very early: switch now
                g_step = S_OPEN_BONFIRE;
            }
            break;
        }
        case S_MSG_TO_LIST:
            if (!dialogOpen()) g_step = S_OPEN_LIST;
            break;
        case S_MSG_TO_BONFIRE:
            if (!dialogOpen()) g_step = S_OPEN_BONFIRE;
            break;
        case S_OPEN_BONFIRE:
            if (!g_built) { buildBonfireList(); g_built = true; }
            if (listOpen(g_stepEv)) {
                g_step = S_IDLE;
                g_built = false;
            }
            break;
        }
    }

    // Hook 2: what the bonfire script gets for "which line was picked?"
    DWORD __stdcall onTalkResult() {
        const DWORD mm = rd32(kMenuMan);
        if (!mm) return 0xFFFFFFFF;
        const int result = (int)rd32(mm + kMmTalkResult);
        const DWORD now = GetTickCount();
        if (g_step == S_IDLE) {
            if (result == kLineIndex && g_listIsBonfire && g_listEv && rd32(mm + kMmTalkMenu) == 0) {
                g_stepEv = g_listEv;
                g_step = S_OPEN_LIST;
                g_built = false;
                g_stepTime = now;
                runStep(-1);
                return 0xFFFFFFFF;
            }
            return (DWORD)result;
        }
        if (now - g_stepTime > 2000) {                                   // the bonfire talk ended some other way
            stopSteps();
            return (DWORD)result;
        }
        g_stepTime = now;
        runStep(result);
        return 0xFFFFFFFF;                                               // the bonfire script waits meanwhile
    }

    // Hook 1, before a line is added by a talk script
    void __stdcall onBeforeAdd(DWORD ev, const Line* line) {
        if (ev != g_listEv) {                                            // another talk: a new list
            g_listEv = ev;
            g_lineCount = 0;
            g_listIsBonfire = false;
            g_lineAdded = false;
        }
        if (g_lineCount == 0) g_listIsBonfire = (line->text == kTextLeave && line->index == 9);
        // no Attune Magic in this list: our line goes before the lines that follow it
        const DWORD i = line->index;
        if (g_listIsBonfire && !g_lineAdded && (i == 7 || i == 8 || i == 10 || i == 11 || i == 12)) {
            g_lineAdded = true;
            const int current = currentCovenant();
            bool any = false;
            for (int c = 1; c <= 9 && !any; ++c) any = listed(c, current);
            if (any && g_lineCount < kMaxLines) {
                listAdd(kTextEnter, kLineIndex);
                g_lines[g_lineCount].text = kTextEnter;
                g_lines[g_lineCount].index = kLineIndex;
                ++g_lineCount;
            }
        }
    }
    // Hook 1, after a line was added
    void __stdcall onAfterAdd(DWORD ev, const Line* line) {
        if (g_lineCount < kMaxLines) {
            g_lines[g_lineCount].text = line->text;
            g_lines[g_lineCount].index = line->index;
            ++g_lineCount;
        }
        // Way of White has no "joined once" flag in the game: DSfix notes it whenever a talk list opens while you are in it
        if (currentCovenant() == 1 && !flag(kWayOfWhiteMark)) setFlag(kWayOfWhiteMark);
        if (g_listIsBonfire && !g_lineAdded && line->text == kTextAttune && line->index == 5) {
            g_lineAdded = true;
            const int current = currentCovenant();
            bool any = false;
            for (int c = 1; c <= 9 && !any; ++c) any = listed(c, current);
            if (any && g_lineCount < kMaxLines) {
                listAdd(kTextEnter, kLineIndex);
                g_lines[g_lineCount].text = kTextEnter;
                g_lines[g_lineCount].index = kLineIndex;
                ++g_lineCount;
            }
        }
    }
    // Hook 3: a talk script clears its list (it leaves the state that showed it)
    void __stdcall onScriptClear(DWORD ev) {
        if (g_step != S_IDLE && ev == g_stepEv) {
            const DWORD mm = rd32(kMenuMan);
            if (mm && (g_step == S_CONFIRM || g_step == S_CEREMONY || g_step == S_MSG_TO_LIST || g_step == S_MSG_TO_BONFIRE))
                *(DWORD*)(mm + kMmDialogKind) = 0;                       // ForceCloseGenericDialog
            stopSteps();
        }
        if (ev == g_listEv) {
            g_lineCount = 0;
            g_listIsBonfire = false;
            g_lineAdded = false;
        }
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Hooks (all checked against these bytes first; one wrong byte and nothing is patched)
    // -----------------------------------------------------------------------------------------------------------------
    //  1. AddTalkListData:        call 0x00C2BB50                          (0x00FCF7E2)
    //  2. GetTalkListEntryResult: mov ecx,[0x013786D0] / mov edx,[ecx+0xB04] (0x00D49A26)
    //  3. ClearTalkListData:      call 0x00C2B140                          (0x00FCF811)
    //  4. text lookup:            push ebx / mov ebx,[esp+8]               (0x00D175E0)
    const BYTE kExpAdd[5] = { 0xE8,0x69,0xC3,0xC5,0xFF };
    const BYTE kExpResult[12] = { 0x8B,0x0D,0xD0,0x86,0x37,0x01, 0x8B,0x91,0x04,0x0B,0x00,0x00 };
    const BYTE kExpClear[5] = { 0xE8,0x2A,0xB9,0xC5,0xFF };
    const BYTE kExpText[5] = { 0x53, 0x8B,0x5C,0x24,0x08 };
    DWORD g_fnAdd = kFnTalkListAdd;
    DWORD g_fnClear = kFnTalkListClear;
    DWORD g_addCont = 0x00FCF7E7;
    DWORD g_resultCont = 0x00D49A32;
    DWORD g_clearCont = 0x00FCF816;
    DWORD g_textCont = 0x00D175E5;
    DWORD g_menuManAddress = kMenuMan;

    // The talk command handler's frame: [esp+0x18] the talk script, [esp+0x24] the line {text, index}
    __declspec(naked) void add_stub() {
        __asm {
            pushad
            pushfd
            lea eax, [esp + 0x48]               // the line (originally [esp+0x24])
            push eax
            push dword ptr[esp + 0x40]          // the talk script (originally [esp+0x18], one push further down)
            call onBeforeAdd
            popfd
            popad
            call dword ptr[g_fnAdd]             // the call the jump replaced (eax = the line)
            pushad
            pushfd
            lea eax, [esp + 0x48]
            push eax
            push dword ptr[esp + 0x40]
            call onAfterAdd
            popfd
            popad
            jmp dword ptr[g_addCont]
        }
    }
    __declspec(naked) void result_stub() {
        __asm {
            pushad
            pushfd
            call onTalkResult
            mov dword ptr[esp + 0x18], eax      // becomes edx at POPAD
            popfd
            popad
            mov ecx, dword ptr[g_menuManAddress] // the first instruction the jump replaced (the second is the edx above)
            mov ecx, dword ptr[ecx]
            jmp dword ptr[g_resultCont]
        }
    }
    __declspec(naked) void clear_stub() {
        __asm {
            pushad
            pushfd
            push dword ptr[esp + 0x3C]          // the talk script (originally [esp+0x18])
            call onScriptClear
            popfd
            popad
            call dword ptr[g_fnClear]           // the call the jump replaced
            jmp dword ptr[g_clearCont]
        }
    }
    // edx = text id, [esp+4] the text table (the caller removes it). Our ids get our text, all others go on as before.
    __declspec(naked) void text_stub() {
        __asm {
            cmp edx, 15009901                   // our ids: kCovTextBase + 1 .. kReasonTextBase + 9
            jb normal
            cmp edx, 15009919
            ja normal
            push ecx
            push edx
            push edx
            call customText
            pop edx
            pop ecx
            test eax, eax
            jz normal
            ret
        normal:
            push ebx                            // the instructions the jump replaced
            mov ebx, dword ptr[esp + 8]
            jmp dword ptr[g_textCont]
        }
    }
}

bool installBonfireCovenants() {
    using namespace bonfirecov;
    if (bonfirecov::g_installed) return true;
    if (!Settings::get().getBonfireCovenants()) return false;

    const HookSite sites[4] = {
        { "talk list add",    0x00FCF7E2, 5,  kExpAdd },
        { "talk list result", 0x00D49A26, 12, kExpResult },
        { "talk list clear",  0x00FCF811, 5,  kExpClear },
        { "text lookup",      0x00D175E0, 5,  kExpText },
    };
    const void* stubs[4] = { (const void*)&add_stub, (const void*)&result_stub, (const void*)&clear_stub,
                             (const void*)&text_stub };
    for (int i = 0; i < 4; ++i) {
        if (memcmp((const void*)sites[i].address, sites[i].expected, sites[i].length) != 0) {
            SDLOG(0, "BonfireCovenants: the code for '%s' at 0x%08X is not what this build should have, "
                     "BonfireCovenants stays off\n", sites[i].name, sites[i].address);
            return false;
        }
    }
    bonfirecov::g_installed = true;
    for (int i = 0; i < 4; ++i) {
        if (!writeJump(sites[i].address, sites[i].length, stubs[i])) {
            SDLOG(0, "BonfireCovenants: could not write at 0x%08X, BonfireCovenants stays off\n", sites[i].address);
            return false;                       // an earlier hook stays; without our line in the list it does nothing
        }
    }
    SDLOG(0, "BonfireCovenants: on (Enter Covenant at bonfires)\n");
    return true;
}

// The fade after Yes: a black screen drawn over everything at Present (only while it runs).
namespace bonfirecov {
    IDirect3DStateBlock9* g_fadeState = NULL;    // made once, reused (nothing is allocated per frame)

    float fadeAlpha() {
        const DWORD start = g_fadeStart;
        if (!start) return 0.0f;
        const DWORD t = GetTickCount() - start;
        if (t < kFadeOutMs) return (float)t / (float)kFadeOutMs;
        if (t < kFadeOutMs + kFadeHoldMs) return 1.0f;
        if (t < kFadeOutMs + kFadeHoldMs + kFadeInMs) return 1.0f - (float)(t - kFadeOutMs - kFadeHoldMs) / (float)kFadeInMs;
        return 0.0f;
    }
}

void bonfireCovenantsDraw(IDirect3DDevice9* dev) {
    using namespace bonfirecov;
    if (!bonfirecov::g_installed || !dev || !g_fadeStart) return;
    const float a = fadeAlpha();
    if (a <= 0.0f) return;
    if (!g_fadeState && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_fadeState))) {
        g_fadeState = NULL;
        return;
    }
    IDirect3DSurface9* bb = NULL;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC desc;
    bb->GetDesc(&desc);
    IDirect3DSurface9* oldRT = NULL;
    IDirect3DSurface9* oldDS = NULL;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);
    g_fadeState->Capture();
    dev->SetRenderTarget(0, bb);
    dev->SetDepthStencilSurface(NULL);
    D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
    dev->SetViewport(&vp);
    dev->SetVertexShader(NULL);
    dev->SetPixelShader(NULL);
    dev->SetTexture(0, NULL);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0000000F);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    struct V { float x, y, z, w; DWORD c; };
    const DWORD col = ((DWORD)(a * 255.0f + 0.5f) << 24);
    const float W = (float)desc.Width - 0.5f, H = (float)desc.Height - 0.5f;
    const V v[4] = { { -0.5f, -0.5f, 0, 1, col }, { W, -0.5f, 0, 1, col }, { -0.5f, H, 0, 1, col }, { W, H, 0, 1, col } };
    if (SUCCEEDED(dev->BeginScene())) {
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
        dev->EndScene();
    }
    g_fadeState->Apply();
    if (oldRT) {
        dev->SetRenderTarget(0, oldRT);
        oldRT->Release();
    }
    dev->SetDepthStencilSurface(oldDS);
    if (oldDS) oldDS->Release();
    bb->Release();
}

void bonfireCovenantsRelease() {
    if (bonfirecov::g_fadeState) {
        bonfirecov::g_fadeState->Release();
        bonfirecov::g_fadeState = NULL;
    }
}
