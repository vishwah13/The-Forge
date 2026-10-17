#pragma once

#include "Sim.h"

// The game's rules. Sandbox (the only ruleset so far) has no king and no economy: two armies fight until
// one is gone, and the winner celebrates. Campaign rules arrive in conquest plan 3.
struct GameMode
{
    BattleSim* pSim;
    int32_t    mWinner; // Team that won, or -1 while both armies stand (or before a start). If both armies
                        // die in the same step the enemy wins, with nobody left to dance.
    uint32_t   mUnitsPerTeam;
};

// Clears the world and spawns two facing armies of unitsPerTeam (melee in front, tanks, then ranged) on a
// world sized to hold them. Refused, leaving the sim untouched, if both armies exceed the unit capacity
// or the world would exceed MAX_WORLD_SIZE.
bool gameModeSandboxStart(GameMode* pMode, BattleSim* pSim, uint32_t unitsPerTeam);
// Steps the sim (same dt rules as simStep) and declares the winner once one army is gone.
void    gameModeUpdate(GameMode* pMode, float deltaTime);
int32_t gameModeGetWinner(const GameMode* pMode);
