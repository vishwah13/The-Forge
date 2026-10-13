#include "AutoBattlerSim.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../../../Common_3/Utilities/Interfaces/IFileSystem.h"

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

struct TestContext
{
    FileStream mFile;
    int32_t    mFailures;
};

static void writeLine(TestContext* pCtx, const char* pText)
{
    fsWriteToStream(&pCtx->mFile, pText, strlen(pText));
    fsWriteToStream(&pCtx->mFile, "\n", 1);
}

static void check(TestContext* pCtx, bool ok, const char* pExpr, int32_t line)
{
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "  %s line %d: %s", ok ? "pass" : "FAIL", line, pExpr);
    writeLine(pCtx, buffer);
    if (!ok)
        ++pCtx->mFailures;
}

#define CHECK(cond) check(pCtx, (cond), #cond, __LINE__)

static void testCellMath(TestContext* pCtx)
{
    int32_t x = -1;
    int32_t z = -1;
    CHECK(worldToCell(0.0f, 0.0f, &x, &z) && x == 0 && z == 0);
    CHECK(worldToCell(79.99f, 79.99f, &x, &z) && x == 15 && z == 15);
    CHECK(worldToCell(12.5f, 37.4f, &x, &z) && x == 2 && z == 7);
    CHECK(!worldToCell(-0.01f, 10.0f, &x, &z));
    CHECK(!worldToCell(10.0f, 80.0f, &x, &z));
    CHECK(!worldToCell(NAN, 10.0f, &x, &z));

    const float2 center = cellCenter(2, 7);
    CHECK(center.x == 12.5f && center.y == 37.5f);

    CHECK(rayToCell(float3(40.0f, 50.0f, 40.0f), float3(0.0f, -1.0f, 0.0f), &x, &z) && x == 8 && z == 8);
    CHECK(rayToCell(float3(40.0f, 50.0f, -7.5f), float3(0.0f, -0.70710678f, 0.70710678f), &x, &z) && x == 8 && z == 8);
    CHECK(!rayToCell(float3(40.0f, 50.0f, 40.0f), float3(0.0f, 0.0f, 1.0f), &x, &z));  // parallel to ground
    CHECK(!rayToCell(float3(40.0f, 50.0f, 40.0f), float3(0.0f, 1.0f, 0.0f), &x, &z));  // pointing away
    CHECK(!rayToCell(float3(40.0f, 50.0f, 0.0f), float3(0.0f, -0.1f, -1.0f), &x, &z)); // lands off the arena
}

static void testBuySell(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    CHECK(simGetPhase(s) == PHASE_PREP && simGetRound(s) == 1 && simGetGold(s) == START_GOLD);
    const uint32_t enemyUnits = simGetUnitCount(s, TEAM_ENEMY);
    CHECK(enemyUnits > 0u);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 0u);

    CHECK(simBuySquad(s, 3, 2, UNIT_MELEE));
    CHECK(simGetGold(s) == START_GOLD - 2);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 25u);
    CHECK(simGetCell(s, 3, 2) == UNIT_MELEE);

    CHECK(!simBuySquad(s, 3, 2, UNIT_TANK));
    CHECK(strcmp(simGetMessage(s), "That cell already has a squad") == 0);
    CHECK(!simBuySquad(s, 3, PLAYER_ROWS, UNIT_MELEE));
    CHECK(!simBuySquad(s, -1, 0, UNIT_MELEE));
    CHECK(!simBuySquad(s, 0, 0, UNIT_NONE));

    CHECK(simBuySquad(s, 0, 0, UNIT_TANK)); // 8 -> 4
    CHECK(simBuySquad(s, 1, 0, UNIT_TANK)); // 4 -> 0
    CHECK(!simBuySquad(s, 2, 0, UNIT_MELEE));
    CHECK(strcmp(simGetMessage(s), "Not enough gold") == 0);
    CHECK(simGetGold(s) == 0);

    CHECK(simSellSquad(s, 3, 2));
    CHECK(simGetGold(s) == 2 && simGetCell(s, 3, 2) == UNIT_NONE);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 18u);
    CHECK(simGetUnitCount(s, TEAM_ENEMY) == enemyUnits);
    CHECK(!simSellSquad(s, 3, 2));
    CHECK(!simSellSquad(s, 0, PLAYER_ROWS));
    simDestroy(s);
}

static void testUnitCap(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetGold(s, 100000);
    for (int32_t z = 0; z < PLAYER_ROWS; ++z)
        for (int32_t x = 0; x < ARENA_CELLS; ++x)
            if (!(x == ARENA_CELLS - 1 && z == PLAYER_ROWS - 1))
                simBuySquad(s, x, z, UNIT_MELEE);
    for (int32_t i = 0; i < 36; ++i)
        simTestSetCell(s, i % ARENA_CELLS, PLAYER_ROWS + i / ARENA_CELLS, UNIT_MELEE);

    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 127u * 25u);
    CHECK(simGetUnitCount(s, TEAM_ENEMY) == 36u * 25u);
    CHECK(!simBuySquad(s, ARENA_CELLS - 1, PLAYER_ROWS - 1, UNIT_MELEE)); // 4075 + 25 > 4096
    CHECK(strcmp(simGetMessage(s), "Unit cap reached") == 0);
    CHECK(simBuySquad(s, ARENA_CELLS - 1, PLAYER_ROWS - 1, UNIT_RANGED)); // 4075 + 16 fits
    CHECK(!simTestSetCell(s, 0, ARENA_CELLS - 1, UNIT_MELEE));             // hook respects the cap too
    CHECK(simGetUnitCount(s, TEAM_PLAYER) + simGetUnitCount(s, TEAM_ENEMY) <= MAX_UNITS);
    simDestroy(s);
}

static void testWaveDeterministic(TestContext* pCtx)
{
    int8_t     first[ARENA_CELLS * ARENA_CELLS];
    BattleSim* s = simCreate(1);
    int32_t    cost = 0;
    int32_t    enemySquads = 0;
    bool       playerHalfEmpty = true;
    for (int32_t z = 0; z < ARENA_CELLS; ++z)
    {
        for (int32_t x = 0; x < ARENA_CELLS; ++x)
        {
            const UnitType type = simGetCell(s, x, z);
            first[z * ARENA_CELLS + x] = type;
            if (type == UNIT_NONE)
                continue;
            if (z < PLAYER_ROWS)
                playerHalfEmpty = false;
            else
            {
                cost += gUnitStats[type].mCost;
                ++enemySquads;
            }
        }
    }
    CHECK(enemySquads > 0);
    CHECK(cost <= 6 + 4 * 1);
    CHECK(playerHalfEmpty);
    simDestroy(s);

    s = simCreate(4);
    bool same = true;
    for (int32_t z = 0; z < ARENA_CELLS; ++z)
        for (int32_t x = 0; x < ARENA_CELLS; ++x)
            same = same && first[z * ARENA_CELLS + x] == simGetCell(s, x, z);
    CHECK(same);
    simDestroy(s);
}

typedef void (*TestFn)(TestContext*);

struct NamedTest
{
    const char* pName;
    TestFn      pFn;
};

int runAutoBattlerSelfTest(const char* pResultFileName)
{
    TestContext ctx = {};
    if (!fsOpenStreamFromPath(RD_LOG, pResultFileName, FM_WRITE, &ctx.mFile))
        return 1;

    const NamedTest tests[] = {
        { "cell math", testCellMath },
        { "buy and sell", testBuySell },
        { "unit cap", testUnitCap },
        { "wave is deterministic", testWaveDeterministic },
    };
    for (uint32_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    {
        writeLine(&ctx, tests[i].pName);
        tests[i].pFn(&ctx);
    }

    char summary[64];
    snprintf(summary, sizeof(summary), "RESULT: %d failure(s)", ctx.mFailures);
    writeLine(&ctx, summary);
    fsCloseStream(&ctx.mFile);
    return ctx.mFailures;
}
