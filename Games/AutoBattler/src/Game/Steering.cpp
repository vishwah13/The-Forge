#include "SimInternal.h"

#include <math.h>

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

// Steers one chunk of units. Reads other units only from the grid snapshot (and read-only lookups of
// their GridSlot), writes only its own units, so chunks run in parallel and in any order.
static void steerChunk(void* pUser, uint64_t)
{
    const SteerChunk&   c = *(const SteerChunk*)pUser;
    BattleSim*          p = c.p;
    const int64_t       startUs = getUSec(true);
    const SpatialGrid&  g = p->mGrid;
    const ecs_world_t*  world = p->pWorld;
    Position*           pos = c.pPos;
    Unit*               unit = c.pUnit;
    Attack*             attack = c.pAttack;
    const ecs_entity_t* entities = c.pEntities;
    const float         dt = c.mDt;

    for (int32_t i = 0; i < c.mCount; ++i)
    {
        const UnitStats& stats = gUnitStats[unit[i].type];
        Attack&          a = attack[i];

        const GridEntry* pTarget = lookupTarget(world, g, a.target);
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
        separation(g, entities[i], pos[i].x, pos[i].z, unit[i].radius, &sx, &sz);
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

// Takes chunks off the shared counter until every chunk has been steered.
static void steerWorker(void* pUser, uint64_t)
{
    BattleSim* p = (BattleSim*)pUser;
    for (;;)
    {
        const uint32_t chunk = tfrg_atomic32_add_relaxed(&p->mNextSteerChunk, 1);
        if (chunk >= p->mSteerChunkCount)
            return;
        steerChunk(&p->pSteerChunks[chunk], 0);
    }
}

// Splits every unit table into chunks and steers them on the thread pool, the calling thread helping.
void steerUnits(BattleSim* p, float dt)
{
    uint32_t   chunkCount = 0;
    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pSteerQuery);
    while (ecs_query_next(&it))
    {
        Position* pos = ecs_field(&it, Position, 0);
        Unit*     unit = ecs_field(&it, Unit, 1);
        Attack*   attack = ecs_field(&it, Attack, 2);
        for (int32_t first = 0; first < it.count; first += STEER_CHUNK_SIZE)
        {
            if (chunkCount == p->mSteerChunkCapacity)
            {
                p->mSteerChunkCapacity = p->mSteerChunkCapacity ? 2 * p->mSteerChunkCapacity : 64;
                p->pSteerChunks = (SteerChunk*)tf_realloc(p->pSteerChunks, sizeof(SteerChunk) * p->mSteerChunkCapacity);
            }
            SteerChunk& c = p->pSteerChunks[chunkCount++];
            c.p = p;
            c.pPos = pos + first;
            c.pUnit = unit + first;
            c.pAttack = attack + first;
            c.pEntities = it.entities + first;
            c.mCount = it.count - first < STEER_CHUNK_SIZE ? it.count - first : STEER_CHUNK_SIZE;
            c.mDt = dt;
        }
    }
    p->mSteerChunkCount = chunkCount;
    tfrg_atomic32_store_relaxed(&p->mNextSteerChunk, 0);
    // One task per worker; each worker and this thread take chunks until none are left. (Not one task per
    // chunk with threadSystemAssist: Assist blocks waiting for work once the queue is empty.)
    if (p->pThreads)
        threadSystemAddTasks(p->pThreads, steerWorker, p->mWorkerCount, 0, p);
    steerWorker(p, 0);
    if (p->pThreads)
        threadSystemWaitIdle(p->pThreads);
}
