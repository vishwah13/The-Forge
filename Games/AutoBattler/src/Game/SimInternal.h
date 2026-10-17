#pragma once

// Private state and functions shared by the Game/ modules. Not part of the sim API (Sim.h).

#include "Components.h"
#include "Sim.h"

#include "../../../../Common_3/Utilities/Interfaces/ITime.h"
#include "../../../../Common_3/Utilities/Threading/Atomics.h"
#include "../../../../Common_3/Utilities/Threading/ThreadSystem.h"

static const float   RETARGET_INTERVAL = 0.25f;
static const float   SEPARATION_SPEED = 4.0f;
static const float   WALK_ANIM_SPEED = 1.72f; // ground speed of the walk clip at 1x for a model of scale 1 (measured foot speed)
static const float   GRID_BUCKET_SIZE = 2.0f;

struct GridEntry
{
    ecs_entity_t mEntity;
    float        mX, mZ, mRadius;
    uint32_t     mTeam;
};

// Per-frame snapshot of every unit, sorted by bucket. Systems read positions of *other* units only
// from here, so steering (run on several threads) never reads data another thread is writing.
struct SpatialGrid
{
    GridEntry* pEntries;      // unit capacity, sorted by bucket
    GridEntry* pScratch;      // unit capacity, query order
    uint32_t*  pBucketStart;  // mDim * mDim + 1
    uint32_t*  pBucketCursor; // mDim * mDim
    // Per team and bucket: rings out (Chebyshev distance in buckets) to the nearest bucket holding a
    // unit of another team, NO_ENEMY if there is none. Lets the nearest-enemy search skip empty rings.
    uint16_t*  pEnemyRings[TEAM_COUNT];
    uint32_t   mCount;
    int32_t    mDim; // buckets per side, covers the world
};

static const uint16_t NO_ENEMY = 0xFFFF;

struct BattleSim
{
    ecs_world_t* pWorld;
    ecs_query_t* pUnitQuery; // Position, Unit, Health
    ecs_query_t* pProjectileQuery;
    ecs_query_t* pSteerQuery; // Position, Unit, Attack
    float        mTime; // seconds since the last simClear
    uint32_t     mProjectileCount;
    uint32_t     mSpawnCounter;
    uint32_t     mUnitCapacity;
    float        mWorldSize; // set by simClear
    int32_t      mCelebratingTeam; // plays ANIM_DANCE; -1 for none
    SpatialGrid  mGrid;
    SimTimings   mTimings;
    tfrg_atomic64_t mSteerCpuUs; // steering runs on several threads, so its sum is kept atomically
    ThreadSystem    pThreads;    // steering workers; NULL when the sim is single-threaded
    uint32_t        mWorkerCount;
    struct SteerChunk* pSteerChunks;
    uint32_t           mSteerChunkCapacity;
    uint32_t           mSteerChunkCount;
    tfrg_atomic32_t    mNextSteerChunk; // next chunk a thread takes
};

// A run of up to STEER_CHUNK_SIZE consecutive units in one Flecs table. Steering is handed out in these
// small chunks so threads that finish early take more work instead of waiting for the busiest slice.
struct SteerChunk
{
    BattleSim*          p;
    Position*           pPos;
    Unit*               pUnit;
    Attack*             pAttack;
    const ecs_entity_t* pEntities;
    int32_t             mCount;
    float               mDt;
};
static const int32_t STEER_CHUNK_SIZE = 256;

// Adds the scope's wall-clock time to *pAccumulator.
struct ScopedUSec
{
    int64_t* pAccumulator;
    int64_t  mStart;
    explicit ScopedUSec(int64_t* pAcc): pAccumulator(pAcc), mStart(getUSec(true)) {}
    ~ScopedUSec() { *pAccumulator += getUSec(true) - mStart; }
};

// SpatialGrid.cpp
void             buildGrid(BattleSim* p);
const GridEntry* lookupTarget(const ecs_world_t* pWorld, const SpatialGrid& g, ecs_entity_t target);
int32_t          findNearestEnemy(const SpatialGrid& g, float x, float z, uint32_t team, uint32_t* pBucketsVisited = NULL);
void             separation(const SpatialGrid& g, ecs_entity_t self, float x, float z, float radius, float* pOutX, float* pOutZ);

// Steering.cpp
void steerUnits(BattleSim* p, float dt);

// Combat.cpp
void combatRegisterSystems(BattleSim* p);

// Health.cpp
void applyDamage(ecs_world_t* pWorld, ecs_entity_t target, float damage);
void healthRegisterSystems(BattleSim* p);

// Sim.cpp
// Adds a unit without checks; callers have validated the position and the unit capacity.
void spawnUnit(BattleSim* p, Team team, UnitType type, float x, float z);
void addSystem(BattleSim* p, const char* pName, ecs_entity_t phase, ecs_iter_action_t callback, bool multiThreaded, ecs_id_t c0,
               ecs_id_t c1 = 0, ecs_id_t c2 = 0);
