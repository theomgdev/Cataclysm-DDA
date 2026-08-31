#pragma once
#ifndef CATA_SRC_NPC_GEAR_UP_H
#define CATA_SRC_NPC_GEAR_UP_H

class Character;

// "Gear up from the stores" as a Character-level order: the avatar starts it
// from the zone-activities menu, NPCs through talk_function::gear_up_from_stores.
// Implemented in npc_gear_up.cpp.

// Is there any loot or camp storage zone in range to draw from at all?
bool gear_up_stores_available( Character &who );

// Reset the sweep state and assign the gear-up activity.
void start_gear_up_from_stores( Character &who );

#endif // CATA_SRC_NPC_GEAR_UP_H
