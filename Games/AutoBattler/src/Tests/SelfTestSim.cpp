#include "SelfTest.h"

#include "../Game/UnitInstance.h"
#include "../../../../Common_3/Game/ThirdParty/OpenSource/flecs/flecs.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

void testCellMath(TestContext* pCtx)
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

void testBuySell(TestContext* pCtx)
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

void testUnitCap(TestContext* pCtx)
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

void testWaveDeterministic(TestContext* pCtx)
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

static void setupMeleeSkirmish(BattleSim* s)
{
    simTestClearBoard(s);
    simTestSetCell(s, 7, 6, UNIT_MELEE);
    simTestSetCell(s, 8, 6, UNIT_MELEE);
    simTestSetCell(s, 7, 9, UNIT_MELEE);
}

void testStartBattleGuards(TestContext* pCtx)
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

void testMeleeBattleAndNextRound(TestContext* pCtx)
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

void testRangedProjectiles(TestContext* pCtx)
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

void testDefeatAndRestart(TestContext* pCtx)
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

void testTimeout(TestContext* pCtx)
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

void testCoincidentUnitsSeparate(TestContext* pCtx)
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

void testDeltaTimeClamp(TestContext* pCtx)
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

void testRestartMidBattle(TestContext* pCtx)
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

struct StressOutcome
{
    uint32_t mPlayers;
    uint32_t mEnemies;
    float    mPlayerHp;
    float    mEnemyHp;
    float    mPlayerMinDistance; // depends on every player position
    float    mEnemyMinDistance;
    float    mPlayerStartHp;
};

static StressOutcome runStressBattle(uint32_t threads)
{
    BattleSim* s = simCreate(threads, 8192);
    simStressSetup(s, 4096); // 8k units, many steering chunks
    const float startHp = simTestTotalHp(s, TEAM_PLAYER);
    stepFor(s, 4.0f); // the armies meet and fight
    const StressOutcome outcome = { simGetUnitCount(s, TEAM_PLAYER),        simGetUnitCount(s, TEAM_ENEMY),
                                    simTestTotalHp(s, TEAM_PLAYER),         simTestTotalHp(s, TEAM_ENEMY),
                                    simTestMinUnitDistance(s, TEAM_PLAYER), simTestMinUnitDistance(s, TEAM_ENEMY),
                                    startHp };
    simDestroy(s);
    return outcome;
}

void testThreadCountDoesNotChangeStress(TestContext* pCtx)
{
    const StressOutcome single = runStressBattle(1);
    const StressOutcome multi = runStressBattle(6);
    CHECK(single.mPlayers == multi.mPlayers && single.mEnemies == multi.mEnemies);
    CHECK(single.mPlayerHp == multi.mPlayerHp && single.mEnemyHp == multi.mEnemyHp);
    CHECK(single.mPlayerMinDistance == multi.mPlayerMinDistance && single.mEnemyMinDistance == multi.mEnemyMinDistance);
    CHECK(single.mPlayerHp < single.mPlayerStartHp); // the armies did fight
}

void testThreadCountDoesNotChangeOutcome(TestContext* pCtx)
{
    const BattleOutcome single = runScriptedBattle(1);
    const BattleOutcome multi = runScriptedBattle(4);
    CHECK(single.mPhase != PHASE_BATTLE);
    CHECK(single.mPhase == multi.mPhase);
    CHECK(single.mTime == multi.mTime);
    CHECK(single.mPlayers == multi.mPlayers && single.mEnemies == multi.mEnemies);
    CHECK(single.mPlayerHp == multi.mPlayerHp);
}

void testStepTimings(TestContext* pCtx)
{
    BattleSim* s = simCreate(2, 8192);
    CHECK(simStressSetup(s, 4096));
    simResetTimings(s);
    SimTimings t = {};
    simGetTimings(s, &t);
    CHECK(t.mSteps == 0u && t.mGridUs == 0 && t.mSteerUs == 0 && t.mSystemsUs == 0 && t.mAttackUs == 0 && t.mProjectileUs == 0 &&
          t.mHealthUs == 0);

    stepFor(s, 0.5f); // 30 battle steps on 8k units: every phase takes measurable time
    simGetTimings(s, &t);
    CHECK(t.mSteps == 30u);
    CHECK(t.mGridUs > 0 && t.mSteerUs > 0 && t.mSystemsUs > 0);
    CHECK(t.mSteerCpuUs > 0); // summed over the worker threads that ran SteerSystem
    CHECK(t.mAttackUs >= 0 && t.mProjectileUs >= 0 && t.mHealthUs >= 0);
    CHECK(t.mSystemsUs >= t.mAttackUs + t.mProjectileUs + t.mHealthUs); // the single-threaded systems run inside ecs_progress

    simResetTimings(s);
    simGetTimings(s, &t);
    CHECK(t.mSteps == 0u && t.mSteerUs == 0 && t.mSystemsUs == 0 && t.mSteerCpuUs == 0);
    simDestroy(s);
}

// Deleting an entity from the middle of a table must leave every other entity with its own data. The
// Forge's flecs once shifted component columns while moving only the last entity id into the hole, which
// re-paired entities and data (and made each delete O(table size)).
struct TestValue
{
    float mValue;
};

void testFlecsDeleteKeepsEntityData(TestContext* pCtx)
{
    ecs_world_t*      w = ecs_init();
    ecs_entity_desc_t entityDesc = {};
    entityDesc.name = "TestValue";
    ecs_component_desc_t componentDesc = {};
    componentDesc.entity = ecs_entity_init(w, &entityDesc);
    componentDesc.type.size = sizeof(TestValue);
    componentDesc.type.alignment = alignof(TestValue);
    const ecs_entity_t component = ecs_component_init(w, &componentDesc);

    ecs_entity_t entities[5];
    for (uint32_t i = 0; i < 5; ++i)
    {
        entities[i] = ecs_new(w);
        const TestValue value = { (float)i };
        ecs_set_id(w, entities[i], component, sizeof(value), &value);
    }
    ecs_delete(w, entities[0]); // first row: the last entity moves into it
    ecs_delete(w, entities[2]); // a middle row
    bool own = true;
    for (uint32_t i = 0; i < 5; ++i)
        if (i != 0 && i != 2)
            own = own && ((const TestValue*)ecs_get_id(w, entities[i], component))->mValue == (float)i;
    CHECK(own);
    CHECK(!ecs_is_alive(w, entities[0]) && !ecs_is_alive(w, entities[2]));
    ecs_fini(w);
}

void testRenderDataPrep(TestContext* pCtx)
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

void testRenderDataBattle(TestContext* pCtx)
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

void testAnimationState(TestContext* pCtx)
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

void testStressSetup(TestContext* pCtx)
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

void testNearestEnemySearch(TestContext* pCtx)
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
