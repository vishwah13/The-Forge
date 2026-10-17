#pragma once

#include "AutoBattlerSim.h"

class Rig;
struct GeometryData;

// Every animation the unit model can play, pre-sampled into model-space joint poses (one mat4 per rig
// joint per frame) so each instance can pick its own frame on the GPU. Each mesh applies its own joint
// remap and inverse bind poses (SkinJoint), so every mesh skinned to the same skeleton shares one table.
// The clips are authored for the model's own skeleton (see Tools/MixamoImport), so they are sampled as they are.
struct BakedClip
{
    uint32_t mFirstFrame;
    uint32_t mFrameCount;
};

struct BakedAnimations
{
    mat4*     pPalettes;   // mFrameTotal * mJointCount pose matrices
    uint32_t  mJointCount; // rig joints per frame
    uint32_t  mFrameTotal;
    BakedClip mClips[ANIM_CLIP_COUNT];
};

// One per mesh joint: which rig joint drives it and the mesh's inverse bind pose for it. Must match
// SkinJoint in Shaders/FSL/AutoBattler.h.fsl.
struct SkinJoint
{
    mat4     mInverseBind;
    uint32_t mRigJoint;
    uint32_t mPad[3];
};

// pRig: the skeleton; pClipFiles: one .ozz per AnimClip under RD_ANIMATIONS.
bool bakeAnimations(Rig* pRig, const char* const pClipFiles[ANIM_CLIP_COUNT], BakedAnimations* pOut);

// Skin table for one mesh. Fails if a joint remap points outside the rig. Free with tf_free.
bool buildSkinJoints(const GeometryData* pGeom, uint32_t rigJointCount, SkinJoint** ppOut, uint32_t* pCount);

// CPU reference of the vertex shader's per-influence matrix.
inline mat4 skinnedJointMatrix(const mat4* pPoses, const SkinJoint* pSkin, uint32_t poseOffset, uint32_t meshJoint)
{
    return pPoses[poseOffset + pSkin[meshJoint].mRigJoint] * pSkin[meshJoint].mInverseBind;
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
