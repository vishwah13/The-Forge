#pragma once

#include "AutoBattlerSim.h"

class Rig;
struct GeometryData;

// Every animation the Stormtrooper can play, pre-sampled into skinning palettes (one mat4 per mesh
// joint per frame) so each instance can pick its own frame on the GPU.
//   ANIM_DANCE: the Stormtrooper's own dance.ozz.
//   ANIM_IDLE / ANIM_WALK: the stickFigure's stand.ozz / walk.ozz, retargeted onto the Stormtrooper.
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

// pTargetRig: the Stormtrooper skeleton; pGeom: its mesh data (joint remaps, inverse bind poses).
bool bakeAnimations(Rig* pTargetRig, const GeometryData* pGeom, BakedAnimations* pOut);
void freeBakedAnimations(BakedAnimations* pBaked);

// Index of the first palette matrix for `clip` at `time` seconds (looping).
uint32_t bakedPaletteOffset(const BakedAnimations& baked, AnimClip clip, float time);
