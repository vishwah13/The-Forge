#include "AnimationBake.h"

#include <math.h>
#include <string.h>

#include "../../../Common_3/Resources/AnimationSystem/Animation/AnimatedObject.h"
#include "../../../Common_3/Resources/AnimationSystem/Animation/Animation.h"
#include "../../../Common_3/Resources/AnimationSystem/Animation/Clip.h"
#include "../../../Common_3/Resources/AnimationSystem/Animation/ClipController.h"
#include "../../../Common_3/Resources/AnimationSystem/Animation/Rig.h"
#include "../../../Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"
#include "../../../Common_3/Utilities/Interfaces/ILog.h"

#include "../../../Common_3/Utilities/Interfaces/IMemory.h"


// Plays one clip on one rig at arbitrary times.
struct ClipSampler
{
    Clip           mClip;
    ClipController mController;
    Animation      mAnimation;
    AnimatedObject mObject;

    void init(Rig* pRig, const char* pFileName)
    {
        mClip.Initialize(RD_ANIMATIONS, pFileName, pRig);
        mController.Initialize(mClip.GetDuration());
        mController.mLoop = true;
        AnimationDesc desc{};
        desc.mRig = pRig;
        desc.mNumLayers = 1;
        desc.mLayerProperties[0].mClip = &mClip;
        desc.mLayerProperties[0].mClipController = &mController;
        mAnimation.Initialize(desc);
        mObject.Initialize(pRig, &mAnimation);
    }

    void exit()
    {
        mObject.Exit();
        mAnimation.Exit();
        mClip.Exit();
    }

    uint32_t frameCount()
    {
        const uint32_t frames = (uint32_t)ceilf(mClip.GetDuration() * BAKE_FPS);
        return frames > 0 ? frames : 1;
    }

    // Joint model matrices of the clip at `time` seconds.
    const Matrix4* sample(float time)
    {
        mController.SetTimeRatioHard(time); // sets the time and pauses; Update(0) then samples exactly there
        mObject.Update(0.0f);
        mObject.ComputePose(mObject.mRootTransform);
        return &mObject.mJointWorldMats[0];
    }
};

bool bakeAnimations(Rig* pRig, const char* const pClipFiles[ANIM_CLIP_COUNT], BakedAnimations* pOut)
{
    memset(pOut, 0, sizeof(*pOut));
    pOut->mJointCount = pRig->mNumJoints;

    ClipSampler samplers[ANIM_CLIP_COUNT];
    for (uint32_t c = 0; c < ANIM_CLIP_COUNT; ++c)
    {
        samplers[c].init(pRig, pClipFiles[c]);
        pOut->mClips[c].mFirstFrame = pOut->mFrameTotal;
        pOut->mClips[c].mFrameCount = samplers[c].frameCount();
        pOut->mFrameTotal += pOut->mClips[c].mFrameCount + 1; // + wrap frame
    }
    pOut->pPalettes = (Affine*)tf_memalign(16, sizeof(Affine) * pOut->mFrameTotal * pOut->mJointCount);

    for (uint32_t c = 0; c < ANIM_CLIP_COUNT; ++c)
    {
        const BakedClip& clip = pOut->mClips[c];
        for (uint32_t f = 0; f < clip.mFrameCount; ++f)
        {
            const Matrix4* pJointModel = samplers[c].sample((float)f / BAKE_FPS);
            Affine*        pDst = &pOut->pPalettes[(clip.mFirstFrame + f) * pOut->mJointCount];
            for (uint32_t j = 0; j < pOut->mJointCount; ++j)
                pDst[j] = toAffine(pJointModel[j]);
        }
        memcpy(&pOut->pPalettes[(clip.mFirstFrame + clip.mFrameCount) * pOut->mJointCount], &pOut->pPalettes[clip.mFirstFrame * pOut->mJointCount],
               sizeof(Affine) * pOut->mJointCount);
        LOGF(eINFO, "AnimationBake: %s baked %u frames", pClipFiles[c], clip.mFrameCount);
    }

    bool         ok = true;
    const float* pFloats = (const float*)pOut->pPalettes;
    for (uint64_t i = 0; i < (uint64_t)pOut->mFrameTotal * pOut->mJointCount * 12u && ok; ++i)
        ok = isfinite(pFloats[i]) != 0;
    if (!ok)
        LOGF(eERROR, "AnimationBake: non-finite palette");
    LOGF(eINFO, "AnimationBake: %u frames x %u joints = %u KB", pOut->mFrameTotal, pOut->mJointCount,
         (uint32_t)(sizeof(Affine) * pOut->mFrameTotal * pOut->mJointCount / 1024));

    for (uint32_t c = 0; c < ANIM_CLIP_COUNT; ++c)
        samplers[c].exit();
    return ok;
}

bool buildSkinJoints(const GeometryData* pGeom, uint32_t rigJointCount, SkinJoint** ppOut, uint32_t* pCount)
{
    SkinJoint* pSkin = (SkinJoint*)tf_memalign(16, sizeof(SkinJoint) * pGeom->mJointCount);
    for (uint32_t i = 0; i < pGeom->mJointCount; ++i)
    {
        if (pGeom->pJointRemaps[i] >= rigJointCount)
        {
            LOGF(eERROR, "AnimationBake: mesh joint %u maps to rig joint %u of %u", i, pGeom->pJointRemaps[i], rigJointCount);
            tf_free(pSkin);
            return false;
        }
        pSkin[i] = {};
        pSkin[i].mInverseBind = toAffine(pGeom->pInverseBindPoses[i]);
        pSkin[i].mRigJoint = pGeom->pJointRemaps[i];
    }
    *ppOut = pSkin;
    *pCount = pGeom->mJointCount;
    return true;
}

void freeBakedAnimations(BakedAnimations* pBaked)
{
    tf_free(pBaked->pPalettes);
    pBaked->pPalettes = NULL;
}

BakedPose bakedPose(const BakedAnimations& baked, AnimClip clip, float time)
{
    const BakedClip& c = baked.mClips[clip];
    const float      frames = time > 0.0f ? time * BAKE_FPS : 0.0f;
    const float      whole = floorf(frames);
    const uint32_t   frame = (uint32_t)whole % c.mFrameCount; // integer modulo: fmodf is slow for 64k units a frame
    return { (c.mFirstFrame + frame) * baked.mJointCount, frames - whole };
}
