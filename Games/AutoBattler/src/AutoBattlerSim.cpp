#include "AutoBattlerSim.h"
#include "UnitInstance.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "../../../Common_3/Game/ThirdParty/OpenSource/flecs/flecs.h"
#include "../../../Common_3/Utilities/Interfaces/ILog.h"
#include "../../../Common_3/Utilities/Interfaces/ITime.h"
#include "../../../Common_3/Utilities/Threading/Atomics.h"

#include "../../../Common_3/Utilities/Interfaces/IMemory.h"

const UnitStats gUnitStats[UNIT_TYPE_COUNT] = {
    // name     side cost  hp      damage cooldown range speed scale
    { "Melee", 5, 2, 100.0f, 15.0f, 0.8f, 1.2f, 3.0f, 1.0f },
    { "Ranged", 4, 3, 60.0f, 12.0f, 1.0f, 8.0f, 2.5f, 0.9f },
    { "Tank", 3, 4, 250.0f, 8.0f, 1.2f, 1.4f, 1.8f, 1.3f },
};

static const float   RETARGET_INTERVAL = 0.25f;
static const float   SEPARATION_SPEED = 4.0f;
static const float   WALK_ANIM_SPEED = 1.72f; // ground speed of the walk clip at 1x for a model of scale 1 (measured foot speed)
static const float   GRID_BUCKET_SIZE = 2.0f;
static const float   STRESS_SPACING = 1.0f; // same as a melee squad on the board
static const float   STRESS_GAP = 10.0f;    // between the two armies' front rows
static const float   STRESS_MARGIN = 5.0f;  // between the armies and the world edge

//--------------------------------------------------------------------------------------------
// Components
//--------------------------------------------------------------------------------------------
struct Position
{
    float x, z;
};

struct Unit
{
    uint8_t  team;
    uint8_t  type;
    uint16_t cell; // board cell index, 0xFFFF when not spawned from the board
    float    radius;
    float    yaw;
    float    animTime; // animation clock, offset per unit so crowds are not in lockstep
    uint32_t moving;   // walked this frame (drives the walk vs idle clip)
    uint32_t attacking; // in range of its target this frame (drives the attack clip)
};

struct Health
{
    float hp, maxHp, hitFlash;
};

struct Attack
{
    ecs_entity_t target;
    float        timer;
    float        retargetTimer;
    uint32_t     inRange;
};

struct GridSlot
{
    uint32_t slot; // index into this frame's grid snapshot
};

struct Projectile
{
    ecs_entity_t target;
    float        x, y, z;
    float        damage;
};

ECS_COMPONENT_DECLARE(Position);
ECS_COMPONENT_DECLARE(Unit);
ECS_COMPONENT_DECLARE(Health);
ECS_COMPONENT_DECLARE(Attack);
ECS_COMPONENT_DECLARE(GridSlot);
ECS_COMPONENT_DECLARE(Projectile);

// Component ids are globals; reset them so each new world registers fresh ids.
#define AB_DEFINE_COMPONENT(world, T) \
    ecs_id(T) = 0;                    \
    ECS_COMPONENT_DEFINE(world, T)

struct GridEntry
{
    ecs_entity_t mEntity;
    float        mX, mZ, mRadius;
    uint32_t     mTeam;
};

// Per-frame snapshot of every unit, sorted by bucket. Systems read positions of *other* units only
// from here, so the multithreaded steering system never reads data another thread is writing.
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
    int8_t       mBoard[ARENA_CELLS * ARENA_CELLS]; // UnitType per cell, UNIT_NONE when empty
    Phase        mPhase;
    int32_t      mRound;
    int32_t      mGold;
    float        mBattleTime;
    float        mResultTime;
    uint32_t     mProjectileCount;
    uint32_t     mSpawnCounter;
    const char*  pMessage;
    uint32_t     mUnitCapacity;
    float        mWorldSize; // ARENA_SIZE, or larger during a stress run
    SpatialGrid  mGrid;
    SimTimings   mTimings;
    tfrg_atomic64_t mSteerCpuUs; // SteerSystem runs on worker threads, so its sum is kept atomically
};

// Adds the scope's wall-clock time to *pAccumulator.
struct ScopedUSec
{
    int64_t* pAccumulator;
    int64_t  mStart;
    explicit ScopedUSec(int64_t* pAcc): pAccumulator(pAcc), mStart(getUSec(true)) {}
    ~ScopedUSec() { *pAccumulator += getUSec(true) - mStart; }
};

//--------------------------------------------------------------------------------------------
// Board helpers
//--------------------------------------------------------------------------------------------
static bool cellInArena(int32_t x, int32_t z) { return x >= 0 && x < ARENA_CELLS && z >= 0 && z < ARENA_CELLS; }

static int32_t cellIndex(int32_t x, int32_t z) { return z * ARENA_CELLS + x; }

static uint32_t squadSize(UnitType type)
{
    const uint32_t side = (uint32_t)gUnitStats[type].mSquadSide;
    return side * side;
}

static uint32_t boardUnitCount(const BattleSim* p)
{
    uint32_t units = 0;
    for (int32_t i = 0; i < ARENA_CELLS * ARENA_CELLS; ++i)
        if (p->mBoard[i] != UNIT_NONE)
            units += squadSize((UnitType)p->mBoard[i]);
    return units;
}

bool worldToCell(float x, float z, int32_t* pCellX, int32_t* pCellZ)
{
    // Written as a positive test so NaN is rejected too.
    if (!(x >= 0.0f && x < ARENA_SIZE && z >= 0.0f && z < ARENA_SIZE))
        return false;
    const int32_t cellX = (int32_t)(x / CELL_SIZE);
    const int32_t cellZ = (int32_t)(z / CELL_SIZE);
    *pCellX = cellX < ARENA_CELLS ? cellX : ARENA_CELLS - 1;
    *pCellZ = cellZ < ARENA_CELLS ? cellZ : ARENA_CELLS - 1;
    return true;
}

float2 cellCenter(int32_t cellX, int32_t cellZ) { return float2(((float)cellX + 0.5f) * CELL_SIZE, ((float)cellZ + 0.5f) * CELL_SIZE); }

bool rayToCell(const float3& origin, const float3& dir, int32_t* pCellX, int32_t* pCellZ)
{
    if (!(dir.y < -1e-4f)) // parallel to the ground or pointing up
        return false;
    const float t = -origin.y / dir.y;
    if (t < 0.0f)
        return false;
    return worldToCell(origin.x + dir.x * t, origin.z + dir.z * t, pCellX, pCellZ);
}

//--------------------------------------------------------------------------------------------
// Spawning
//--------------------------------------------------------------------------------------------
static void spawnUnit(BattleSim* p, Team team, UnitType type, float x, float z, uint16_t cell)
{
    const UnitStats&   stats = gUnitStats[type];
    const ecs_entity_t e = ecs_new(p->pWorld);

    const Position pos = { x, z };
    const Unit     unit = { (uint8_t)team, (uint8_t)type, cell, UNIT_RADIUS * stats.mScale, team == TEAM_PLAYER ? 0.0f : PI,
                            (float)(p->mSpawnCounter % 97u) * 0.37f, 0u, 0u };
    const Health   health = { stats.mHp, stats.mHp, 0.0f };
    // Stagger retargeting so only a slice of the army searches each frame.
    const Attack   attack = { 0, 0.0f, RETARGET_INTERVAL * (float)(p->mSpawnCounter++ % 16u) / 16.0f, 0u };
    const GridSlot slot = { UINT32_MAX };

    ecs_set(p->pWorld, e, Position, pos);
    ecs_set(p->pWorld, e, Unit, unit);
    ecs_set(p->pWorld, e, Health, health);
    ecs_set(p->pWorld, e, Attack, attack);
    ecs_set(p->pWorld, e, GridSlot, slot);
}

static void spawnSquad(BattleSim* p, int32_t cellX, int32_t cellZ, UnitType type)
{
    const int32_t side = gUnitStats[type].mSquadSide;
    const float   spacing = CELL_SIZE / (float)side;
    const Team    team = cellZ < PLAYER_ROWS ? TEAM_PLAYER : TEAM_ENEMY;
    for (int32_t j = 0; j < side; ++j)
        for (int32_t i = 0; i < side; ++i)
            spawnUnit(p, team, type, (float)cellX * CELL_SIZE + ((float)i + 0.5f) * spacing,
                      (float)cellZ * CELL_SIZE + ((float)j + 0.5f) * spacing, (uint16_t)cellIndex(cellX, cellZ));
}

static void setWorldSize(BattleSim* p, float worldSize)
{
    p->mWorldSize = worldSize;
    p->mGrid.mDim = (int32_t)ceilf(worldSize / GRID_BUCKET_SIZE);
}

// Units on the field always mirror the board while not in battle, and the board game is played on the arena.
static void rebuildFromBoard(BattleSim* p)
{
    setWorldSize(p, ARENA_SIZE);
    ecs_delete_with(p->pWorld, ecs_id(Unit));
    ecs_delete_with(p->pWorld, ecs_id(Projectile));
    p->mProjectileCount = 0;
    for (int32_t z = 0; z < ARENA_CELLS; ++z)
        for (int32_t x = 0; x < ARENA_CELLS; ++x)
            if (p->mBoard[cellIndex(x, z)] != UNIT_NONE)
                spawnSquad(p, x, z, (UnitType)p->mBoard[cellIndex(x, z)]);
}

static uint32_t nextRandom(uint32_t* pState)
{
    uint32_t s = *pState;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *pState = s;
    return s;
}

// Fills the enemy half for the current round. Seeded by the round so waves are repeatable.
static void generateWave(BattleSim* p)
{
    for (int32_t z = PLAYER_ROWS; z < ARENA_CELLS; ++z)
        for (int32_t x = 0; x < ARENA_CELLS; ++x)
            p->mBoard[cellIndex(x, z)] = UNIT_NONE;

    uint32_t state = 0x9E3779B9u ^ ((uint32_t)p->mRound * 2654435761u);
    if (state == 0)
        state = 1;
    int32_t  budget = 6 + 4 * p->mRound;
    uint32_t units = boardUnitCount(p);
    for (int32_t attempt = 0; attempt < 256 && budget >= gUnitStats[UNIT_MELEE].mCost; ++attempt)
    {
        const UnitType type = (UnitType)(nextRandom(&state) % (uint32_t)UNIT_TYPE_COUNT);
        const int32_t  x = (int32_t)(nextRandom(&state) % (uint32_t)ARENA_CELLS);
        const int32_t  z = PLAYER_ROWS + (int32_t)(nextRandom(&state) % (uint32_t)(ARENA_CELLS - PLAYER_ROWS));
        if (gUnitStats[type].mCost > budget || p->mBoard[cellIndex(x, z)] != UNIT_NONE)
            continue;
        if (units + squadSize(type) > MAX_UNITS)
            break;
        p->mBoard[cellIndex(x, z)] = type;
        budget -= gUnitStats[type].mCost;
        units += squadSize(type);
    }
}

//--------------------------------------------------------------------------------------------
// Spatial grid
//--------------------------------------------------------------------------------------------
static int32_t bucketCoord(const SpatialGrid& g, float v)
{
    const int32_t b = (int32_t)(v / GRID_BUCKET_SIZE);
    return b < 0 ? 0 : (b >= g.mDim ? g.mDim - 1 : b);
}

static uint32_t bucketOf(const SpatialGrid& g, float x, float z) { return (uint32_t)(bucketCoord(g, z) * g.mDim + bucketCoord(g, x)); }

// Two-pass distance transform with unit-cost 8-neighbour steps: exact Chebyshev distance.
static void buildEnemyRings(SpatialGrid& g)
{
    const int32_t  dim = g.mDim;
    const uint32_t bucketCount = (uint32_t)(dim * dim);
    for (uint32_t team = 0; team < TEAM_COUNT; ++team)
    {
        uint16_t* d = g.pEnemyRings[team];
        for (uint32_t b = 0; b < bucketCount; ++b)
        {
            d[b] = NO_ENEMY;
            for (uint32_t s = g.pBucketStart[b]; s < g.pBucketStart[b + 1]; ++s)
                if (g.pEntries[s].mTeam != team)
                {
                    d[b] = 0;
                    break;
                }
        }
        // Forward pass takes the already-visited neighbours (left and the row above), the backward pass the rest.
        for (int32_t pass = 0; pass < 2; ++pass)
        {
            const int32_t step = pass == 0 ? 1 : -1;
            const int32_t first = pass == 0 ? 0 : dim - 1;
            for (int32_t z = first; z >= 0 && z < dim; z += step)
                for (int32_t x = first; x >= 0 && x < dim; x += step)
                {
                    uint32_t      best = d[z * dim + x];
                    const int32_t neighbours[4][2] = { { x - step, z }, { x - step, z - step }, { x, z - step }, { x + step, z - step } };
                    for (int32_t n = 0; n < 4; ++n)
                    {
                        const int32_t nx = neighbours[n][0];
                        const int32_t nz = neighbours[n][1];
                        if (nx >= 0 && nx < dim && nz >= 0 && nz < dim && d[nz * dim + nx] != NO_ENEMY && d[nz * dim + nx] + 1u < best)
                            best = d[nz * dim + nx] + 1u;
                    }
                    d[z * dim + x] = (uint16_t)best;
                }
        }
    }
}

static void buildGrid(BattleSim* p)
{
    SpatialGrid& g = p->mGrid;
    g.mCount = 0;
    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pUnitQuery);
    while (ecs_query_next(&it))
    {
        const Position* pos = ecs_field(&it, Position, 0);
        const Unit*     unit = ecs_field(&it, Unit, 1);
        for (int32_t i = 0; i < it.count; ++i)
        {
            ASSERT(g.mCount < p->mUnitCapacity);
            GridEntry& e = g.pScratch[g.mCount++];
            e.mEntity = it.entities[i];
            e.mX = pos[i].x;
            e.mZ = pos[i].z;
            e.mRadius = unit[i].radius;
            e.mTeam = unit[i].team;
        }
    }

    const uint32_t bucketCount = (uint32_t)(g.mDim * g.mDim);
    memset(g.pBucketStart, 0, sizeof(uint32_t) * (bucketCount + 1));
    for (uint32_t i = 0; i < g.mCount; ++i)
        ++g.pBucketStart[bucketOf(g, g.pScratch[i].mX, g.pScratch[i].mZ) + 1];
    for (uint32_t b = 0; b < bucketCount; ++b)
        g.pBucketStart[b + 1] += g.pBucketStart[b];
    memcpy(g.pBucketCursor, g.pBucketStart, sizeof(uint32_t) * bucketCount);
    for (uint32_t i = 0; i < g.mCount; ++i)
        g.pEntries[g.pBucketCursor[bucketOf(g, g.pScratch[i].mX, g.pScratch[i].mZ)]++] = g.pScratch[i];

    for (uint32_t s = 0; s < g.mCount; ++s)
        ecs_get_mut(p->pWorld, g.pEntries[s].mEntity, GridSlot)->slot = s;

    buildEnemyRings(g);
}

static const GridEntry* lookupTarget(const ecs_world_t* pWorld, const SpatialGrid& g, ecs_entity_t target)
{
    if (!target || !ecs_is_alive(pWorld, target))
        return NULL;
    const GridSlot* pSlot = ecs_get(pWorld, target, GridSlot);
    if (!pSlot || pSlot->slot >= g.mCount || g.pEntries[pSlot->slot].mEntity != target)
        return NULL;
    return &g.pEntries[pSlot->slot];
}

// Searches rings of buckets outward and stops once no closer enemy can exist. Returns a slot or -1.
static int32_t findNearestEnemy(const SpatialGrid& g, float x, float z, uint32_t team, uint32_t* pBucketsVisited = NULL)
{
    const int32_t cx = bucketCoord(g, x);
    const int32_t cz = bucketCoord(g, z);
    int32_t       best = -1;
    float         bestD2 = FLT_MAX;
    // Rings closer than the nearest enemy bucket hold no enemies, so start there: same result, less work.
    const uint16_t firstRing = g.pEnemyRings[team][cz * g.mDim + cx];
    if (firstRing == NO_ENEMY)
        return -1;
    for (int32_t r = firstRing; r < g.mDim; ++r)
    {
        const float ringMin = (float)(r - 1) * GRID_BUCKET_SIZE;
        if (best >= 0 && ringMin > 0.0f && ringMin * ringMin > bestD2)
            break;
        for (int32_t bz = cz - r; bz <= cz + r; ++bz)
        {
            if (bz < 0 || bz >= g.mDim)
                continue;
            const bool    edgeRow = bz == cz - r || bz == cz + r;
            const int32_t step = edgeRow || r == 0 ? 1 : 2 * r; // interior rows: only the two ring columns
            for (int32_t bx = cx - r; bx <= cx + r; bx += step)
            {
                if (bx < 0 || bx >= g.mDim)
                    continue;
                const uint32_t b = (uint32_t)(bz * g.mDim + bx);
                if (pBucketsVisited)
                    ++*pBucketsVisited;
                for (uint32_t s = g.pBucketStart[b]; s < g.pBucketStart[b + 1]; ++s)
                {
                    const GridEntry& e = g.pEntries[s];
                    if (e.mTeam == team)
                        continue;
                    const float dx = e.mX - x;
                    const float dz = e.mZ - z;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < bestD2)
                    {
                        bestD2 = d2;
                        best = (int32_t)s;
                    }
                }
            }
        }
    }
    return best;
}

static void separation(const SpatialGrid& g, ecs_entity_t self, float x, float z, float radius, float* pOutX, float* pOutZ)
{
    float         sx = 0.0f;
    float         sz = 0.0f;
    const int32_t cx = bucketCoord(g, x);
    const int32_t cz = bucketCoord(g, z);
    for (int32_t bz = cz - 1; bz <= cz + 1; ++bz)
    {
        for (int32_t bx = cx - 1; bx <= cx + 1; ++bx)
        {
            if (bx < 0 || bx >= g.mDim || bz < 0 || bz >= g.mDim)
                continue;
            const uint32_t b = (uint32_t)(bz * g.mDim + bx);
            for (uint32_t s = g.pBucketStart[b]; s < g.pBucketStart[b + 1]; ++s)
            {
                const GridEntry& e = g.pEntries[s];
                if (e.mEntity == self)
                    continue;
                const float dx = x - e.mX;
                const float dz = z - e.mZ;
                const float d2 = dx * dx + dz * dz;
                const float minDist = radius + e.mRadius;
                if (d2 >= minDist * minDist)
                    continue;
                if (d2 > 1e-8f)
                {
                    const float d = sqrtf(d2);
                    const float push = (minDist - d) / minDist;
                    sx += dx / d * push;
                    sz += dz / d * push;
                }
                else
                {
                    // Exactly overlapping: push apart along a direction derived from the pair of ids,
                    // opposite for each of the two units.
                    const float angle = (float)(uint32_t)((self ^ e.mEntity) & 1023u) * (2.0f * PI / 1024.0f);
                    const float sign = self < e.mEntity ? 1.0f : -1.0f;
                    sx += cosf(angle) * sign;
                    sz += sinf(angle) * sign;
                }
            }
        }
    }
    *pOutX = sx;
    *pOutZ = sz;
}

//--------------------------------------------------------------------------------------------
// Systems
//--------------------------------------------------------------------------------------------
static void applyDamage(ecs_world_t* pWorld, ecs_entity_t target, float damage)
{
    if (!ecs_is_alive(pWorld, target))
        return;
    Health* pHealth = ecs_get_mut(pWorld, target, Health);
    if (pHealth)
    {
        pHealth->hp -= damage;
        pHealth->hitFlash = 1.0f;
    }
}

// Multithreaded: writes only its own entity's components and reads others from the grid snapshot.
static void SteerSystem(ecs_iter_t* it)
{
    BattleSim*         p = (BattleSim*)it->ctx;
    const int64_t      startUs = getUSec(true);
    const SpatialGrid& g = p->mGrid;
    Position*          pos = ecs_field(it, Position, 0);
    Unit*              unit = ecs_field(it, Unit, 1);
    Attack*            attack = ecs_field(it, Attack, 2);
    const float        dt = it->delta_time;

    for (int32_t i = 0; i < it->count; ++i)
    {
        const UnitStats& stats = gUnitStats[unit[i].type];
        Attack&          a = attack[i];

        const GridEntry* pTarget = lookupTarget(it->world, g, a.target);
        a.retargetTimer -= dt;
        if (!pTarget || a.retargetTimer <= 0.0f)
        {
            a.retargetTimer = RETARGET_INTERVAL;
            const int32_t slot = findNearestEnemy(g, pos[i].x, pos[i].z, unit[i].team);
            pTarget = slot >= 0 ? &g.pEntries[slot] : NULL;
            a.target = pTarget ? pTarget->mEntity : 0;
        }

        float vx = 0.0f;
        float vz = 0.0f;
        a.inRange = 0;
        if (pTarget)
        {
            const float dx = pTarget->mX - pos[i].x;
            const float dz = pTarget->mZ - pos[i].z;
            const float dist = sqrtf(dx * dx + dz * dz);
            if (dist <= stats.mRange + unit[i].radius + pTarget->mRadius)
                a.inRange = 1;
            else
            {
                vx = dx / dist * stats.mSpeed;
                vz = dz / dist * stats.mSpeed;
            }
            if (dist > 1e-4f)
                unit[i].yaw = atan2f(dx, dz);
        }

        float sx = 0.0f;
        float sz = 0.0f;
        separation(g, it->entities[i], pos[i].x, pos[i].z, unit[i].radius, &sx, &sz);
        vx += sx * SEPARATION_SPEED;
        vz += sz * SEPARATION_SPEED;

        unit[i].moving = (vx != 0.0f || vz != 0.0f) && !a.inRange ? 1u : 0u;
        unit[i].attacking = a.inRange;
        // Bigger units cover more ground per step, so the walk plays slower for the same speed.
        unit[i].animTime += unit[i].moving ? dt * stats.mSpeed / (WALK_ANIM_SPEED * UNIT_MODEL_SCALE * stats.mScale) : dt;

        const float r = unit[i].radius;
        pos[i].x = clampf(pos[i].x + vx * dt, r, p->mWorldSize - r);
        pos[i].z = clampf(pos[i].z + vz * dt, r, p->mWorldSize - r);
    }
    tfrg_atomic64_add_relaxed(&p->mSteerCpuUs, (uint64_t)(getUSec(true) - startUs));
}

// Single-threaded: damages other entities and spawns projectiles.
static void AttackSystem(ecs_iter_t* it)
{
    BattleSim*      p = (BattleSim*)it->ctx;
    ScopedUSec      timer(&p->mTimings.mAttackUs);
    const Position* pos = ecs_field(it, Position, 0);
    const Unit*     unit = ecs_field(it, Unit, 1);
    Attack*         attack = ecs_field(it, Attack, 2);

    for (int32_t i = 0; i < it->count; ++i)
    {
        Attack& a = attack[i];
        a.timer -= it->delta_time;
        if (a.timer < 0.0f)
            a.timer = 0.0f;
        if (a.timer > 0.0f || !a.inRange || !a.target || !ecs_is_alive(it->world, a.target))
            continue;

        const UnitStats& stats = gUnitStats[unit[i].type];
        a.timer = stats.mCooldown;
        if (unit[i].type == UNIT_RANGED)
        {
            if (p->mProjectileCount >= MAX_PROJECTILES)
                continue;
            const Projectile   projectile = { a.target, pos[i].x, 1.2f * stats.mScale, pos[i].z, stats.mDamage };
            const ecs_entity_t e = ecs_new(it->world);
            ecs_set(it->world, e, Projectile, projectile);
            ++p->mProjectileCount;
        }
        else
        {
            applyDamage(it->world, a.target, stats.mDamage);
        }
    }
}

static void ProjectileSystem(ecs_iter_t* it)
{
    BattleSim*  p = (BattleSim*)it->ctx;
    ScopedUSec  timer(&p->mTimings.mProjectileUs);
    Projectile* projectile = ecs_field(it, Projectile, 0);
    const float step = PROJECTILE_SPEED * it->delta_time;

    for (int32_t i = 0; i < it->count; ++i)
    {
        Projectile& pr = projectile[i];
        if (!pr.target || !ecs_is_alive(it->world, pr.target))
        {
            ecs_delete(it->world, it->entities[i]);
            --p->mProjectileCount;
            continue;
        }
        const Position* pTargetPos = ecs_get(it->world, pr.target, Position);
        const float     dx = pTargetPos->x - pr.x;
        const float     dz = pTargetPos->z - pr.z;
        const float     dist = sqrtf(dx * dx + dz * dz);
        if (dist <= step + 0.3f)
        {
            applyDamage(it->world, pr.target, pr.damage);
            ecs_delete(it->world, it->entities[i]);
            --p->mProjectileCount;
            continue;
        }
        pr.x += dx / dist * step;
        pr.z += dz / dist * step;
    }
}

static void HealthSystem(ecs_iter_t* it)
{
    ScopedUSec timer(&((BattleSim*)it->ctx)->mTimings.mHealthUs);
    Health*    health = ecs_field(it, Health, 0);
    for (int32_t i = 0; i < it->count; ++i)
    {
        health[i].hitFlash = health[i].hitFlash > 0.0f ? health[i].hitFlash - it->delta_time * 5.0f : 0.0f;
        if (health[i].hp <= 0.0f)
            ecs_delete(it->world, it->entities[i]);
    }
}

static void addSystem(BattleSim* p, const char* pName, ecs_entity_t phase, ecs_iter_action_t callback, bool multiThreaded, ecs_id_t c0,
                      ecs_id_t c1 = 0, ecs_id_t c2 = 0)
{
    const ecs_id_t    addIds[3] = { ecs_pair(EcsDependsOn, phase), phase, 0 };
    ecs_entity_desc_t entityDesc = {};
    entityDesc.name = pName;
    entityDesc.add = addIds;

    ecs_system_desc_t desc = {};
    desc.entity = ecs_entity_init(p->pWorld, &entityDesc);
    desc.query.terms[0].id = c0;
    desc.query.terms[1].id = c1;
    desc.query.terms[2].id = c2;
    desc.callback = callback;
    desc.ctx = p;
    desc.multi_threaded = multiThreaded;
    ecs_system_init(p->pWorld, &desc);
}

static void enterResult(BattleSim* p, Phase phase)
{
    p->mPhase = phase;
    p->mResultTime = 0.0f;
    ecs_delete_with(p->pWorld, ecs_id(Projectile));
    p->mProjectileCount = 0;
}

static bool boardHasPlayerSquad(const BattleSim* p)
{
    for (int32_t i = 0; i < ARENA_CELLS * PLAYER_ROWS; ++i)
        if (p->mBoard[i] != UNIT_NONE)
            return true;
    return false;
}

//--------------------------------------------------------------------------------------------
// Lifetime
//--------------------------------------------------------------------------------------------
// Army of unitsPerTeam at STRESS_SPACING: cols x rows per team, both facing across STRESS_GAP on a square world.
static void stressLayout(uint32_t unitsPerTeam, uint32_t* pCols, uint32_t* pRows, float* pWorldSize)
{
    const uint32_t cols = (uint32_t)ceilf(sqrtf(2.0f * (float)unitsPerTeam)); // twice as wide as deep
    const uint32_t rows = cols > 0 ? (unitsPerTeam + cols - 1) / cols : 0;
    const float    armies = fmaxf((float)cols * STRESS_SPACING, 2.0f * (float)rows * STRESS_SPACING + STRESS_GAP);
    *pCols = cols;
    *pRows = rows;
    *pWorldSize = fmaxf(ARENA_SIZE, armies + 2.0f * STRESS_MARGIN);
}

BattleSim* simCreate(uint32_t threadCount, uint32_t unitCapacity)
{
    static bool sEcsInitialized = false;
    if (!sEcsInitialized)
    {
        initEntityComponentSystem();
        ecs_log_set_level(-2);
        sEcsInitialized = true;
    }

    BattleSim* p = (BattleSim*)tf_calloc(1, sizeof(BattleSim));
    p->pWorld = ecs_init();
    ecs_set_threads(p->pWorld, (int32_t)(threadCount > 0 ? threadCount : 1));

    AB_DEFINE_COMPONENT(p->pWorld, Position);
    AB_DEFINE_COMPONENT(p->pWorld, Unit);
    AB_DEFINE_COMPONENT(p->pWorld, Health);
    AB_DEFINE_COMPONENT(p->pWorld, Attack);
    AB_DEFINE_COMPONENT(p->pWorld, GridSlot);
    AB_DEFINE_COMPONENT(p->pWorld, Projectile);

    ecs_query_desc_t unitQuery = {};
    unitQuery.terms[0].id = ecs_id(Position);
    unitQuery.terms[1].id = ecs_id(Unit);
    unitQuery.terms[2].id = ecs_id(Health);
    p->pUnitQuery = ecs_query_init(p->pWorld, &unitQuery);

    ecs_query_desc_t projectileQuery = {};
    projectileQuery.terms[0].id = ecs_id(Projectile);
    p->pProjectileQuery = ecs_query_init(p->pWorld, &projectileQuery);

    // Size the grid for the largest stress world this capacity allows.
    uint32_t cols = 0;
    uint32_t rows = 0;
    float    maxWorldSize = 0.0f;
    stressLayout(unitCapacity / 2, &cols, &rows, &maxWorldSize);
    const uint32_t maxDim = (uint32_t)ceilf(maxWorldSize / GRID_BUCKET_SIZE);
    const uint32_t bucketCount = maxDim * maxDim;
    p->mUnitCapacity = unitCapacity;
    p->mGrid.pEntries = (GridEntry*)tf_malloc(sizeof(GridEntry) * unitCapacity);
    p->mGrid.pScratch = (GridEntry*)tf_malloc(sizeof(GridEntry) * unitCapacity);
    p->mGrid.pBucketStart = (uint32_t*)tf_malloc(sizeof(uint32_t) * (bucketCount + 1));
    p->mGrid.pBucketCursor = (uint32_t*)tf_malloc(sizeof(uint32_t) * bucketCount);
    for (uint32_t team = 0; team < TEAM_COUNT; ++team)
        p->mGrid.pEnemyRings[team] = (uint16_t*)tf_malloc(sizeof(uint16_t) * bucketCount);

    // Declaration order is execution order within a phase.
    addSystem(p, "SteerSystem", EcsOnUpdate, SteerSystem, true, ecs_id(Position), ecs_id(Unit), ecs_id(Attack));
    addSystem(p, "AttackSystem", EcsOnUpdate, AttackSystem, false, ecs_id(Position), ecs_id(Unit), ecs_id(Attack));
    addSystem(p, "ProjectileSystem", EcsOnUpdate, ProjectileSystem, false, ecs_id(Projectile));
    addSystem(p, "HealthSystem", EcsPostUpdate, HealthSystem, false, ecs_id(Health));

    simRestart(p);
    return p;
}

void simDestroy(BattleSim* p)
{
    if (!p)
        return;
    ecs_query_fini(p->pUnitQuery);
    ecs_query_fini(p->pProjectileQuery);
    ecs_fini(p->pWorld);
    tf_free(p->mGrid.pEntries);
    tf_free(p->mGrid.pScratch);
    tf_free(p->mGrid.pBucketStart);
    tf_free(p->mGrid.pBucketCursor);
    for (uint32_t team = 0; team < TEAM_COUNT; ++team)
        tf_free(p->mGrid.pEnemyRings[team]);
    tf_free(p);
}

void simRestart(BattleSim* p)
{
    memset(p->mBoard, UNIT_NONE, sizeof(p->mBoard));
    p->mRound = 1;
    p->mGold = START_GOLD;
    p->mBattleTime = 0.0f;
    p->mResultTime = 0.0f;
    p->pMessage = "";
    generateWave(p);
    rebuildFromBoard(p);
    p->mPhase = PHASE_PREP;
}

bool simStartBattle(BattleSim* p)
{
    if (p->mPhase != PHASE_PREP)
    {
        p->pMessage = "A battle is already running";
        return false;
    }
    if (!boardHasPlayerSquad(p))
    {
        p->pMessage = "Place at least one squad first";
        return false;
    }
    p->mPhase = PHASE_BATTLE;
    p->mBattleTime = 0.0f;
    p->pMessage = "";
    return true;
}

bool simStressSetup(BattleSim* p, uint32_t unitsPerTeam)
{
    if (unitsPerTeam == 0 || 2ull * unitsPerTeam > p->mUnitCapacity)
    {
        p->pMessage = "Stress armies exceed the unit capacity";
        return false;
    }
    uint32_t cols = 0;
    uint32_t rows = 0;
    float    worldSize = 0.0f;
    stressLayout(unitsPerTeam, &cols, &rows, &worldSize);

    memset(p->mBoard, UNIT_NONE, sizeof(p->mBoard));
    ecs_delete_with(p->pWorld, ecs_id(Unit));
    ecs_delete_with(p->pWorld, ecs_id(Projectile));
    p->mProjectileCount = 0;
    setWorldSize(p, worldSize);

    // Row 0 is each army's front: melee in front, tanks behind them, ranged at the back.
    const float left = 0.5f * (worldSize - (float)cols * STRESS_SPACING);
    const float mid = 0.5f * worldSize;
    for (uint32_t k = 0; k < unitsPerTeam; ++k)
    {
        const uint32_t col = k % cols;
        const uint32_t row = k / cols;
        const UnitType type = row * 10 < rows * 6 ? UNIT_MELEE : (row * 10 < rows * 8 ? UNIT_TANK : UNIT_RANGED);
        const float    x = left + ((float)col + 0.5f) * STRESS_SPACING;
        const float    depth = 0.5f * STRESS_GAP + ((float)row + 0.5f) * STRESS_SPACING;
        spawnUnit(p, TEAM_PLAYER, type, x, mid - depth, 0xFFFF);
        spawnUnit(p, TEAM_ENEMY, type, x, mid + depth, 0xFFFF);
    }

    p->mPhase = PHASE_BATTLE;
    p->mBattleTime = 0.0f;
    p->mResultTime = 0.0f;
    p->pMessage = "";
    return true;
}

// Outside battle no systems run; keep idle/dance clocks ticking so units stay alive on screen.
static void advanceAnimationClocks(BattleSim* p, float dt)
{
    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pUnitQuery);
    while (ecs_query_next(&it))
    {
        Unit* unit = ecs_field(&it, Unit, 1);
        for (int32_t i = 0; i < it.count; ++i)
        {
            unit[i].moving = 0u;
            unit[i].attacking = 0u;
            unit[i].animTime += dt;
        }
    }
}

void simStep(BattleSim* p, float deltaTime)
{
    if (!(deltaTime > 0.0f)) // also rejects NaN
        return;
    const float dt = deltaTime < MAX_STEP ? deltaTime : MAX_STEP;

    switch (p->mPhase)
    {
    case PHASE_BATTLE:
    {
        p->mBattleTime += dt;
        {
            ScopedUSec timer(&p->mTimings.mGridUs);
            buildGrid(p);
        }
        {
            ScopedUSec timer(&p->mTimings.mSystemsUs);
            ecs_progress(p->pWorld, dt);
        }
        ++p->mTimings.mSteps;
        const uint32_t players = simGetUnitCount(p, TEAM_PLAYER);
        const uint32_t enemies = simGetUnitCount(p, TEAM_ENEMY);
        if (enemies == 0 && players > 0)
            enterResult(p, PHASE_VICTORY);
        else if (players == 0)
            enterResult(p, PHASE_DEFEAT);
        else if (p->mBattleTime >= BATTLE_TIME_LIMIT)
        {
            enterResult(p, PHASE_DEFEAT);
            p->pMessage = "Time ran out";
        }
        break;
    }
    case PHASE_VICTORY:
        advanceAnimationClocks(p, dt);
        p->mResultTime += dt;
        if (p->mResultTime >= RESULT_DURATION)
        {
            ++p->mRound;
            p->mGold += 5 + p->mRound;
            generateWave(p);
            rebuildFromBoard(p);
            p->mPhase = PHASE_PREP;
        }
        break;
    case PHASE_DEFEAT:
        advanceAnimationClocks(p, dt);
        p->mResultTime += dt;
        break;
    case PHASE_PREP:
        advanceAnimationClocks(p, dt);
        break;
    }
}

//--------------------------------------------------------------------------------------------
// Board editing
//--------------------------------------------------------------------------------------------
static const char* buyRefusal(const BattleSim* p, int32_t x, int32_t z, UnitType type)
{
    if (p->mPhase != PHASE_PREP)
        return "Squads can only be placed during prep";
    if (type < 0 || type >= UNIT_TYPE_COUNT)
        return "Pick a unit type";
    if (!cellInArena(x, z) || z >= PLAYER_ROWS)
        return "Place squads on your half (bottom)";
    if (p->mBoard[cellIndex(x, z)] != UNIT_NONE)
        return "That cell already has a squad";
    if (p->mGold < gUnitStats[type].mCost)
        return "Not enough gold";
    if (boardUnitCount(p) + squadSize(type) > MAX_UNITS)
        return "Unit cap reached";
    return NULL;
}

bool simCanBuySquad(const BattleSim* p, int32_t cellX, int32_t cellZ, UnitType type) { return buyRefusal(p, cellX, cellZ, type) == NULL; }

bool simBuySquad(BattleSim* p, int32_t cellX, int32_t cellZ, UnitType type)
{
    const char* pRefusal = buyRefusal(p, cellX, cellZ, type);
    if (pRefusal)
    {
        p->pMessage = pRefusal;
        return false;
    }
    p->mBoard[cellIndex(cellX, cellZ)] = type;
    p->mGold -= gUnitStats[type].mCost;
    p->pMessage = "";
    spawnSquad(p, cellX, cellZ, type);
    return true;
}

bool simSellSquad(BattleSim* p, int32_t cellX, int32_t cellZ)
{
    if (p->mPhase != PHASE_PREP)
    {
        p->pMessage = "Squads can only be sold during prep";
        return false;
    }
    if (!cellInArena(cellX, cellZ) || cellZ >= PLAYER_ROWS || p->mBoard[cellIndex(cellX, cellZ)] == UNIT_NONE)
    {
        p->pMessage = "Nothing of yours to sell there";
        return false;
    }
    p->mGold += gUnitStats[p->mBoard[cellIndex(cellX, cellZ)]].mCost;
    p->mBoard[cellIndex(cellX, cellZ)] = UNIT_NONE;
    p->pMessage = "";
    rebuildFromBoard(p);
    return true;
}

UnitType simGetCell(const BattleSim* p, int32_t cellX, int32_t cellZ)
{
    return cellInArena(cellX, cellZ) ? (UnitType)p->mBoard[cellIndex(cellX, cellZ)] : UNIT_NONE;
}

//--------------------------------------------------------------------------------------------
// Queries
//--------------------------------------------------------------------------------------------
Phase simGetPhase(const BattleSim* p) { return p->mPhase; }

int32_t simGetRound(const BattleSim* p) { return p->mRound; }

int32_t simGetGold(const BattleSim* p) { return p->mGold; }

const char* simGetMessage(const BattleSim* p) { return p->pMessage; }

float simGetBattleTime(const BattleSim* p) { return p->mBattleTime; }

uint32_t simGetProjectileCount(const BattleSim* p) { return p->mProjectileCount; }

uint32_t simGetUnitCapacity(const BattleSim* p) { return p->mUnitCapacity; }

float simGetWorldSize(const BattleSim* p) { return p->mWorldSize; }

void simGetTimings(const BattleSim* p, SimTimings* pOut)
{
    *pOut = p->mTimings;
    pOut->mSteerCpuUs = (int64_t)tfrg_atomic64_load_relaxed(&p->mSteerCpuUs);
}

void simResetTimings(BattleSim* p)
{
    memset(&p->mTimings, 0, sizeof(p->mTimings));
    tfrg_atomic64_store_relaxed(&p->mSteerCpuUs, 0);
}

//--------------------------------------------------------------------------------------------
// Render data
//--------------------------------------------------------------------------------------------
static const float  HEALTH_BAR_HEIGHT = 2.0f; // model units, above the 1.73-unit-tall bind pose

void simFillRenderData(BattleSim* p, RenderData* pOut)
{
    pOut->mUnitCount = 0;
    pOut->mBillboardCount = 0;

    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pUnitQuery);
    while (ecs_query_next(&it))
    {
        const Position* pos = ecs_field(&it, Position, 0);
        const Unit*     unit = ecs_field(&it, Unit, 1);
        const Health*   health = ecs_field(&it, Health, 2);
        for (int32_t i = 0; i < it.count; ++i)
        {
            ASSERT(pOut->mUnitCount < p->mUnitCapacity);
            const float   scale = gUnitStats[unit[i].type].mScale * UNIT_MODEL_SCALE;
            UnitInstance& instance = pOut->pUnits[pOut->mUnitCount];
            instance.mX = pos[i].x;
            instance.mZ = pos[i].z;
            instance.mYaw = unit[i].yaw;
            instance.mScale = scale;
            instance.mTintFlash = packTintFlash(unit[i].team, health[i].hitFlash);
            instance.mPose = 0; // the app fills it from pUnitAnims
            const bool winner = (p->mPhase == PHASE_VICTORY && unit[i].team == TEAM_PLAYER) ||
                                (p->mPhase == PHASE_DEFEAT && unit[i].team == TEAM_ENEMY);
            UnitAnim& anim = pOut->pUnitAnims[pOut->mUnitCount];
            anim.mClip = winner ? ANIM_DANCE : (unit[i].moving ? ANIM_WALK : (unit[i].attacking ? ANIM_ATTACK : ANIM_IDLE));
            anim.mTime = unit[i].animTime;
            ++pOut->mUnitCount;

            if (health[i].hp < health[i].maxHp)
            {
                ASSERT(pOut->mBillboardCount < p->mUnitCapacity + MAX_PROJECTILES);
                const float        fill = health[i].hp > 0.0f ? health[i].hp / health[i].maxHp : 0.0f;
                BillboardInstance& bar = pOut->pBillboards[pOut->mBillboardCount++];
                bar.mCenterHalfWidth = float4(pos[i].x, HEALTH_BAR_HEIGHT * scale, pos[i].z, 0.4f);
                bar.mFullColorFill = float4(0.2f, 0.9f, 0.25f, fill);
                bar.mEmptyColorHalfHeight = float4(0.55f, 0.08f, 0.08f, 0.06f);
            }
        }
    }

    it = ecs_query_iter(p->pWorld, p->pProjectileQuery);
    while (ecs_query_next(&it))
    {
        const Projectile* projectile = ecs_field(&it, Projectile, 0);
        for (int32_t i = 0; i < it.count; ++i)
        {
            ASSERT(pOut->mBillboardCount < p->mUnitCapacity + MAX_PROJECTILES);
            BillboardInstance& b = pOut->pBillboards[pOut->mBillboardCount++];
            b.mCenterHalfWidth = float4(projectile[i].x, projectile[i].y, projectile[i].z, 0.12f);
            b.mFullColorFill = float4(1.0f, 0.85f, 0.2f, 1.0f);
            b.mEmptyColorHalfHeight = float4(1.0f, 0.85f, 0.2f, 0.12f);
        }
    }
}

uint32_t simGetUnitCount(BattleSim* p, Team team)
{
    uint32_t   count = 0;
    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pUnitQuery);
    while (ecs_query_next(&it))
    {
        const Unit* unit = ecs_field(&it, Unit, 1);
        for (int32_t i = 0; i < it.count; ++i)
            if (unit[i].team == team)
                ++count;
    }
    return count;
}

//--------------------------------------------------------------------------------------------
// Test hooks
//--------------------------------------------------------------------------------------------
bool simTestSetCell(BattleSim* p, int32_t cellX, int32_t cellZ, UnitType type)
{
    if (!cellInArena(cellX, cellZ))
        return false;
    const int8_t previous = p->mBoard[cellIndex(cellX, cellZ)];
    p->mBoard[cellIndex(cellX, cellZ)] = type;
    if (boardUnitCount(p) > MAX_UNITS)
    {
        p->mBoard[cellIndex(cellX, cellZ)] = previous;
        return false;
    }
    rebuildFromBoard(p);
    return true;
}

void simTestClearBoard(BattleSim* p)
{
    memset(p->mBoard, UNIT_NONE, sizeof(p->mBoard));
    rebuildFromBoard(p);
}

void simTestSetGold(BattleSim* p, int32_t gold) { p->mGold = gold; }

void simTestSpawnUnit(BattleSim* p, Team team, UnitType type, float x, float z) { spawnUnit(p, team, type, x, z, 0xFFFF); }

void simTestSetBattleTime(BattleSim* p, float seconds) { p->mBattleTime = seconds; }

float simTestTotalHp(BattleSim* p, Team team)
{
    float      total = 0.0f;
    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pUnitQuery);
    while (ecs_query_next(&it))
    {
        const Unit*   unit = ecs_field(&it, Unit, 1);
        const Health* health = ecs_field(&it, Health, 2);
        for (int32_t i = 0; i < it.count; ++i)
            if (unit[i].team == team)
                total += health[i].hp;
    }
    return total;
}

bool simTestAllPositionsFinite(BattleSim* p)
{
    bool       finite = true;
    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pUnitQuery);
    while (ecs_query_next(&it))
    {
        const Position* pos = ecs_field(&it, Position, 0);
        for (int32_t i = 0; i < it.count; ++i)
            finite = finite && isfinite(pos[i].x) && isfinite(pos[i].z);
    }
    return finite;
}

float simTestMinUnitDistance(BattleSim* p, Team team)
{
    Position*  points = (Position*)tf_malloc(sizeof(Position) * p->mUnitCapacity);
    uint32_t   count = 0;
    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pUnitQuery);
    while (ecs_query_next(&it))
    {
        const Position* pos = ecs_field(&it, Position, 0);
        const Unit*     unit = ecs_field(&it, Unit, 1);
        for (int32_t i = 0; i < it.count; ++i)
            if (unit[i].team == team)
                points[count++] = pos[i];
    }
    float minD2 = FLT_MAX;
    for (uint32_t a = 0; a < count; ++a)
        for (uint32_t b = a + 1; b < count; ++b)
        {
            const float dx = points[a].x - points[b].x;
            const float dz = points[a].z - points[b].z;
            if (dx * dx + dz * dz < minD2)
                minD2 = dx * dx + dz * dz;
        }
    tf_free(points);
    return sqrtf(minD2);
}

SimSearchStats simTestNearestEnemySearch(BattleSim* p)
{
    buildGrid(p);
    const SpatialGrid& g = p->mGrid;
    SimSearchStats     stats = {};
    uint64_t           visited = 0;
    for (uint32_t i = 0; i < g.mCount; ++i)
    {
        const GridEntry& self = g.pEntries[i];
        uint32_t         buckets = 0;
        const int32_t    found = findNearestEnemy(g, self.mX, self.mZ, self.mTeam, &buckets);
        visited += buckets;
        ++stats.mSearches;

        float bestD2 = FLT_MAX;
        for (uint32_t j = 0; j < g.mCount; ++j)
            if (g.pEntries[j].mTeam != self.mTeam)
            {
                const float dx = g.pEntries[j].mX - self.mX;
                const float dz = g.pEntries[j].mZ - self.mZ;
                bestD2 = fminf(bestD2, dx * dx + dz * dz);
            }
        float foundD2 = FLT_MAX;
        if (found >= 0)
        {
            const float dx = g.pEntries[found].mX - self.mX;
            const float dz = g.pEntries[found].mZ - self.mZ;
            foundD2 = dx * dx + dz * dz;
        }
        stats.mMismatches += foundD2 == bestD2 ? 0u : 1u;
    }
    stats.mAverageBucketsVisited = stats.mSearches ? (float)visited / (float)stats.mSearches : 0.0f;
    return stats;
}
