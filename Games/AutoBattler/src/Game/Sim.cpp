#include "SimInternal.h"
#include "UnitInstance.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "../../../../Common_3/Utilities/Interfaces/ILog.h"

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

ECS_COMPONENT_DECLARE(Position);
ECS_COMPONENT_DECLARE(Unit);
ECS_COMPONENT_DECLARE(Health);
ECS_COMPONENT_DECLARE(Attack);
ECS_COMPONENT_DECLARE(GridSlot);
ECS_COMPONENT_DECLARE(Projectile);


// Spawning
void spawnUnit(BattleSim* p, Team team, UnitType type, float x, float z)
{
    const UnitStats&   stats = gUnitStats[type];
    const ecs_entity_t e = ecs_new(p->pWorld);

    const Position pos = { x, z };
    const Unit     unit = { (uint8_t)team, (uint8_t)type, UNIT_RADIUS * stats.mScale, team == TEAM_PLAYER ? 0.0f : PI,
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

static void setWorldSize(BattleSim* p, float worldSize)
{
    p->mWorldSize = worldSize;
    p->mGrid.mDim = (int32_t)ceilf(worldSize / GRID_BUCKET_SIZE);
}

void addSystem(BattleSim* p, const char* pName, ecs_entity_t phase, ecs_iter_action_t callback, bool multiThreaded, ecs_id_t c0,
               ecs_id_t c1, ecs_id_t c2)
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
    if (threadCount > 1)
    {
        ThreadSystemInitDesc threadDesc = gThreadSystemInitDescDefault;
        threadDesc.threadCount = threadCount - 1; // the caller steers too
        threadDesc.threadName = "AutoBattler steer";
        if (threadSystemInit(&p->pThreads, &threadDesc) && p->pThreads)
        {
            ThreadSystemInfo info = {};
            threadSystemGetInfo(p->pThreads, &info);
            p->mWorkerCount = (uint32_t)info.threadCount; // clamped to the core count
        }
        else
            p->pThreads = NULL; // falls back to steering on the calling thread
    }

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

    ecs_query_desc_t steerQuery = {};
    steerQuery.terms[0].id = ecs_id(Position);
    steerQuery.terms[1].id = ecs_id(Unit);
    steerQuery.terms[2].id = ecs_id(Attack);
    p->pSteerQuery = ecs_query_init(p->pWorld, &steerQuery);

    // Size the grid for the largest world simClear allows.
    const uint32_t maxDim = (uint32_t)ceilf(MAX_WORLD_SIZE / GRID_BUCKET_SIZE);
    const uint32_t bucketCount = maxDim * maxDim;
    p->mUnitCapacity = unitCapacity;
    p->mGrid.pEntries = (GridEntry*)tf_malloc(sizeof(GridEntry) * unitCapacity);
    p->mGrid.pScratch = (GridEntry*)tf_malloc(sizeof(GridEntry) * unitCapacity);
    p->mGrid.pBucketStart = (uint32_t*)tf_malloc(sizeof(uint32_t) * (bucketCount + 1));
    p->mGrid.pBucketCursor = (uint32_t*)tf_malloc(sizeof(uint32_t) * bucketCount);
    for (uint32_t team = 0; team < TEAM_COUNT; ++team)
        p->mGrid.pEnemyRings[team] = (uint16_t*)tf_malloc(sizeof(uint16_t) * bucketCount);

    // Declaration order is execution order within a phase.
    combatRegisterSystems(p);
    healthRegisterSystems(p);

    simClear(p, 80.0f);
    return p;
}

void simDestroy(BattleSim* p)
{
    if (!p)
        return;
    ecs_query_fini(p->pUnitQuery);
    ecs_query_fini(p->pProjectileQuery);
    ecs_query_fini(p->pSteerQuery);
    ecs_fini(p->pWorld);
    if (p->pThreads)
        threadSystemExit(&p->pThreads, &gThreadSystemExitDescDefault);
    tf_free(p->pSteerChunks);
    tf_free(p->mGrid.pEntries);
    tf_free(p->mGrid.pScratch);
    tf_free(p->mGrid.pBucketStart);
    tf_free(p->mGrid.pBucketCursor);
    for (uint32_t team = 0; team < TEAM_COUNT; ++team)
        tf_free(p->mGrid.pEnemyRings[team]);
    tf_free(p);
}

void simStep(BattleSim* p, float deltaTime)
{
    if (!(deltaTime > 0.0f)) // also rejects NaN
        return;
    const float dt = deltaTime < MAX_STEP ? deltaTime : MAX_STEP;

    p->mTime += dt;
    {
        ScopedUSec timer(&p->mTimings.mGridUs);
        buildGrid(p);
    }
    {
        ScopedUSec timer(&p->mTimings.mSteerUs);
        steerUnits(p, dt);
    }
    {
        ScopedUSec timer(&p->mTimings.mSystemsUs);
        ecs_progress(p->pWorld, dt);
    }
    ++p->mTimings.mSteps;
}

void simClear(BattleSim* p, float worldSize)
{
    ecs_delete_with(p->pWorld, ecs_id(Unit));
    ecs_delete_with(p->pWorld, ecs_id(Projectile));
    p->mProjectileCount = 0;
    p->mTime = 0.0f;
    p->mCelebratingTeam = -1;
    setWorldSize(p, worldSize > 0.0f && worldSize < MAX_WORLD_SIZE ? worldSize : MAX_WORLD_SIZE);
}

bool simSpawnUnit(BattleSim* p, Team team, UnitType type, float x, float z)
{
    // Positive tests so NaN is refused too.
    if (!(x >= 0.0f && x < p->mWorldSize && z >= 0.0f && z < p->mWorldSize))
        return false;
    if (!(type >= 0 && type < UNIT_TYPE_COUNT) || team >= TEAM_COUNT) // indexes gUnitStats and the per-team grid rings
        return false;
    if (simGetUnitCount(p, TEAM_PLAYER) + simGetUnitCount(p, TEAM_ENEMY) >= p->mUnitCapacity)
        return false;
    spawnUnit(p, team, type, x, z);
    return true;
}

float simGetTime(const BattleSim* p) { return p->mTime; }

void simSetCelebratingTeam(BattleSim* p, int32_t team) { p->mCelebratingTeam = team; }

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

// Render data
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
            const bool winner = (int32_t)unit[i].team == p->mCelebratingTeam;
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
