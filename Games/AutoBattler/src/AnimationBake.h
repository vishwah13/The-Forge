#pragma once

#include "AutoBattlerSim.h"
#include "UnitInstance.h"

class Rig;
struct GeometryData;

// Rows 0-2 of an affine matrix (row 3 is always 0 0 0 1): 48 bytes instead of 64. Must match Affine in
// Shaders/FSL/AutoBattler.h.fsl.
struct Affine
{
    float4 mRows[3];
};
static_assert(sizeof(Affine) == 48, "must match the FSL Affine");

inline Affine toAffine(const mat4& m) { return { { v4ToF4(m.getRow(0)), v4ToF4(m.getRow(1)), v4ToF4(m.getRow(2)) } }; }

inline float3 affineMul(const Affine& a, const float4& v)
{
    const vec4 x = f4Tov4(v);
    return float3((float)dot(f4Tov4(a.mRows[0]), x), (float)dot(f4Tov4(a.mRows[1]), x), (float)dot(f4Tov4(a.mRows[2]), x));
}

// Every animation the unit model can play, pre-sampled into model-space joint poses (one Affine per rig
// joint per frame) so each instance can pick its own frame on the GPU. Each mesh applies its own joint
// remap and inverse bind poses (SkinJoint), so every mesh skinned to the same skeleton shares one table.
// The clips are authored for the model's own skeleton (see Tools/MixamoImport), so they are sampled as they are.
static const float BAKE_FPS = 30.0f; // baked frames per second of animation

// Each clip stores mFrameCount frames plus a copy of its first frame, so blending to the next frame never
// leaves the clip.
struct BakedClip
{
    uint32_t mFirstFrame;
    uint32_t mFrameCount; // distinct frames
};

struct BakedAnimations
{
    Affine*   pPalettes;   // mFrameTotal * mJointCount pose matrices
    uint32_t  mJointCount; // rig joints per frame
    uint32_t  mFrameTotal; // stored frames, wrap frames included
    BakedClip mClips[ANIM_CLIP_COUNT];
};

// One per mesh joint: which rig joint drives it and the mesh's inverse bind pose for it. Must match
// SkinJoint in Shaders/FSL/AutoBattler.h.fsl.
struct SkinJoint
{
    Affine   mInverseBind;
    uint32_t mRigJoint;
    uint32_t mPad[3];
};
static_assert(sizeof(SkinJoint) == 64, "must match the FSL SkinJoint");

// pRig: the skeleton; pClipFiles: one .ozz per AnimClip under RD_ANIMATIONS.
bool bakeAnimations(Rig* pRig, const char* const pClipFiles[ANIM_CLIP_COUNT], BakedAnimations* pOut);

// Skin table for one mesh. Fails if a joint remap points outside the rig. Free with tf_free.
bool buildSkinJoints(const GeometryData* pGeom, uint32_t rigJointCount, SkinJoint** ppOut, uint32_t* pCount);

// CPU reference of the vertex shader's per-influence transform: pose(rig joint) * inverseBind * v.
inline float3 skinnedPoint(const Affine* pPoses, const SkinJoint* pSkin, uint32_t poseOffset, uint32_t meshJoint, const float4& v)
{
    const float3 bind = affineMul(pSkin[meshJoint].mInverseBind, v);
    return affineMul(pPoses[poseOffset + pSkin[meshJoint].mRigJoint], float4(bind.x, bind.y, bind.z, v.w));
}
void freeBakedAnimations(BakedAnimations* pBaked);

// Where a unit's pose starts in the baked table for `clip` at `time` seconds (looping), and how far it
// is towards the next frame.
struct BakedPose
{
    uint32_t mOffset; // first matrix
    float    mBlend;  // [0,1) towards the frame mJointCount matrices later
};
BakedPose bakedPose(const BakedAnimations& baked, AnimClip clip, float time);

// True when every pose offset in the table fits UnitInstance::mPose (UNIT_POSE_OFFSET_BITS).
inline bool bakedPosesFitInstance(const BakedAnimations& baked)
{
    return (uint64_t)baked.mFrameTotal * baked.mJointCount <= (1ull << UNIT_POSE_OFFSET_BITS);
}
