#include "AutoBattlerSim.h"
#include "UnitCulling.h"
#include "UnitInstance.h"
#include "UnitLod.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../../Common_3/Utilities/Interfaces/IFileSystem.h"

#include "../../../Common_3/Utilities/Interfaces/IMemory.h"

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

static RenderData allocRenderData(uint32_t unitCapacity = MAX_UNITS)
{
    RenderData data = {};
    data.pUnits = (UnitInstance*)tf_malloc(sizeof(UnitInstance) * unitCapacity);
    data.pBillboards = (BillboardInstance*)tf_malloc(sizeof(BillboardInstance) * (unitCapacity + MAX_PROJECTILES));
    data.pUnitAnims = (UnitAnim*)tf_malloc(sizeof(UnitAnim) * unitCapacity);
    return data;
}

static void freeRenderData(RenderData* pData)
{
    tf_free(pData->pUnits);
    tf_free(pData->pBillboards);
    tf_free(pData->pUnitAnims);
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
        const float4 t(data.pUnits[i].mX, 0.0f, data.pUnits[i].mZ, 1.0f);
        onGround = onGround && t.y == 0.0f && t.x > 0.0f && t.x < ARENA_SIZE && t.z > 0.0f && t.z < ARENA_SIZE;
        if (unitTeam(data.pUnits[i]) == TEAM_PLAYER)
            ++blue;
        noFlash = noFlash && unitFlash(data.pUnits[i]) == 0.0f;
    }
    CHECK(onGround);
    CHECK(noFlash);
    CHECK(blue == 25u);

    // Units render at UNIT_MODEL_SCALE x their type scale.
    bool scaled = true;
    for (uint32_t i = 0; i < data.mUnitCount; ++i)
    {
        const float expected =
            UNIT_MODEL_SCALE * (unitTeam(data.pUnits[i]) == TEAM_PLAYER ? gUnitStats[UNIT_MELEE].mScale : gUnitStats[UNIT_TANK].mScale);
        scaled = scaled && fabsf(data.pUnits[i].mScale - expected) < 1e-4f;
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

static void testUnitInstancePacking(TestContext* pCtx)
{
    CHECK(sizeof(UnitInstance) == 24u);

    // World matrix rebuilt from the packed fields equals the one the sim used to send.
    const float yaws[] = { 0.0f, 0.7f, -2.5f, PI };
    bool        same = true;
    for (uint32_t i = 0; i < TF_ARRAY_COUNT(yaws); ++i)
    {
        UnitInstance u = {};
        u.mX = 12.5f;
        u.mZ = -3.0f;
        u.mYaw = yaws[i];
        u.mScale = 1.3f;
        const mat4 expected = mat4::translation(vec3(12.5f, 0.0f, -3.0f)) * mat4::rotationY(yaws[i]) * mat4::scale(vec3(1.3f));
        const mat4 actual = unitWorldMatrix(u);
        for (int c = 0; c < 4; ++c)
            same = same && (float)length(actual[c] - expected[c]) < 1e-5f;
    }
    CHECK(same);

    UnitInstance u = {};
    u.mTintFlash = packTintFlash(TEAM_ENEMY, 1.0f);
    CHECK(unitTeam(u) == TEAM_ENEMY && unitFlash(u) == 1.0f);
    u.mTintFlash = packTintFlash(TEAM_PLAYER, 0.0f);
    CHECK(unitTeam(u) == TEAM_PLAYER && unitFlash(u) == 0.0f);
    u.mTintFlash = packTintFlash(TEAM_PLAYER, 0.5f);
    CHECK(fabsf(unitFlash(u) - 0.5f) < 1.0f / 255.0f);

    const uint32_t pose = packUnitPose((1u << 24) - 1u, 0.25f);
    CHECK(unitPoseOffset(pose) == (1u << 24) - 1u);
    CHECK(fabsf(unitPoseBlend(pose) - 0.25f) < 1.0f / 255.0f);
    CHECK(unitPoseBlend(packUnitPose(7u, 0.0f)) == 0.0f && unitPoseOffset(packUnitPose(7u, 0.0f)) == 7u);
}

static uint32_t countClip(const RenderData& data, AnimClip clip, bool blueOnly)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < data.mUnitCount; ++i)
    {
        const bool blue = unitTeam(data.pUnits[i]) == TEAM_PLAYER;
        if (data.pUnitAnims[i].mClip == clip && (!blueOnly || blue))
            ++n;
    }
    return n;
}

static void testAnimationState(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    setupMeleeSkirmish(s);
    RenderData data = allocRenderData();

    // Prep: everyone idles, clocks run and are not in lockstep.
    simFillRenderData(s, &data);
    CHECK(countClip(data, ANIM_IDLE, false) == data.mUnitCount);
    const float t0 = data.pUnitAnims[0].mTime;
    bool        offsets = false;
    for (uint32_t i = 1; i < data.mUnitCount; ++i)
        offsets = offsets || data.pUnitAnims[i].mTime != t0;
    CHECK(offsets);
    simStep(s, 0.5f / 15.0f);
    simFillRenderData(s, &data);
    CHECK(data.pUnitAnims[0].mTime > t0);

    // Battle: the armies start 10+ m apart, so units walk towards each other.
    CHECK(simStartBattle(s));
    stepFor(s, 0.5f);
    simFillRenderData(s, &data);
    CHECK(countClip(data, ANIM_WALK, false) > 0u);

    // Victory: the surviving player units dance.
    runBattle(s, 60.0f);
    CHECK(simGetPhase(s) == PHASE_VICTORY);
    simFillRenderData(s, &data);
    CHECK(data.mUnitCount > 0u && countClip(data, ANIM_DANCE, false) == data.mUnitCount);
    simDestroy(s);

    // Timeout loss: both armies are alive, only the enemy (winner) dances.
    s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 0, 0, UNIT_MELEE);
    simTestSetCell(s, 15, 15, UNIT_TANK);
    CHECK(simStartBattle(s));
    simTestSetBattleTime(s, BATTLE_TIME_LIMIT - 0.001f);
    simStep(s, 1.0f / 60.0f);
    CHECK(simGetPhase(s) == PHASE_DEFEAT);
    simFillRenderData(s, &data);
    CHECK(countClip(data, ANIM_DANCE, true) == 0u);
    CHECK(countClip(data, ANIM_DANCE, false) == 9u);
    simDestroy(s);

    // Units in range of their target play the attack clip; the one unit out of range walks.
    s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 0, 0, UNIT_MELEE);              // 5 x 5 player squad, z 0.5 .. 4.5
    simTestSpawnUnit(s, TEAM_ENEMY, UNIT_TANK, 2.5f, 5.5f); // touching the squad's front row
    CHECK(simStartBattle(s));
    stepFor(s, 0.2f);
    simFillRenderData(s, &data);
    CHECK(countClip(data, ANIM_ATTACK, false) >= 2u); // the tank and at least one front-row unit
    CHECK(countClip(data, ANIM_WALK, false) > 0u);    // back rows close in
    freeRenderData(&data);
    simDestroy(s);
}

static bool allInsideWorld(const RenderData& data, float worldSize, float* pMaxX)
{
    bool inside = true;
    *pMaxX = 0.0f;
    for (uint32_t i = 0; i < data.mUnitCount; ++i)
    {
        const float4 t(data.pUnits[i].mX, 0.0f, data.pUnits[i].mZ, 1.0f);
        inside = inside && t.x > 0.0f && t.x < worldSize && t.z > 0.0f && t.z < worldSize;
        *pMaxX = t.x > *pMaxX ? t.x : *pMaxX;
    }
    return inside;
}

static void testStressSetup(TestContext* pCtx)
{
    const uint32_t perTeam = 4096;
    BattleSim*     s = simCreate(1, 2 * perTeam);
    CHECK(simGetUnitCapacity(s) == 2 * perTeam);
    CHECK(simGetWorldSize(s) == ARENA_SIZE);
    CHECK(!simStressSetup(s, perTeam + 1)); // over capacity
    CHECK(simGetPhase(s) == PHASE_PREP);

    CHECK(simStressSetup(s, perTeam));
    CHECK(simGetPhase(s) == PHASE_BATTLE);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == perTeam && simGetUnitCount(s, TEAM_ENEMY) == perTeam);
    const float world = simGetWorldSize(s);
    CHECK(world > ARENA_SIZE);                              // 8k units do not fit the 80 m arena at normal spacing
    CHECK(simTestMinUnitDistance(s, TEAM_PLAYER) >= 0.99f); // same 1 m spacing as a melee squad

    RenderData data = allocRenderData(2 * perTeam);
    float      maxX = 0.0f;
    simFillRenderData(s, &data);
    CHECK(data.mUnitCount == 2 * perTeam);
    CHECK(allInsideWorld(data, world, &maxX));
    CHECK(maxX > ARENA_SIZE);

    // Units move freely on the bigger world instead of being clamped to the arena.
    stepFor(s, 0.5f);
    CHECK(simGetPhase(s) == PHASE_BATTLE);
    CHECK(simTestAllPositionsFinite(s));
    simFillRenderData(s, &data);
    CHECK(allInsideWorld(data, world, &maxX));
    CHECK(maxX > ARENA_SIZE);

    // Restart returns to the normal game on the 80 m arena.
    simRestart(s);
    CHECK(simGetWorldSize(s) == ARENA_SIZE);
    CHECK(simGetPhase(s) == PHASE_PREP && simGetUnitCount(s, TEAM_PLAYER) == 0u);
    CHECK(simBuySquad(s, 0, 0, UNIT_MELEE) && simStartBattle(s));
    freeRenderData(&data);
    simDestroy(s);
}

// Feet of a unit whose bounding sphere centre is at (x, y, z) in world space.
static float3 unitAt(float x, float y, float z, float scale) { return float3(x, y - UNIT_BOUND_CENTER_Y * scale, z); }

static void testUnitCulling(TestContext* pCtx)
{
    // Camera at z = -50 looking down +z, so a point's depth is z + 50 and the right frustum edge at
    // depth d is x = d * tan(fovX / 2). Same projection as the app.
    const float zNear = 0.5f;
    const float zFar = 2000.0f;
    const float tanHalfX = tanf(PI / 6.0f);
    const float aspectInverse = 9.0f / 16.0f;
    const mat4  view = mat4::lookAtLH(Point3(0.0f, 0.0f, -50.0f), Point3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));
    const mat4  proj = mat4::perspectiveLH_ReverseZ(PI / 3.0f, aspectInverse, zNear, zFar);
    float4      planes[6];
    frustumPlanesFromViewProj(proj * view, planes);

    const float s = UNIT_MODEL_SCALE * gUnitStats[UNIT_MELEE].mScale; // melee instance scale
    const float r = UNIT_BOUND_RADIUS * s;                            // world radius
    const float edgeX = 50.0f * tanHalfX;     // right edge at z = 0
    const float edgeY = edgeX * aspectInverse; // top edge at z = 0
    const float c30 = cosf(PI / 6.0f);        // a sphere at edge + o is o * c30 from the side plane
    CHECK(r > 1.0f * c30 && r < 2.0f * c30);  // so +1 m straddles the edge and +2 m is fully outside

    CHECK(unitInFrustum(planes, unitAt(0.0f, 0.0f, 0.0f, s), s));
    CHECK(unitInFrustum(planes, unitAt(edgeX + 1.0f, 0.0f, 0.0f, s), s));   // straddles the right edge
    CHECK(!unitInFrustum(planes, unitAt(edgeX + 2.0f, 0.0f, 0.0f, s), s));  // right of it
    CHECK(unitInFrustum(planes, unitAt(-edgeX - 1.0f, 0.0f, 0.0f, s), s));  // straddles the left edge
    CHECK(!unitInFrustum(planes, unitAt(-edgeX - 2.0f, 0.0f, 0.0f, s), s)); // left of it
    CHECK(!unitInFrustum(planes, unitAt(0.0f, edgeY + 2.0f / cosf(atanf(tanHalfX * aspectInverse)), 0.0f, s), s)); // above
    CHECK(!unitInFrustum(planes, unitAt(0.0f, -edgeY - 2.0f / cosf(atanf(tanHalfX * aspectInverse)), 0.0f, s), s)); // below
    CHECK(!unitInFrustum(planes, unitAt(0.0f, 0.0f, -60.0f, s), s));                       // behind the camera
    CHECK(unitInFrustum(planes, unitAt(0.0f, 0.0f, -50.0f + zNear - 1.0f, s), s));          // straddles the near plane
    // The reverse-Z matrix stores depth as 1 - f/(f-n) in floats, which places the far plane up to ~1 m
    // off at 2 km (the GPU clips with the same matrix), so these two use wider margins.
    CHECK(unitInFrustum(planes, unitAt(0.0f, 0.0f, -50.0f + zFar + 0.3f, s), s));           // straddles the far plane
    CHECK(!unitInFrustum(planes, unitAt(0.0f, 0.0f, -50.0f + zFar + 3.0f, s), s));          // beyond it
    CHECK(unitInFrustum(planes, unitAt(edgeX + 2.0f, 0.0f, 0.0f, 2.0f * s), 2.0f * s));          // a bigger unit still reaches in
}

static void testUnitLodSelection(TestContext* pCtx)
{
    // Same camera as testUnitCulling: at z = -50 looking down +z.
    const float3 eye(0.0f, 0.0f, -50.0f);
    const mat4   view = mat4::lookAtLH(Point3(f3Tov3(eye)), Point3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));
    const mat4   proj = mat4::perspectiveLH_ReverseZ(PI / 3.0f, 9.0f / 16.0f, 0.5f, 2000.0f);
    float4       planes[6];
    frustumPlanesFromViewProj(proj * view, planes);

    const float s = 0.5f;
    const float r = UNIT_BOUND_RADIUS * s;
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z + 10.0f * r, s), s) == 0);
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z + (UNIT_LOD_DISTANCES[0] - 1.0f) * r, s), s) == 0);
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z + (UNIT_LOD_DISTANCES[0] + 1.0f) * r, s), s) == 1);
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z + (UNIT_LOD_DISTANCES[1] - 1.0f) * r, s), s) == 1);
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z + (UNIT_LOD_DISTANCES[1] + 1.0f) * r, s), s) == 2);
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z + 1000.0f, s), s) == 2);
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z - 10.0f, s), s) == -1); // behind the camera
    // Distances are in bounding radii, so a bigger unit keeps detail further out.
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z + (UNIT_LOD_DISTANCES[0] + 1.0f) * r, 2.0f * s), 2.0f * s) == 0);
}

// UV sphere with a duplicated seam column, like a textured mesh.
static void makeSphere(uint32_t rings, uint32_t segments, float** ppPositions, uint32_t** ppIndices, uint32_t* pVertexCount,
                       uint32_t* pIndexCount)
{
    *pVertexCount = (rings + 1) * (segments + 1);
    *pIndexCount = rings * segments * 6;
    float*    positions = (float*)tf_malloc(sizeof(float) * 3 * *pVertexCount);
    uint32_t* indices = (uint32_t*)tf_malloc(sizeof(uint32_t) * *pIndexCount);
    for (uint32_t ring = 0; ring <= rings; ++ring)
        for (uint32_t seg = 0; seg <= segments; ++seg)
        {
            const float theta = PI * (float)ring / (float)rings;
            const float phi = 2.0f * PI * (float)seg / (float)segments;
            float*      p = &positions[3 * (ring * (segments + 1) + seg)];
            p[0] = sinf(theta) * cosf(phi);
            p[1] = cosf(theta);
            p[2] = sinf(theta) * sinf(phi);
        }
    uint32_t n = 0;
    for (uint32_t ring = 0; ring < rings; ++ring)
        for (uint32_t seg = 0; seg < segments; ++seg)
        {
            const uint32_t a = ring * (segments + 1) + seg;
            const uint32_t b = a + segments + 1;
            const uint32_t quad[6] = { a, b, a + 1, a + 1, b, b + 1 };
            for (uint32_t k = 0; k < 6; ++k)
                indices[n++] = quad[k];
        }
    *ppPositions = positions;
    *ppIndices = indices;
}

static void testUnitLodBuild(TestContext* pCtx)
{
    float*    positions = NULL;
    uint32_t* indices = NULL;
    uint32_t  vertexCount = 0;
    uint32_t  indexCount = 0;
    makeSphere(48, 96, &positions, &indices, &vertexCount, &indexCount);

    UnitLods lods = {};
    CHECK(!buildUnitLods(indices, 0, positions, vertexCount, 3 * sizeof(float), &lods)); // nothing to simplify
    CHECK(buildUnitLods(indices, indexCount, positions, vertexCount, 3 * sizeof(float), &lods));

    // LOD0 is the mesh unchanged; each further level is a clearly smaller, non-empty triangle list.
    CHECK(lods.mFirstIndex[0] == 0 && lods.mIndexCount[0] == indexCount);
    CHECK(lods.pIndices && memcmp(lods.pIndices, indices, sizeof(uint32_t) * indexCount) == 0);
    CHECK(lods.mIndexCount[1] > 0 && lods.mIndexCount[1] * 2 < lods.mIndexCount[0]);
    CHECK(lods.mIndexCount[2] > 0 && lods.mIndexCount[2] * 2 < lods.mIndexCount[1]);

    // Levels are packed back to back, whole triangles, and only reference the shared vertices.
    bool     packed = true;
    uint32_t total = 0;
    for (uint32_t lod = 0; lod < UNIT_LOD_COUNT; ++lod)
    {
        packed = packed && lods.mFirstIndex[lod] == total && lods.mIndexCount[lod] % 3 == 0;
        total += lods.mIndexCount[lod];
    }
    CHECK(packed && lods.mTotalIndexCount == total);
    bool inRange = true;
    for (uint32_t i = 0; i < lods.mTotalIndexCount; ++i)
        inRange = inRange && lods.pIndices[i] < vertexCount;
    CHECK(inRange);

    freeUnitLods(&lods);
    CHECK(lods.pIndices == NULL);
    tf_free(positions);
    tf_free(indices);
}

static void testNearestEnemySearch(TestContext* pCtx)
{
    // Two 4,096-unit armies 10 m apart, 46 rows deep: back-row units are far from any enemy.
    BattleSim* s = simCreate(1, 8192);
    CHECK(simStressSetup(s, 4096));
    SimSearchStats stats = simTestNearestEnemySearch(s);
    CHECK(stats.mSearches == 8192u);
    CHECK(stats.mMismatches == 0u);
    // Searching only the rings that can hold an enemy keeps the work near the front.
    CHECK(stats.mAverageBucketsVisited < 300.0f);
    char line[96];
    snprintf(line, sizeof(line), "  average buckets visited per search: %.1f", stats.mAverageBucketsVisited);
    writeLine(pCtx, line);

    // Scattered units, so searches start at many different distances from the enemy.
    simTestClearBoard(s);
    uint32_t state = 12345u;
    for (uint32_t i = 0; i < 600; ++i)
    {
        state = state * 1664525u + 1013904223u;
        const float x = 1.0f + (float)((state >> 8) % 7800u) / 100.0f;
        state = state * 1664525u + 1013904223u;
        const float z = 1.0f + (float)((state >> 8) % 7800u) / 100.0f;
        simTestSpawnUnit(s, i % 7 == 0 ? TEAM_ENEMY : TEAM_PLAYER, UNIT_MELEE, x, z);
    }
    stats = simTestNearestEnemySearch(s);
    CHECK(stats.mSearches == 600u);
    CHECK(stats.mMismatches == 0u);
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
        { "unit instance packing", testUnitInstancePacking },
        { "animation state", testAnimationState },
        { "stress setup", testStressSetup },
        { "unit culling", testUnitCulling },
        { "unit LOD selection", testUnitLodSelection },
        { "unit LOD build", testUnitLodBuild },
        { "nearest enemy search", testNearestEnemySearch },
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
