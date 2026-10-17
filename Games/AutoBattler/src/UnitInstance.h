#pragma once

#include "AutoBattlerSim.h"

#include <math.h>

// What each unit sends to the GPU every frame. Must match UnitInstance in Shaders/FSL/AutoBattler.h.fsl:
// scalar members only, so the Vulkan (std430) stride is 24 bytes like here. The shaders rebuild the world
// matrix with the same formula as unitWorldMatrix().
struct UnitInstance
{
    float    mX, mZ;     // world position on the ground (units stand at y = 0)
    float    mYaw;       // radians about +y, same sense as mat4::rotationY
    float    mScale;     // UNIT_MODEL_SCALE * type scale
    uint32_t mTintFlash; // bits 0-7 team (index into FrameUniforms.teamColors), 8-15 hit flash unorm8
    uint32_t mPose;      // bits 0-23 first matrix of the unit's pose in the baked table, 24-31 blend to the next frame unorm8
};
static_assert(sizeof(UnitInstance) == 24, "must match the FSL UnitInstance");

static const uint32_t UNIT_POSE_OFFSET_BITS = 24; // must match UNIT_POSE_OFFSET_MASK in Shaders/FSL/AutoBattler.h.fsl

inline uint32_t packUnorm8(float v) { return (uint32_t)(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

inline uint32_t packTintFlash(uint32_t team, float flash) { return (team & 0xffu) | (packUnorm8(flash) << 8); }
inline uint32_t unitTeam(const UnitInstance& u) { return u.mTintFlash & 0xffu; }
inline float    unitFlash(const UnitInstance& u) { return (float)((u.mTintFlash >> 8) & 0xffu) / 255.0f; }

inline uint32_t packUnitPose(uint32_t offset, float blend)
{
    ASSERT(offset < (1u << UNIT_POSE_OFFSET_BITS));
    return offset | (packUnorm8(blend) << UNIT_POSE_OFFSET_BITS);
}
inline uint32_t unitPoseOffset(uint32_t pose) { return pose & ((1u << UNIT_POSE_OFFSET_BITS) - 1u); }
inline float    unitPoseBlend(uint32_t pose) { return (float)(pose >> UNIT_POSE_OFFSET_BITS) / 255.0f; }

// Reference for the shaders: translation(x, 0, z) * rotationY(yaw) * scale(scale), written out.
inline mat4 unitWorldMatrix(const UnitInstance& u)
{
    const float c = cosf(u.mYaw) * u.mScale;
    const float s = sinf(u.mYaw) * u.mScale;
    return mat4(vec4(c, 0.0f, -s, 0.0f), vec4(0.0f, u.mScale, 0.0f, 0.0f), vec4(s, 0.0f, c, 0.0f), vec4(u.mX, 0.0f, u.mZ, 1.0f));
}
