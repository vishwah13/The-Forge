#include "UnitTypes.h"

const UnitStats gUnitStats[UNIT_TYPE_COUNT] = {
    // name     hp      damage cooldown range speed scale
    { "Melee", 100.0f, 15.0f, 0.8f, 1.2f, 3.0f, 1.0f },
    { "Ranged", 60.0f, 12.0f, 1.0f, 8.0f, 2.5f, 0.9f },
    { "Tank", 250.0f, 8.0f, 1.2f, 1.4f, 1.8f, 1.3f },
};
