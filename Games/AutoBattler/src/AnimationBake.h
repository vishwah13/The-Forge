#pragma once

#include "AutoBattlerSim.h"

class Rig;
struct GeometryData;

// Every animation the unit model can play, pre-sampled into skinning palettes (one mat4 per mesh
// joint per frame) so each instance can pick its own frame on the GPU. The clips are authored for the
// model's own skeleton (see Tools/MixamoImport), so they are sampled as they are.
struct BakedClip
{
    uint32_t mFirstFrame;
    uint32_t mFrameCount;
};

struct BakedAnimations
{
    mat4*     pPalettes; // mFrameTotal * mJointCount matrices
    uint32_t  mJointCount;
    uint32_t  mFrameTotal;
    BakedClip mClips[ANIM_CLIP_COUNT];
};

// pRig: the model's skeleton; pGeom: its mesh data (joint remaps, inverse bind poses);
// pClipFiles: one .ozz per AnimClip under RD_ANIMATIONS.
bool bakeAnimations(Rig* pRig, const GeometryData* pGeom, const char* const pClipFiles[ANIM_CLIP_COUNT], BakedAnimations* pOut);
void freeBakedAnimations(BakedAnimations* pBaked);

// Where a unit's pose starts in the baked table for `clip` at `time` seconds (looping), and how far it
// is towards the next frame.
struct BakedPose
{
    uint32_t mOffset; // first matrix
    float    mBlend;  // [0,1) towards the frame mJointCount matrices later
};
BakedPose bakedPose(const BakedAnimations& baked, AnimClip clip, float time);
