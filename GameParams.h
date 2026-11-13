#pragma once

#include <cstdint>

// The game's parameter tables (research: the Dark Souls Overhaul team, metal-crow & Sean Pesce, for the table
// headers and layouts). The game keeps every .param file of GameParam.parambnd in memory for the whole session.
// A background thread finds the two tables DSfix needs by their header name, checks that each really is that table,
// and then:
//  * disableItemAutoEquip: clears the "equip automatically on pickup" flag of every item except the Estus Flasks;
//  * durabilityBars: keeps the weapon table for the maximum durability of the equipped weapons.
// It only starts when one of the two settings is on; with both off nothing is searched or changed.
void startGameParamsThread();

// Maximum durability of a weapon (the id as stored in the equipment slot, upgrade level included).
// Returns false while the weapon table has not been found yet, or if the weapon is not in it.
bool weaponMaxDurability(uint32_t weaponId, uint32_t* maxDurability);

// WeaponsArmorSort: sorting of the Weapons and Armor lists (Inventory, Equipment "select item", Bottomless Box).
// Hold RS + D-pad left / right / up / down (or LALT + arrows) to pick the value and the order, release to sort. Nothing is patched
// when the setting is off.
bool installWeaponsArmorSort();
bool weaponsArmorSortActive();

// What the sort prompt shows. False when no sortable list has focus (then nothing is drawn).
struct SortPromptInfo {
    int window;       // 1 Inventory / Equipment (prompt on the help bar), 2 Bottomless Box (prompt in the header)
    int kind;         // 1 weapons, 2 armor, 3 every other item list
    int choice;       // 0 game order, 1..4 physical / magic / fire / lightning, 5 durability, 6 weight, 7 storage (box),
                      // 8 most recent
    int desc;         // 1 DESC, 0 ASC
    int held;         // 0 nothing held, 1 RS held (controller prompt), 2 LALT held (keyboard prompt)
    int ownKeyboard;  // without DDTM: 1 = keyboard / mouse used last
};
bool weaponsArmorSortPrompt(SortPromptInfo* out);

// BottomlessBoxMarkers: in the Bottomless Box list, a small chest picture on the rows with nothing held and some stored.
// Nothing is patched when the setting is off.
bool installBottomlessBoxMarkers();
bool bottomlessBoxMarkersActive();
// The rows of the box list on screen now, from the top: flags[k] = 1 when that row gets the marker. Returns how many rows
// were filled (0 = the box list is not on screen).
int bottomlessBoxMarkerRows(unsigned char* flags, int max);

// QuickSelectEstus: during play, holding D-pad Down (350 ms) selects the Estus Flask in the quick-item slots, with the
// game's own animation and sound. A tap switches as before. Nothing is patched when the setting is off.
bool installQuickSelectEstus();

// Item list wheel (no setting, with DDTMControllerFix): in the item lists (Inventory, Bottomless Box, Equipment, shops)
// a wheel notch selects the first item below / above the visible rows, so the list scrolls row by row. Installed only
// when DDTMControllerFix is set up; it acts only once that DLL is loaded. Elsewhere the wheel is the game's own.
bool installListWheelScroll();

// gameMap (DSfix.ini gameMap): the in-game map with the gesture window. All of it is in GameMap.h (included by
// RenderstateManager.cpp); installGameMap is called from main.cpp. Nothing is patched when the setting is off.
bool installGameMap();
// for gameMap: puts its controller and mouse wheel callbacks on the shared XInput and wheel hooks (installs them if needed)
bool installGameMapInput(void(__stdcall* pad)(unsigned long idx, void* state), bool(__stdcall* wheel)(int delta));
// WeaponsArmorSort, "Most recent": notes what the hero gains, equips and attunes (called every frame from
// RSManager::redirectPresent; it works 4 times a second, and does nothing when WeaponsArmorSort is off)
void wasortFrameTick();
