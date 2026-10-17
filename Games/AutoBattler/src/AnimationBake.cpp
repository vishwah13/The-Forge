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

static const float BAKE_FPS = 30.0f;

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
        pOut->mFrameTotal += pOut->mClips[c].mFrameCount;
    }
    pOut->pPalettes = (mat4*)tf_memalign(16, sizeof(mat4) * pOut->mFrameTotal * pOut->mJointCount);

    for (uint32_t c = 0; c < ANIM_CLIP_COUNT; ++c)
    {
        const BakedClip& clip = pOut->mClips[c];
        for (uint32_t f = 0; f < clip.mFrameCount; ++f)
        {
            const Matrix4* pJointModel = samplers[c].sample((float)f / BAKE_FPS);
            mat4*          pDst = &pOut->pPalettes[(clip.mFirstFrame + f) * pOut->mJointCount];
            for (uint32_t j = 0; j < pOut->mJointCount; ++j)
                pDst[j] = pJointModel[j];
        }
        LOGF(eINFO, "AnimationBake: %s baked %u frames", pClipFiles[c], clip.mFrameCount);
    }

    bool         ok = true;
    const float* pFloats = (const float*)pOut->pPalettes;
    for (uint64_t i = 0; i < (uint64_t)pOut->mFrameTotal * pOut->mJointCount * 16u && ok; ++i)
        ok = isfinite(pFloats[i]) != 0;
    if (!ok)
        LOGF(eERROR, "AnimationBake: non-finite palette");

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
        pSkin[i].mInverseBind = pGeom->pInverseBindPoses[i];
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
    const uint32_t   frame = time > 0.0f ? (uint32_t)(time * BAKE_FPS) % c.mFrameCount : 0u;
    return { (c.mFirstFrame + frame) * baked.mJointCount, 0.0f };
}
