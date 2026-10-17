#include "SelfTest.h"

#include "../Game/GameMode.h"

#include "../Game/UnitInstance.h"
#include "../../../../Common_3/Game/ThirdParty/OpenSource/flecs/flecs.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"
// The old board's squad layout: a side x side block filling the 5 m cell (cellX, cellZ) of the default 80 m
// world, so the scenarios below keep their positions.
static void spawnBlock(BattleSim* s, Team team, UnitType type, int32_t cellX, int32_t cellZ)
{
    const int32_t side = type == UNIT_MELEE ? 5 : (type == UNIT_RANGED ? 4 : 3);
    const float   spacing = 5.0f / (float)side;
    for (int32_t j = 0; j < side; ++j)
        for (int32_t i = 0; i < side; ++i)
            simSpawnUnit(s, team, type, (float)cellX * 5.0f + ((float)i + 0.5f) * spacing, (float)cellZ * 5.0f + ((float)j + 0.5f) * spacing);
}

// Two player melee blocks facing one enemy melee block 10 m away.
static void setupMeleeSkirmish(BattleSim* s)
{
    simClear(s, 80.0f);
    spawnBlock(s, TEAM_PLAYER, UNIT_MELEE, 7, 6);
    spawnBlock(s, TEAM_PLAYER, UNIT_MELEE, 8, 6);
    spawnBlock(s, TEAM_ENEMY, UNIT_MELEE, 7, 9);
}
void testRangedProjectiles(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    spawnBlock(s, TEAM_PLAYER, UNIT_RANGED, 8, 7);
    spawnBlock(s, TEAM_ENEMY, UNIT_TANK, 8, 8);
    const float enemyHpBefore = simTestTotalHp(s, TEAM_ENEMY);
    CHECK(enemyHpBefore == 9.0f * 250.0f);
    uint32_t maxProjectiles = 0;
    for (int32_t i = 0; i < 120 && simGetUnitCount(s, TEAM_ENEMY) > 0u; ++i)
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
void testCoincidentUnitsSeparate(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simSpawnUnit(s, TEAM_ENEMY, UNIT_MELEE, 75.0f, 75.0f); // far away: something to walk towards
    simSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, 40.0f, 20.0f);
    simSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, 40.0f, 20.0f);
    CHECK(simTestMinUnitDistance(s, TEAM_PLAYER) < 1e-6f);
    stepFor(s, 1.0f);
    CHECK(simTestAllPositionsFinite(s));
    CHECK(simTestMinUnitDistance(s, TEAM_PLAYER) > 0.2f);
    simDestroy(s);
}

void testDeltaTimeClamp(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    setupMeleeSkirmish(s);
    simStep(s, 10.0f);
    CHECK(simGetTime(s) <= MAX_STEP + 1e-6f);
    const float t = simGetTime(s);
    simStep(s, NAN);
    simStep(s, -1.0f);
    simStep(s, 0.0f);
    CHECK(simGetTime(s) == t);
    CHECK(simTestAllPositionsFinite(s));
    simDestroy(s);
}
struct BattleOutcome
{
    float    mTime;
    uint32_t mPlayers;
    uint32_t mEnemies;
    float    mPlayerHp;
};
static BattleOutcome runScriptedBattle(uint32_t threads)
{
    BattleSim* s = simCreate(threads);
    spawnBlock(s, TEAM_PLAYER, UNIT_MELEE, 7, 6);
    spawnBlock(s, TEAM_PLAYER, UNIT_MELEE, 8, 6);
    spawnBlock(s, TEAM_PLAYER, UNIT_RANGED, 8, 5);
    spawnBlock(s, TEAM_ENEMY, UNIT_MELEE, 7, 9);
    spawnBlock(s, TEAM_ENEMY, UNIT_TANK, 8, 9);
    runBattle(s, 60.0f);
    const BattleOutcome outcome = { simGetTime(s), simGetUnitCount(s, TEAM_PLAYER), simGetUnitCount(s, TEAM_ENEMY),
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
    GameMode   mode = {};
    gameModeSandboxStart(&mode, s, 4096); // 8k units, many steering chunks
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
    CHECK(single.mPlayers == 0u || single.mEnemies == 0u); // the battle finished
    CHECK(single.mTime == multi.mTime);
    CHECK(single.mPlayers == multi.mPlayers && single.mEnemies == multi.mEnemies);
    CHECK(single.mPlayerHp == multi.mPlayerHp);
}

void testStepTimings(TestContext* pCtx)
{
    BattleSim* s = simCreate(2, 8192);
    GameMode   mode = {};
    CHECK(gameModeSandboxStart(&mode, s, 4096));
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
void testRenderDataLayout(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    spawnBlock(s, TEAM_PLAYER, UNIT_MELEE, 0, 0);
    spawnBlock(s, TEAM_ENEMY, UNIT_TANK, 15, 15);
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
        onGround = onGround && t.y == 0.0f && t.x > 0.0f && t.x < simGetWorldSize(s) && t.z > 0.0f && t.z < simGetWorldSize(s);
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
    spawnBlock(s, TEAM_PLAYER, UNIT_RANGED, 8, 7);
    spawnBlock(s, TEAM_ENEMY, UNIT_TANK, 8, 8);
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

    // Before the first step everyone idles; clocks run and are not in lockstep.
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

    // The armies start 10+ m apart, so units walk towards each other.
    stepFor(s, 0.5f);
    simFillRenderData(s, &data);
    CHECK(countClip(data, ANIM_WALK, false) > 0u);

    // The player wins (two blocks against one); a celebrating team dances.
    runBattle(s, 60.0f);
    CHECK(simGetUnitCount(s, TEAM_ENEMY) == 0u && simGetUnitCount(s, TEAM_PLAYER) > 0u);
    simSetCelebratingTeam(s, TEAM_PLAYER);
    simFillRenderData(s, &data);
    CHECK(data.mUnitCount > 0u && countClip(data, ANIM_DANCE, false) == data.mUnitCount);
    simDestroy(s);

    // Only the celebrating team dances, even with both armies alive.
    s = simCreate(1);
    spawnBlock(s, TEAM_PLAYER, UNIT_MELEE, 0, 0);
    spawnBlock(s, TEAM_ENEMY, UNIT_TANK, 15, 15);
    simSetCelebratingTeam(s, TEAM_ENEMY);
    simFillRenderData(s, &data);
    CHECK(countClip(data, ANIM_DANCE, true) == 0u);
    CHECK(countClip(data, ANIM_DANCE, false) == 9u);
    simDestroy(s);

    // Units in range of their target play the attack clip; the one unit out of range walks.
    s = simCreate(1);
    spawnBlock(s, TEAM_PLAYER, UNIT_MELEE, 0, 0);           // 5 x 5 player block, z 0.5 .. 4.5
    simSpawnUnit(s, TEAM_ENEMY, UNIT_TANK, 2.5f, 5.5f);     // touching the block's front row
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
void testSandboxSetup(TestContext* pCtx)
{
    const uint32_t perTeam = 4096;
    BattleSim*     s = simCreate(1, 2 * perTeam);
    GameMode       mode = {};
    CHECK(simGetUnitCapacity(s) == 2 * perTeam);
    CHECK(simGetWorldSize(s) == 80.0f);
    CHECK(!gameModeSandboxStart(&mode, s, perTeam + 1)); // over capacity

    CHECK(gameModeSandboxStart(&mode, s, perTeam));
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == perTeam && simGetUnitCount(s, TEAM_ENEMY) == perTeam);
    const float world = simGetWorldSize(s);
    CHECK(world > 80.0f);                                   // 8k units do not fit 80 m at normal spacing
    CHECK(simTestMinUnitDistance(s, TEAM_PLAYER) >= 0.99f); // same 1 m spacing as a melee block

    RenderData data = allocRenderData(2 * perTeam);
    float      maxX = 0.0f;
    simFillRenderData(s, &data);
    CHECK(data.mUnitCount == 2 * perTeam);
    CHECK(allInsideWorld(data, world, &maxX));
    CHECK(maxX > 80.0f);

    // Units move freely on the bigger world.
    gameModeUpdate(&mode, 0.5f / 15.0f);
    stepFor(s, 0.5f);
    CHECK(gameModeGetWinner(&mode) == -1);
    CHECK(simTestAllPositionsFinite(s));
    simFillRenderData(s, &data);
    CHECK(allInsideWorld(data, world, &maxX));
    CHECK(maxX > 80.0f);

    // Clearing returns to an empty 80 m world.
    simClear(s, 80.0f);
    CHECK(simGetWorldSize(s) == 80.0f);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 0u && simGetUnitCount(s, TEAM_ENEMY) == 0u);
    freeRenderData(&data);
    simDestroy(s);
}

void testNearestEnemySearch(TestContext* pCtx)
{
    // Two 4,096-unit armies 10 m apart, 46 rows deep: back-row units are far from any enemy.
    BattleSim* s = simCreate(1, 8192);
    GameMode   mode = {};
    CHECK(gameModeSandboxStart(&mode, s, 4096));
    SimSearchStats stats = simTestNearestEnemySearch(s);
    CHECK(stats.mSearches == 8192u);
    CHECK(stats.mMismatches == 0u);
    // Searching only the rings that can hold an enemy keeps the work near the front.
    CHECK(stats.mAverageBucketsVisited < 300.0f);
    char line[96];
    snprintf(line, sizeof(line), "  average buckets visited per search: %.1f", stats.mAverageBucketsVisited);
    writeLine(pCtx, line);

    // Scattered units, so searches start at many different distances from the enemy.
    simClear(s, 80.0f);
    uint32_t state = 12345u;
    for (uint32_t i = 0; i < 600; ++i)
    {
        state = state * 1664525u + 1013904223u;
        const float x = 1.0f + (float)((state >> 8) % 7800u) / 100.0f;
        state = state * 1664525u + 1013904223u;
        const float z = 1.0f + (float)((state >> 8) % 7800u) / 100.0f;
        simSpawnUnit(s, i % 7 == 0 ? TEAM_ENEMY : TEAM_PLAYER, UNIT_MELEE, x, z);
    }
    stats = simTestNearestEnemySearch(s);
    CHECK(stats.mSearches == 600u);
    CHECK(stats.mMismatches == 0u);
    simDestroy(s);
}

void testSpawnRules(TestContext* pCtx)
{
    BattleSim* s = simCreate(1, 4);
    simClear(s, 50.0f);
    CHECK(simGetWorldSize(s) == 50.0f && simGetUnitCount(s, TEAM_PLAYER) == 0u);
    CHECK(simSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, 10.0f, 10.0f));
    CHECK(!simSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, -1.0f, 10.0f)); // outside the world
    CHECK(!simSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, 10.0f, 50.0f));
    CHECK(!simSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, NAN, 10.0f));
    CHECK(simSpawnUnit(s, TEAM_ENEMY, UNIT_TANK, 20.0f, 20.0f));
    CHECK(simSpawnUnit(s, TEAM_ENEMY, UNIT_RANGED, 21.0f, 20.0f));
    CHECK(simSpawnUnit(s, TEAM_ENEMY, UNIT_RANGED, 22.0f, 20.0f));
    CHECK(!simSpawnUnit(s, TEAM_ENEMY, UNIT_RANGED, 23.0f, 20.0f)); // capacity 4 reached
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 1u && simGetUnitCount(s, TEAM_ENEMY) == 3u);
    simClear(s, 2000.0f);
    CHECK(simGetWorldSize(s) == MAX_WORLD_SIZE); // clamped
    CHECK(simGetUnitCount(s, TEAM_PLAYER) + simGetUnitCount(s, TEAM_ENEMY) == 0u);
    simDestroy(s);
}

void testSandboxBattle(TestContext* pCtx)
{
    BattleSim* s = simCreate(1, 1024);
    GameMode   mode = {};
    CHECK(!gameModeSandboxStart(&mode, s, 513)); // 1,026 units: over capacity
    CHECK(simGetUnitCount(s, TEAM_PLAYER) + simGetUnitCount(s, TEAM_ENEMY) == 0u);
    CHECK(gameModeSandboxStart(&mode, s, 200));
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 200u && simGetUnitCount(s, TEAM_ENEMY) == 200u);
    CHECK(gameModeGetWinner(&mode) == -1);

    // Bad steps change nothing and never decide the battle.
    gameModeUpdate(&mode, NAN);
    gameModeUpdate(&mode, -1.0f);
    gameModeUpdate(&mode, 0.0f);
    CHECK(simGetTime(s) == 0.0f && gameModeGetWinner(&mode) == -1);

    for (int32_t i = 0; i < 60 * 120 && gameModeGetWinner(&mode) == -1; ++i)
        gameModeUpdate(&mode, 1.0f / 60.0f);
    const int32_t winner = gameModeGetWinner(&mode);
    CHECK(winner == TEAM_PLAYER || winner == TEAM_ENEMY);
    CHECK(simGetUnitCount(s, (Team)(1 - winner)) == 0u && simGetUnitCount(s, (Team)winner) > 0u);

    // The winners dance.
    RenderData data = allocRenderData(1024);
    simFillRenderData(s, &data);
    bool dancing = data.mUnitCount > 0u;
    for (uint32_t i = 0; i < data.mUnitCount; ++i)
        dancing = dancing && data.pUnitAnims[i].mClip == ANIM_DANCE;
    CHECK(dancing);

    // Restarting mid-battle clears the units, projectiles, winner and celebration.
    CHECK(gameModeSandboxStart(&mode, s, 100));
    gameModeUpdate(&mode, 1.0f / 60.0f);
    CHECK(gameModeSandboxStart(&mode, s, 100));
    CHECK(gameModeGetWinner(&mode) == -1 && simGetProjectileCount(s) == 0u && simGetTime(s) == 0.0f);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 100u && simGetUnitCount(s, TEAM_ENEMY) == 100u);
    simFillRenderData(s, &data);
    bool nobodyDances = true;
    for (uint32_t i = 0; i < data.mUnitCount; ++i)
        nobodyDances = nobodyDances && data.pUnitAnims[i].mClip != ANIM_DANCE;
    CHECK(nobodyDances);
    freeRenderData(&data);
    simDestroy(s);
}
