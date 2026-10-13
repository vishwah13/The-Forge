#pragma once

#include "../../../../Common_3/Utilities/Math/MathTypes.h"

// Gameplay simulation for 40_AutoBattler, built on Flecs. Has no graphics dependencies so the self
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

// GPU instance layouts. Must match UnitInstance/BillboardInstance in Shaders/FSL/AutoBattler.h.fsl.
struct UnitInstance
{
    mat4   mWorld;
    float4 mColor; // rgb team colour, a = hit flash [0,1]
};

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
    uint32_t           mUnitCount;
    uint32_t           mBillboardCount;
};

struct BattleSim;

BattleSim* simCreate(uint32_t threadCount);
void       simDestroy(BattleSim* pSim);

// Round 1, START_GOLD, empty player board, fresh enemy wave, PHASE_PREP.
void simRestart(BattleSim* pSim);

// PREP with at least one player squad -> BATTLE. Otherwise refused (see simGetMessage()).
bool simStartBattle(BattleSim* pSim);

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

// Writes one UnitInstance per unit, a health bar per damaged unit and a billboard per projectile.
void simFillRenderData(BattleSim* pSim, RenderData* pOut);

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

// Runs the headless simulation tests, writes a report next to the log, returns the failure count.
int runAutoBattlerSelfTest(const char* pResultFileName);
