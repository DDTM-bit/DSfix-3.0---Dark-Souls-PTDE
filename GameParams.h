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
