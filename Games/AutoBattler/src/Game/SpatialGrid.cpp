#include "SimInternal.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

// Spatial grid
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

void buildGrid(BattleSim* p)
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

const GridEntry* lookupTarget(const ecs_world_t* pWorld, const SpatialGrid& g, ecs_entity_t target)
{
    if (!target || !ecs_is_alive(pWorld, target))
        return NULL;
    const GridSlot* pSlot = ecs_get(pWorld, target, GridSlot);
    if (!pSlot || pSlot->slot >= g.mCount || g.pEntries[pSlot->slot].mEntity != target)
        return NULL;
    return &g.pEntries[pSlot->slot];
}

// Searches rings of buckets outward and stops once no closer enemy can exist. Returns a slot or -1.
int32_t findNearestEnemy(const SpatialGrid& g, float x, float z, uint32_t team, uint32_t* pBucketsVisited)
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

void separation(const SpatialGrid& g, ecs_entity_t self, float x, float z, float radius, float* pOutX, float* pOutZ)
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
