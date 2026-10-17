#pragma once

#include "../../../../Common_3/Utilities/Math/MathTypes.h"

#include "UnitTypes.h"

// Gameplay simulation for AutoBattler, built on Flecs. Has no graphics dependencies so the self
// test can drive it without a renderer. Only one BattleSim may exist at a time: Flecs component ids
// are file-scope globals that are re-registered for each world.

enum Team : uint8_t
{
    TEAM_PLAYER = 0,
    TEAM_ENEMY,
    TEAM_COUNT
};

static const uint32_t MAX_UNITS = 4096;
static const uint32_t MAX_PROJECTILES = 1024;
static const float    MAX_STEP = 1.0f / 30.0f;
static const float    MAX_WORLD_SIZE = 1024.0f; // metres; the spatial grid is sized for it
static const float    UNIT_RADIUS = 0.4f;
static const float    PROJECTILE_SPEED = 15.0f;
static const uint32_t MAX_BILLBOARDS = MAX_UNITS + MAX_PROJECTILES;
static const float    UNIT_MODEL_SCALE = 1.0f; // the Paladin is modelled in metres, ~1.73 m tall

// Animation clip a unit is playing. The app turns (clip, time) into a baked palette offset.
enum AnimClip : uint8_t
{
    ANIM_IDLE = 0,
    ANIM_WALK,
    ANIM_ATTACK, // in range of the target
    ANIM_DANCE,  // the celebrating team (simSetCelebratingTeam)
    ANIM_CLIP_COUNT
};

struct UnitInstance; // UnitInstance.h

// Clip and time a unit is playing; the app turns it into UnitInstance::mPose. CPU only.
struct UnitAnim
{
    AnimClip mClip;
    float    mTime; // seconds
};

// GPU instance layout. Must match BillboardInstance in Shaders/FSL/AutoBattler.h.fsl.
struct BillboardInstance
{
    float4 mCenterHalfWidth;      // xyz world centre, w half width
    float4 mFullColorFill;        // rgb colour left of the fill line, a = fill fraction [0,1]
    float4 mEmptyColorHalfHeight; // rgb colour right of the fill line, a = half height
};

struct RenderData
{
    UnitInstance*      pUnits;      // capacity MAX_UNITS
    BillboardInstance* pBillboards; // capacity MAX_BILLBOARDS
    UnitAnim*          pUnitAnims;  // capacity MAX_UNITS, parallel to pUnits
    uint32_t           mUnitCount;
    uint32_t           mBillboardCount;
};

struct BattleSim;

// threadCount: threads the sim may use, the caller's included (steering runs on threadCount - 1 workers
// plus the caller). unitCapacity: most units alive at once.
BattleSim* simCreate(uint32_t threadCount, uint32_t unitCapacity = MAX_UNITS);
void       simDestroy(BattleSim* pSim);

// Removes every unit and projectile and sets the world to a worldSize square (clamped to MAX_WORLD_SIZE).
void simClear(BattleSim* pSim, float worldSize);
// Adds one unit. Refused (false) at the unit capacity, outside the world, or for an invalid type or team.
bool simSpawnUnit(BattleSim* pSim, Team team, UnitType type, float x, float z);
// Advances movement and combat by deltaTime. Ignores NaN and non-positive steps; clamps to MAX_STEP.
void simStep(BattleSim* pSim, float deltaTime);
float simGetTime(const BattleSim* pSim); // seconds simulated since the last simClear
// Units of `team` play the dance clip (a winner celebrating); -1 for none. simClear resets it to -1.
void simSetCelebratingTeam(BattleSim* pSim, int32_t team);

uint32_t    simGetUnitCount(BattleSim* pSim, Team team);
uint32_t    simGetProjectileCount(const BattleSim* pSim);
uint32_t    simGetUnitCapacity(const BattleSim* pSim);
float       simGetWorldSize(const BattleSim* pSim); // set by simClear

// Wall-clock time spent in each part of the battle steps since the last simResetTimings(), summed, in
// microseconds. mSystemsUs is all of ecs_progress: the single-threaded systems timed separately here plus
// Flecs' own scheduling and command merges.
struct SimTimings
{
    int64_t  mGridUs;       // spatial grid rebuild
    int64_t  mSteerUs;      // steering, spread over the sim's threads in small chunks
    int64_t  mSystemsUs;    // ecs_progress
    int64_t  mAttackUs;     // AttackSystem
    int64_t  mProjectileUs; // ProjectileSystem
    int64_t  mHealthUs;     // HealthSystem
    int64_t  mSteerCpuUs;   // steering summed over every thread (CPU time, not wall time)
    uint32_t mSteps;        // battle steps measured
};
void simGetTimings(const BattleSim* pSim, SimTimings* pOut);
void simResetTimings(BattleSim* pSim);

// Writes one UnitInstance and UnitAnim per unit, a health bar per damaged unit and a billboard per projectile.
// pOut needs room for simGetUnitCapacity() units and simGetUnitCapacity() + MAX_PROJECTILES billboards.
void simFillRenderData(BattleSim* pSim, RenderData* pOut);

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Test hooks.
float simTestTotalHp(BattleSim* pSim, Team team);
bool  simTestAllPositionsFinite(BattleSim* pSim);
float simTestMinUnitDistance(BattleSim* pSim, Team team);

// Runs the nearest-enemy search for every unit on a fresh grid and checks each result against a
// brute-force search (same distance; ties may pick a different unit).
struct SimSearchStats
{
    uint32_t mSearches;
    uint32_t mMismatches;
    float    mAverageBucketsVisited;
};
SimSearchStats simTestNearestEnemySearch(BattleSim* pSim);

// Runs the headless simulation tests, writes a report next to the log, returns the failure count.
int runAutoBattlerSelfTest(const char* pResultFileName);
