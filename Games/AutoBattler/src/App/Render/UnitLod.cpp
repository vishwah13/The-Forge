#include "UnitLod.h"

#include <string.h>

#include "../../../../../Common_3/Tools/ThirdParty/OpenSource/meshoptimizer/src/meshoptimizer.h"

#include "../../../../../Common_3/Utilities/Interfaces/IMemory.h"

// Share of LOD0's triangles each level aims for, and the most it may deviate from the original
// surface, as a fraction of the mesh's extent.
static const float LOD_TRIANGLE_SHARE[UNIT_LOD_COUNT] = { 1.0f, 0.25f, 0.06f };
static const float LOD_MAX_ERROR[UNIT_LOD_COUNT] = { 0.0f, 0.02f, 0.08f };

bool buildUnitLods(const uint32_t* pIndices, uint32_t indexCount, const float* pPositions, uint32_t vertexCount, uint32_t positionStride,
                   UnitLods* pOut)
{
    memset(pOut, 0, sizeof(*pOut));
    if (!pIndices || !pPositions || indexCount == 0 || indexCount % 3 != 0 || vertexCount == 0)
        return false;

    // No level is larger than LOD0, so this holds all of them.
    uint32_t* pPacked = (uint32_t*)tf_malloc(sizeof(uint32_t) * indexCount * UNIT_LOD_COUNT);
    uint32_t* pScratch = (uint32_t*)tf_malloc(sizeof(uint32_t) * indexCount);
    memcpy(pPacked, pIndices, sizeof(uint32_t) * indexCount);
    pOut->mIndexCount[0] = indexCount;
    uint32_t total = indexCount;

    for (uint32_t lod = 1; lod < UNIT_LOD_COUNT; ++lod)
    {
        const size_t target = (size_t)((float)indexCount * LOD_TRIANGLE_SHARE[lod]) / 3 * 3;
        size_t       count = meshopt_simplify(pScratch, pIndices, indexCount, pPositions, vertexCount, positionStride, target,
                                              LOD_MAX_ERROR[lod]);
        // Texture seams stop the topology-preserving simplifier early (~35% on the Stormtrooper); fall
        // back to the sloppy one, which ignores topology, when it did not even halve the previous level.
        // Its output can shade darker, which is acceptable at the distances LOD2 is used.
        if (count * 2 > pOut->mIndexCount[lod - 1])
            count = meshopt_simplifySloppy(pScratch, pIndices, indexCount, pPositions, vertexCount, positionStride, target,
                                           LOD_MAX_ERROR[lod]);
        meshopt_optimizeVertexCache(pPacked + total, pScratch, count, vertexCount);
        pOut->mFirstIndex[lod] = total;
        pOut->mIndexCount[lod] = (uint32_t)count;
        total += (uint32_t)count;
    }

    tf_free(pScratch);
    pOut->pIndices = pPacked;
    pOut->mTotalIndexCount = total;
    return true;
}

void freeUnitLods(UnitLods* pLods)
{
    tf_free(pLods->pIndices);
    memset(pLods, 0, sizeof(*pLods));
}
