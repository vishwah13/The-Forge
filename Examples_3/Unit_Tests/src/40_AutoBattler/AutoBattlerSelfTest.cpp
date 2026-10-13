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

static void runBattle(BattleSim* s, float seconds)
{
    const int32_t steps = (int32_t)(seconds * 60.0f);
    for (int32_t i = 0; i < steps && simGetPhase(s) == PHASE_BATTLE; ++i)
        simStep(s, 1.0f / 60.0f);
}

static void stepFor(BattleSim* s, float seconds)
{
    const int32_t steps = (int32_t)(seconds * 60.0f);
    for (int32_t i = 0; i < steps; ++i)
        simStep(s, 1.0f / 60.0f);
}

static void setupMeleeSkirmish(BattleSim* s)
{
    simTestClearBoard(s);
    simTestSetCell(s, 7, 6, UNIT_MELEE);
    simTestSetCell(s, 8, 6, UNIT_MELEE);
    simTestSetCell(s, 7, 9, UNIT_MELEE);
}

static void testStartBattleGuards(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    CHECK(!simStartBattle(s));
    CHECK(simGetPhase(s) == PHASE_PREP);
    CHECK(strcmp(simGetMessage(s), "Place at least one squad first") == 0);
    CHECK(simBuySquad(s, 0, 0, UNIT_MELEE));
    CHECK(simStartBattle(s));
    CHECK(simGetPhase(s) == PHASE_BATTLE);
    CHECK(!simStartBattle(s));
    CHECK(!simBuySquad(s, 1, 0, UNIT_MELEE));
    CHECK(strcmp(simGetMessage(s), "Squads can only be placed during prep") == 0);
    CHECK(!simSellSquad(s, 0, 0));
    simDestroy(s);
}

static void testMeleeBattleAndNextRound(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    setupMeleeSkirmish(s);
    CHECK(simStartBattle(s));
    runBattle(s, 60.0f);
    CHECK(simGetPhase(s) == PHASE_VICTORY);
    CHECK(simGetUnitCount(s, TEAM_ENEMY) == 0u);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) > 0u);
    CHECK(simGetBattleTime(s) < 60.0f);

    stepFor(s, RESULT_DURATION + 0.5f);
    CHECK(simGetPhase(s) == PHASE_PREP);
    CHECK(simGetRound(s) == 2);
    CHECK(simGetGold(s) == START_GOLD + 5 + 2);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 50u); // army restored at full strength
    CHECK(simGetCell(s, 7, 6) == UNIT_MELEE && simGetCell(s, 8, 6) == UNIT_MELEE);
    CHECK(simGetUnitCount(s, TEAM_ENEMY) > 0u); // new wave
    simDestroy(s);
}

static void testRangedProjectiles(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 8, 7, UNIT_RANGED);
    simTestSetCell(s, 8, 8, UNIT_TANK);
    const float enemyHpBefore = simTestTotalHp(s, TEAM_ENEMY);
    CHECK(enemyHpBefore == 9.0f * 250.0f);
    CHECK(simStartBattle(s));
    uint32_t maxProjectiles = 0;
    for (int32_t i = 0; i < 120 && simGetPhase(s) == PHASE_BATTLE; ++i)
    {
        simStep(s, 1.0f / 60.0f);
        if (simGetProjectileCount(s) > maxProjectiles)
            maxProjectiles = simGetProjectileCount(s);
    }
    CHECK(maxProjectiles > 0u);
    CHECK(maxProjectiles <= MAX_PROJECTILES);
    CHECK(simTestTotalHp(s, TEAM_ENEMY) < enemyHpBefore);
    simDestroy(s);
}

static void testDefeatAndRestart(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 7, 6, UNIT_MELEE);
    for (int32_t x = 3; x < 13; ++x)
        simTestSetCell(s, x, 9, UNIT_TANK);
    CHECK(simStartBattle(s));
    runBattle(s, 60.0f);
    CHECK(simGetPhase(s) == PHASE_DEFEAT);
    stepFor(s, 10.0f);
    CHECK(simGetPhase(s) == PHASE_DEFEAT); // stays until restart
    CHECK(simGetRound(s) == 1);
    simRestart(s);
    CHECK(simGetPhase(s) == PHASE_PREP && simGetRound(s) == 1 && simGetGold(s) == START_GOLD);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 0u);
    simDestroy(s);
}

static void testTimeout(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 0, 0, UNIT_MELEE);
    simTestSetCell(s, 15, 15, UNIT_TANK);
    CHECK(simStartBattle(s));
    simTestSetBattleTime(s, BATTLE_TIME_LIMIT - 0.001f);
    simStep(s, 1.0f / 60.0f);
    CHECK(simGetPhase(s) == PHASE_DEFEAT);
    CHECK(strcmp(simGetMessage(s), "Time ran out") == 0);
    simDestroy(s);
}

static void testCoincidentUnitsSeparate(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 0, 0, UNIT_MELEE);
    simTestSetCell(s, 15, 15, UNIT_MELEE);
    simTestSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, 40.0f, 20.0f);
    simTestSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, 40.0f, 20.0f);
    CHECK(simTestMinUnitDistance(s, TEAM_PLAYER) < 1e-6f);
    CHECK(simStartBattle(s));
    stepFor(s, 1.0f);
    CHECK(simTestAllPositionsFinite(s));
    CHECK(simTestMinUnitDistance(s, TEAM_PLAYER) > 0.2f);
    simDestroy(s);
}

static void testDeltaTimeClamp(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    setupMeleeSkirmish(s);
    CHECK(simStartBattle(s));
    simStep(s, 10.0f);
    CHECK(simGetBattleTime(s) <= MAX_STEP + 1e-6f);
    const float t = simGetBattleTime(s);
    simStep(s, NAN);
    simStep(s, -1.0f);
    simStep(s, 0.0f);
    CHECK(simGetBattleTime(s) == t);
    CHECK(simTestAllPositionsFinite(s));
    simDestroy(s);
}

static void testRestartMidBattle(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 8, 7, UNIT_RANGED);
    simTestSetCell(s, 8, 8, UNIT_TANK);
    CHECK(simStartBattle(s));
    stepFor(s, 0.1f);
    CHECK(simGetProjectileCount(s) > 0u);
    simRestart(s);
    CHECK(simGetPhase(s) == PHASE_PREP);
    CHECK(simGetProjectileCount(s) == 0u);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 0u);
    simDestroy(s);
}

struct BattleOutcome
{
    Phase    mPhase;
    float    mTime;
    uint32_t mPlayers;
    uint32_t mEnemies;
    float    mPlayerHp;
};

static BattleOutcome runScriptedBattle(uint32_t threads)
{
    BattleSim* s = simCreate(threads);
    simTestClearBoard(s);
    simTestSetCell(s, 7, 6, UNIT_MELEE);
    simTestSetCell(s, 8, 6, UNIT_MELEE);
    simTestSetCell(s, 8, 5, UNIT_RANGED);
    simTestSetCell(s, 7, 9, UNIT_MELEE);
    simTestSetCell(s, 8, 9, UNIT_TANK);
    simStartBattle(s);
    runBattle(s, 60.0f);
    const BattleOutcome outcome = { simGetPhase(s), simGetBattleTime(s), simGetUnitCount(s, TEAM_PLAYER), simGetUnitCount(s, TEAM_ENEMY),
                                    simTestTotalHp(s, TEAM_PLAYER) };
    simDestroy(s);
    return outcome;
}

static void testThreadCountDoesNotChangeOutcome(TestContext* pCtx)
{
    const BattleOutcome single = runScriptedBattle(1);
    const BattleOutcome multi = runScriptedBattle(4);
    CHECK(single.mPhase != PHASE_BATTLE);
    CHECK(single.mPhase == multi.mPhase);
    CHECK(single.mTime == multi.mTime);
    CHECK(single.mPlayers == multi.mPlayers && single.mEnemies == multi.mEnemies);
    CHECK(single.mPlayerHp == multi.mPlayerHp);
}

static RenderData allocRenderData()
{
    RenderData data = {};
    data.pUnits = (UnitInstance*)tf_malloc(sizeof(UnitInstance) * MAX_UNITS);
    data.pBillboards = (BillboardInstance*)tf_malloc(sizeof(BillboardInstance) * MAX_BILLBOARDS);
    return data;
}

static void freeRenderData(RenderData* pData)
{
    tf_free(pData->pUnits);
    tf_free(pData->pBillboards);
}

static void testRenderDataPrep(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 0, 0, UNIT_MELEE);
    simTestSetCell(s, 15, 15, UNIT_TANK);
    RenderData data = allocRenderData();
    simFillRenderData(s, &data);
    CHECK(data.mUnitCount == 25u + 9u);
    CHECK(data.mBillboardCount == 0u); // full health, no projectiles

    uint32_t blue = 0;
    bool     onGround = true;
    bool     noFlash = true;
    for (uint32_t i = 0; i < data.mUnitCount; ++i)
    {
        const Vector4 t = data.pUnits[i].mWorld.getCol3();
        onGround = onGround && t.getY() == 0.0f && t.getX() > 0.0f && t.getX() < ARENA_SIZE && t.getZ() > 0.0f && t.getZ() < ARENA_SIZE;
        if (data.pUnits[i].mColor.z > data.pUnits[i].mColor.x)
            ++blue;
        noFlash = noFlash && data.pUnits[i].mColor.w == 0.0f;
    }
    CHECK(onGround);
    CHECK(noFlash);
    CHECK(blue == 25u);

    // Units render at UNIT_MODEL_SCALE x their type scale (the mesh's bind pose is ~3.7 m tall).
    bool scaled = true;
    for (uint32_t i = 0; i < data.mUnitCount; ++i)
    {
        const float expected = UNIT_MODEL_SCALE * (data.pUnits[i].mColor.z > data.pUnits[i].mColor.x ? gUnitStats[UNIT_MELEE].mScale
                                                                                                       : gUnitStats[UNIT_TANK].mScale);
        scaled = scaled && fabsf(length(data.pUnits[i].mWorld.getCol0().getXYZ()) - expected) < 1e-4f;
    }
    CHECK(scaled);
    freeRenderData(&data);
    simDestroy(s);
}

static void testRenderDataBattle(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 8, 7, UNIT_RANGED);
    simTestSetCell(s, 8, 8, UNIT_TANK);
    CHECK(simStartBattle(s));
    stepFor(s, 0.75f); // first volley has landed, so at least one tank is damaged
    RenderData data = allocRenderData();
    simFillRenderData(s, &data);
    CHECK(data.mUnitCount == simGetUnitCount(s, TEAM_PLAYER) + simGetUnitCount(s, TEAM_ENEMY));
    CHECK(data.mUnitCount <= MAX_UNITS);
    CHECK(data.mBillboardCount >= simGetProjectileCount(s) + 1u); // projectiles + at least one damaged tank
    CHECK(data.mBillboardCount <= MAX_BILLBOARDS);
    bool fillInRange = true;
    for (uint32_t i = 0; i < data.mBillboardCount; ++i)
        fillInRange = fillInRange && data.pBillboards[i].mFullColorFill.w >= 0.0f && data.pBillboards[i].mFullColorFill.w <= 1.0f;
    CHECK(fillInRange);
    freeRenderData(&data);
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
        { "start battle guards", testStartBattleGuards },
        { "melee battle and next round", testMeleeBattleAndNextRound },
        { "ranged projectiles", testRangedProjectiles },
        { "defeat and restart", testDefeatAndRestart },
        { "timeout", testTimeout },
        { "coincident units separate", testCoincidentUnitsSeparate },
        { "delta time clamp", testDeltaTimeClamp },
        { "restart mid battle", testRestartMidBattle },
        { "thread count does not change outcome", testThreadCountDoesNotChangeOutcome },
        { "render data in prep", testRenderDataPrep },
        { "render data in battle", testRenderDataBattle },
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
