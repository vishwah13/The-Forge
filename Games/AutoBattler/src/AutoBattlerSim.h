#pragma once

#include "../../../Common_3/Utilities/Math/MathTypes.h"

// Gameplay simulation for AutoBattler, built on Flecs. Has no graphics dependencies so the self
// test can drive it without a renderer. Only one BattleSim may exist at a time: Flecs component ids
// are file-scope globals that are re-registered for each world.

enum UnitType : int8_t
{
    UNIT_NONE = -1,
    UNIT_MELEE = 0,
    UNIT_RANGED,
    UNIT_TANK,
    UNIT_TYPE_COUNT
};

enum Team : uint8_t
{
    TEAM_PLAYER = 0,
    TEAM_ENEMY,
    TEAM_COUNT
};

enum Phase : uint8_t
{
    PHASE_PREP = 0,
    PHASE_BATTLE,
    PHASE_VICTORY,
    PHASE_DEFEAT
};

static const int32_t  ARENA_CELLS = 16;
static const int32_t  PLAYER_ROWS = ARENA_CELLS / 2; // rows [0, PLAYER_ROWS) belong to the player
static const float    CELL_SIZE = 5.0f;
static const float    ARENA_SIZE = ARENA_CELLS * CELL_SIZE;
static const uint32_t MAX_UNITS = 4096;
static const uint32_t MAX_PROJECTILES = 1024;
static const float    BATTLE_TIME_LIMIT = 90.0f;
static const float    RESULT_DURATION = 4.0f;
static const float    MAX_STEP = 1.0f / 30.0f;
static const int32_t  START_GOLD = 10;
static const float    UNIT_RADIUS = 0.4f;
static const float    PROJECTILE_SPEED = 15.0f;
static const uint32_t MAX_BILLBOARDS = MAX_UNITS + MAX_PROJECTILES;
static const float    UNIT_MODEL_SCALE = 1.0f; // the Paladin is modelled in metres, ~1.73 m tall

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

// Animation clip a unit is playing. The app turns (clip, time) into a baked palette offset.
enum AnimClip : uint8_t
{
    ANIM_IDLE = 0,
    ANIM_WALK,
    ANIM_ATTACK, // in range of the target
    ANIM_DANCE,  // winners after the battle
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
// plus the caller). unitCapacity sizes unit storage; the game itself never exceeds MAX_UNITS, only stress
// runs use more.
BattleSim* simCreate(uint32_t threadCount, uint32_t unitCapacity = MAX_UNITS);
void       simDestroy(BattleSim* pSim);

// Round 1, START_GOLD, empty player board, fresh enemy wave, PHASE_PREP.
void simRestart(BattleSim* pSim);

// PREP with at least one player squad -> BATTLE. Otherwise refused (see simGetMessage()).
bool simStartBattle(BattleSim* pSim);

// Stress run: replaces everything with two facing armies of unitsPerTeam at normal (1 m) spacing on a
// world sized to hold them, and starts the battle. Refused if both armies exceed the unit capacity.
// simRestart() returns to the normal ARENA_SIZE game.
bool simStressSetup(BattleSim* pSim, uint32_t unitsPerTeam);

// Advances the battle or result timer. Ignores NaN/non-positive steps and clamps to MAX_STEP.
void simStep(BattleSim* pSim, float deltaTime);

// Board editing, PREP only. On refusal the state is unchanged and simGetMessage() says why.
bool     simCanBuySquad(const BattleSim* pSim, int32_t cellX, int32_t cellZ, UnitType type);
bool     simBuySquad(BattleSim* pSim, int32_t cellX, int32_t cellZ, UnitType type);
bool     simSellSquad(BattleSim* pSim, int32_t cellX, int32_t cellZ);
UnitType simGetCell(const BattleSim* pSim, int32_t cellX, int32_t cellZ);

Phase       simGetPhase(const BattleSim* pSim);
int32_t     simGetRound(const BattleSim* pSim);
int32_t     simGetGold(const BattleSim* pSim);
uint32_t    simGetUnitCount(BattleSim* pSim, Team team);
float       simGetBattleTime(const BattleSim* pSim);
uint32_t    simGetProjectileCount(const BattleSim* pSim);
const char* simGetMessage(const BattleSim* pSim);
uint32_t    simGetUnitCapacity(const BattleSim* pSim);
float       simGetWorldSize(const BattleSim* pSim); // ARENA_SIZE except during stress runs

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

// Pure helpers, also used by the app's mouse picking.
bool   worldToCell(float x, float z, int32_t* pCellX, int32_t* pCellZ);
float2 cellCenter(int32_t cellX, int32_t cellZ);
bool   rayToCell(const float3& origin, const float3& dir, int32_t* pCellX, int32_t* pCellZ);

// Test hooks: edit either half of the board directly, ignoring gold and phase (never the unit cap).
bool simTestSetCell(BattleSim* pSim, int32_t cellX, int32_t cellZ, UnitType type);
void simTestClearBoard(BattleSim* pSim);
void simTestSetGold(BattleSim* pSim, int32_t gold);
void  simTestSpawnUnit(BattleSim* pSim, Team team, UnitType type, float x, float z);
void  simTestSetBattleTime(BattleSim* pSim, float seconds);
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
