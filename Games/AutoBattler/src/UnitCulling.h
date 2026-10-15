#pragma once

#include "../../../Common_3/Utilities/Math/MathTypes.h"

// Per-unit frustum culling. The GPU cull pass (Shaders/FSL/Cull.comp.fsl) runs the same test; this is
// its reference implementation, checked by the self test and against the GPU's draw count.

// Bounding sphere around the Stormtrooper in model units (bind pose ~3.7 units tall), padded for the
// arms reaching out while walking and dancing. The instance matrix scales it to world size.
static const float UNIT_BOUND_CENTER_Y = 1.85f;
static const float UNIT_BOUND_RADIUS = 2.6f;

// Six inward-facing, normalised planes (xyz normal, w distance) from a column-vector viewProj with
// D3D clip depth 0..w (either depth direction).
inline void frustumPlanesFromViewProj(const mat4& viewProj, float4 outPlanes[6])
{
    // Row i of viewProj dotted with a point gives its clip coordinate i (Gribb-Hartmann).
    vec4 rows[4];
    for (int i = 0; i < 4; ++i)
        rows[i] = viewProj.getRow(i);
    const vec4 planes[6] = {
        rows[3] + rows[0], // -w <= x
        rows[3] - rows[0], //  x <= w
        rows[3] + rows[1], // -w <= y
        rows[3] - rows[1], //  y <= w
        rows[2],           //  0 <= z
        rows[3] - rows[2], //  z <= w
    };
    for (int i = 0; i < 6; ++i)
        outPlanes[i] = v4ToF4(planes[i] / (float)length(planes[i].getXYZ()));
}

// Camera distance, in bounding radii, at which a unit switches to LOD1 and LOD2 (see UnitLod.h).
// At 1080p with the 60 degree camera that is roughly 100 px and 35 px of unit height.
static const float UNIT_LOD_DISTANCES[2] = { 25.0f, 70.0f };

// True unless the unit's bounding sphere lies entirely outside one plane.
inline bool unitInFrustum(const float4 planes[6], const mat4& world)
{
    const vec3  center = (world[1] * UNIT_BOUND_CENTER_Y + world[3]).getXYZ();
    const float radius = UNIT_BOUND_RADIUS * (float)length(world[0].getXYZ());
    for (int i = 0; i < 6; ++i)
        if ((float)dot(f3Tov3(planes[i].getXYZ()), center) + planes[i].w < -radius)
            return false;
    return true;
}

// -1 when culled, otherwise the level of detail to draw the unit with.
inline int32_t unitLod(const float4 planes[6], const float3& cameraPos, const mat4& world)
{
    if (!unitInFrustum(planes, world))
        return -1;
    const vec3  center = (world[1] * UNIT_BOUND_CENTER_Y + world[3]).getXYZ();
    const float radius = UNIT_BOUND_RADIUS * (float)length(world[0].getXYZ());
    const float distance = (float)length(center - f3Tov3(cameraPos)) / radius;
    return distance >= UNIT_LOD_DISTANCES[1] ? 2 : (distance >= UNIT_LOD_DISTANCES[0] ? 1 : 0);
}
