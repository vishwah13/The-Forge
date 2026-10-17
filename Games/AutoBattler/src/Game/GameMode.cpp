#include "GameMode.h"
#include "SimInternal.h"

#include <math.h>

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

static const float SANDBOX_SPACING = 1.0f;  // one metre between soldiers, like a melee block
static const float SANDBOX_GAP = 10.0f;     // between the two armies' front rows
static const float SANDBOX_MARGIN = 5.0f;   // between the armies and the world edge
static const float SANDBOX_MIN_WORLD = 80.0f;

// Army of unitsPerTeam at SANDBOX_SPACING: cols x rows per team, both facing across SANDBOX_GAP on a square world.
static void sandboxLayout(uint32_t unitsPerTeam, uint32_t* pCols, uint32_t* pRows, float* pWorldSize)
{
    const uint32_t cols = (uint32_t)ceilf(sqrtf(2.0f * (float)unitsPerTeam)); // twice as wide as deep
    const uint32_t rows = cols > 0 ? (unitsPerTeam + cols - 1) / cols : 0;
    const float    armies = fmaxf((float)cols * SANDBOX_SPACING, 2.0f * (float)rows * SANDBOX_SPACING + SANDBOX_GAP);
    *pCols = cols;
    *pRows = rows;
    *pWorldSize = fmaxf(SANDBOX_MIN_WORLD, armies + 2.0f * SANDBOX_MARGIN);
}

bool gameModeSandboxStart(GameMode* pMode, BattleSim* pSim, uint32_t unitsPerTeam)
{
    uint32_t cols = 0;
    uint32_t rows = 0;
    float    worldSize = 0.0f;
    if (unitsPerTeam == 0 || 2ull * unitsPerTeam > simGetUnitCapacity(pSim))
        return false;
    sandboxLayout(unitsPerTeam, &cols, &rows, &worldSize);
    if (worldSize > MAX_WORLD_SIZE)
        return false;

    simClear(pSim, worldSize);
    // Row 0 is each army's front: melee in front, tanks behind them, ranged at the back.
    const float left = 0.5f * (worldSize - (float)cols * SANDBOX_SPACING);
    const float mid = 0.5f * worldSize;
    for (uint32_t k = 0; k < unitsPerTeam; ++k)
    {
        const uint32_t col = k % cols;
        const uint32_t row = k / cols;
        const UnitType type = row * 10 < rows * 6 ? UNIT_MELEE : (row * 10 < rows * 8 ? UNIT_TANK : UNIT_RANGED);
        const float    x = left + ((float)col + 0.5f) * SANDBOX_SPACING;
        const float    depth = 0.5f * SANDBOX_GAP + ((float)row + 0.5f) * SANDBOX_SPACING;
        spawnUnit(pSim, TEAM_PLAYER, type, x, mid - depth);
        spawnUnit(pSim, TEAM_ENEMY, type, x, mid + depth);
    }
    pMode->pSim = pSim;
    pMode->mWinner = -1;
    pMode->mUnitsPerTeam = unitsPerTeam;
    return true;
}

void gameModeUpdate(GameMode* pMode, float deltaTime)
{
    if (!pMode->pSim || !(deltaTime > 0.0f)) // also rejects NaN
        return;
    simStep(pMode->pSim, deltaTime);
    if (pMode->mWinner >= 0)
        return;
    const uint32_t players = simGetUnitCount(pMode->pSim, TEAM_PLAYER);
    const uint32_t enemies = simGetUnitCount(pMode->pSim, TEAM_ENEMY);
    if (players == 0 || enemies == 0)
    {
        pMode->mWinner = players > 0 ? TEAM_PLAYER : TEAM_ENEMY;
        simSetCelebratingTeam(pMode->pSim, pMode->mWinner);
    }
}

int32_t gameModeGetWinner(const GameMode* pMode) { return pMode->mWinner; }
