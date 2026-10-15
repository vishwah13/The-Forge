#pragma once

#include <stdint.h>

// Levels of detail for the unit mesh: LOD0 is the mesh itself, LOD1 and LOD2 are simplified index
// lists over the same vertices (so skin weights and the baked animation still apply). All levels live
// in one index array, one after another.
static const uint32_t UNIT_LOD_COUNT = 3;

struct UnitLods
{
    uint32_t* pIndices; // all levels, mTotalIndexCount entries
    uint32_t  mTotalIndexCount;
    uint32_t  mFirstIndex[UNIT_LOD_COUNT];
    uint32_t  mIndexCount[UNIT_LOD_COUNT];
};

// positions: vertexCount float3s, positionStride bytes apart. Returns false on empty or invalid input.
bool buildUnitLods(const uint32_t* pIndices, uint32_t indexCount, const float* pPositions, uint32_t vertexCount, uint32_t positionStride,
                   UnitLods* pOut);
void freeUnitLods(UnitLods* pLods);
