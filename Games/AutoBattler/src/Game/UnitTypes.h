#pragma once

#include <stdint.h>

enum UnitType : int8_t
{
    UNIT_NONE = -1,
    UNIT_MELEE = 0,
    UNIT_RANGED,
    UNIT_TANK,
    UNIT_TYPE_COUNT
};

struct UnitStats
{
    const char* pName;
    int32_t     mSquadSide; // a squad is mSquadSide x mSquadSide units
    int32_t     mCost;
    float       mHp;
    float       mDamage;
    float       mCooldown;
    float       mRange;
    float       mSpeed;
    float       mScale;
};
extern const UnitStats gUnitStats[UNIT_TYPE_COUNT];
