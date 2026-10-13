#include "AnimationBake.h"

#include <math.h>
#include <string.h>

#include "../../../../Common_3/Resources/AnimationSystem/Animation/AnimatedObject.h"
#include "../../../../Common_3/Resources/AnimationSystem/Animation/Animation.h"
#include "../../../../Common_3/Resources/AnimationSystem/Animation/Clip.h"
#include "../../../../Common_3/Resources/AnimationSystem/Animation/ClipController.h"
#include "../../../../Common_3/Resources/AnimationSystem/Animation/Rig.h"
#include "../../../../Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"
#include "../../../../Common_3/Utilities/Interfaces/ILog.h"

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

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

    const Matrix4* bindPose()
    {
        mObject.ComputeBindPose(mObject.mRootTransform);
        return &mObject.mJointWorldMats[0];
    }
};

static void writePalette(const GeometryData* pGeom, const Matrix4* pJointModel, mat4* pOut)
{
    for (uint32_t i = 0; i < pGeom->mJointCount; ++i)
        pOut[i] = pJointModel[pGeom->pJointRemaps[i]] * pGeom->pInverseBindPoses[i];
}

// Maps a Stormtrooper (Mixamo) joint to the stickFigure (3ds Max Biped) joint driving it, or -1.
// The rigs share most names once "mixamorig:" is stripped; the exceptions are aliased here.
static int32_t findSourceJoint(Rig* pSourceRig, const char* pTargetName)
{
    static const char* ALIASES[][2] = {
        { "Spine2", "Spine3" }, // Mixamo's chest joint; Biped has one more spine segment
        { "LeftToeBase", "LeftFootThumb1" },
        { "RightToeBase", "RightFootThumb1" },
        { "LeftHandPinky1", "LeftHandRingPinky1" },
        { "LeftHandPinky2", "LeftHandRingPinky2" },
        { "LeftHandPinky3", "LeftHandRingPinky3" },
    };
    const char* pName = strncmp(pTargetName, "mixamorig:", 10) == 0 ? pTargetName + 10 : pTargetName;
    for (uint32_t a = 0; a < sizeof(ALIASES) / sizeof(ALIASES[0]); ++a)
        if (strcmp(pName, ALIASES[a][0]) == 0)
            pName = ALIASES[a][1];

    const auto names = pSourceRig->mSkeleton.joint_names();
    for (uint32_t k = 0; k < (uint32_t)names.size(); ++k)
        if (_stricmp(names[k], pName) == 0)
            return (int32_t)k;
    return -1;
}

// Rest-pose data needed to transfer a source pose onto the target skeleton.
struct Retargeter
{
    int32_t        mTargetCount;
    const int16_t* pParents;
    Matrix4*       pTargetBind;  // target joint model matrices in bind pose
    Matrix4*       pTargetLocal; // target bind pose relative to the parent
    Matrix4*       pModel;       // scratch: retargeted target pose
    int32_t*       pSourceOf;    // source joint per target joint, -1 when unmapped
    Matrix3*       pSourceBindInv;
    Matrix3        mYawFix; // rotates the source rig's facing onto the target's
    int32_t        mTargetHips;
    int32_t        mSourceHips;
    float          mHipScale;

    // Mapped joints take the source joint's model-space rotation change from its rest pose, applied on
    // top of the target's rest pose. Positions come from the target's own bone lengths, so proportions
    // stay the Stormtrooper's. The hips keep only the source's vertical bob: the sim moves units.
    const Matrix4* apply(const Matrix4* pSource, const Matrix4* pSourceBind)
    {
        for (int32_t j = 0; j < mTargetCount; ++j)
        {
            const int16_t parent = pParents[j];
            if (parent < 0)
            {
                pModel[j] = pTargetBind[j];
                continue;
            }
            const Matrix4& parentModel = pModel[parent];
            Vector3        pos = (parentModel * pTargetLocal[j].getCol3()).getXYZ();
            Matrix3        rot;
            const int32_t  k = pSourceOf[j];
            if (k >= 0)
            {
                const Matrix3 delta = mYawFix * pSource[k].getUpper3x3() * pSourceBindInv[j] * transpose(mYawFix);
                rot = delta * pTargetBind[j].getUpper3x3();
            }
            else
            {
                rot = parentModel.getUpper3x3() * pTargetLocal[j].getUpper3x3();
            }
            if (j == mTargetHips)
            {
                const float bob = ((float)pSource[mSourceHips].getCol3().getY() - (float)pSourceBind[mSourceHips].getCol3().getY()) * mHipScale;
                pos = pTargetBind[j].getCol3().getXYZ() + Vector3(0.0f, bob, 0.0f);
            }
            pModel[j] = Matrix4(rot, pos);
        }
        return pModel;
    }
};

static float yawOf(const Vector3& v) { return atan2f((float)v.getX(), (float)v.getZ()); }

static Vector3 jointPos(const Matrix4* pModel, int32_t j) { return pModel[j].getCol3().getXYZ(); }

bool bakeAnimations(Rig* pTargetRig, const GeometryData* pGeom, BakedAnimations* pOut)
{
    memset(pOut, 0, sizeof(*pOut));
    pOut->mJointCount = pGeom->mJointCount;

    Rig sourceRig;
    sourceRig.Initialize(RD_ANIMATIONS, "stickFigure/skeleton.ozz");

    ClipSampler samplers[ANIM_CLIP_COUNT];
    samplers[ANIM_IDLE].init(&sourceRig, "stickFigure/animations/stand.ozz");
    samplers[ANIM_WALK].init(&sourceRig, "stickFigure/animations/walk.ozz");
    samplers[ANIM_DANCE].init(pTargetRig, "stormtrooper/animations/dance.ozz");

    for (uint32_t c = 0; c < ANIM_CLIP_COUNT; ++c)
    {
        pOut->mClips[c].mFirstFrame = pOut->mFrameTotal;
        pOut->mClips[c].mFrameCount = samplers[c].frameCount();
        pOut->mFrameTotal += pOut->mClips[c].mFrameCount;
    }
    pOut->pPalettes = (mat4*)tf_memalign(16, sizeof(mat4) * pOut->mFrameTotal * pOut->mJointCount);

    // Rest poses of both rigs.
    const int32_t targetCount = pTargetRig->mSkeleton.num_joints();
    const int32_t sourceCount = sourceRig.mSkeleton.num_joints();
    Matrix4*      targetBind = (Matrix4*)tf_memalign(16, sizeof(Matrix4) * (size_t)targetCount);
    Matrix4*      sourceBind = (Matrix4*)tf_memalign(16, sizeof(Matrix4) * (size_t)sourceCount);
    memcpy((void*)targetBind, samplers[ANIM_DANCE].bindPose(), sizeof(Matrix4) * (size_t)targetCount);
    memcpy((void*)sourceBind, samplers[ANIM_IDLE].bindPose(), sizeof(Matrix4) * (size_t)sourceCount);

    Retargeter rt = {};
    rt.mTargetCount = targetCount;
    rt.pParents = &pTargetRig->mSkeleton.joint_parents()[0];
    rt.pTargetBind = targetBind;
    rt.pTargetLocal = (Matrix4*)tf_memalign(16, sizeof(Matrix4) * (size_t)targetCount);
    rt.pModel = (Matrix4*)tf_memalign(16, sizeof(Matrix4) * (size_t)targetCount);
    rt.pSourceOf = (int32_t*)tf_malloc(sizeof(int32_t) * (size_t)targetCount);
    rt.pSourceBindInv = (Matrix3*)tf_memalign(16, sizeof(Matrix3) * (size_t)targetCount);

    const auto targetNames = pTargetRig->mSkeleton.joint_names();
    int32_t    mapped = 0;
    for (int32_t j = 0; j < targetCount; ++j)
    {
        const int16_t parent = rt.pParents[j];
        rt.pTargetLocal[j] = parent < 0 ? targetBind[j] : inverse(targetBind[parent]) * targetBind[j];
        rt.pSourceOf[j] = findSourceJoint(&sourceRig, targetNames[j]);
        rt.pSourceBindInv[j] = rt.pSourceOf[j] >= 0 ? inverse(sourceBind[rt.pSourceOf[j]].getUpper3x3()) : Matrix3::identity();
        mapped += rt.pSourceOf[j] >= 0 ? 1 : 0;
    }

    rt.mTargetHips = pTargetRig->FindJoint("mixamorig:Hips");
    rt.mSourceHips = sourceRig.FindJoint("Hips");
    const int32_t tLeft = pTargetRig->FindJoint("mixamorig:LeftUpLeg");
    const int32_t tRight = pTargetRig->FindJoint("mixamorig:RightUpLeg");
    const int32_t sLeft = sourceRig.FindJoint("LeftUpLeg");
    const int32_t sRight = sourceRig.FindJoint("RightUpLeg");
    bool          ok = rt.mTargetHips >= 0 && rt.mSourceHips >= 0 && tLeft >= 0 && tRight >= 0 && sLeft >= 0 && sRight >= 0;
    if (ok)
    {
        const float yaw = yawOf(jointPos(targetBind, tLeft) - jointPos(targetBind, tRight)) - yawOf(jointPos(sourceBind, sLeft) - jointPos(sourceBind, sRight));
        rt.mYawFix = Matrix3::rotationY(yaw);
        const float sourceHipY = (float)jointPos(sourceBind, rt.mSourceHips).getY();
        rt.mHipScale = sourceHipY > 1e-4f ? (float)jointPos(targetBind, rt.mTargetHips).getY() / sourceHipY : 1.0f;
        LOGF(eINFO, "AnimationBake: %d/%d joints mapped, yaw fix %.1f deg, hip scale %.3f, source hips y %.3f", mapped, targetCount,
             yaw * 180.0f / PI, rt.mHipScale, sourceHipY);

        for (uint32_t c = 0; c < ANIM_CLIP_COUNT; ++c)
        {
            const BakedClip& clip = pOut->mClips[c];
            for (uint32_t f = 0; f < clip.mFrameCount; ++f)
            {
                const Matrix4* pPose = samplers[c].sample((float)f / BAKE_FPS);
                if (c != ANIM_DANCE)
                    pPose = rt.apply(pPose, sourceBind);
                writePalette(pGeom, pPose, &pOut->pPalettes[(clip.mFirstFrame + f) * pOut->mJointCount]);
            }
            LOGF(eINFO, "AnimationBake: clip %u baked %u frames", c, clip.mFrameCount);
        }

        const float* pFloats = (const float*)pOut->pPalettes;
        for (uint64_t i = 0; i < (uint64_t)pOut->mFrameTotal * pOut->mJointCount * 16u && ok; ++i)
            ok = isfinite(pFloats[i]) != 0;
    }
    if (!ok)
        LOGF(eERROR, "AnimationBake: failed (missing joints or non-finite palette)");

    tf_free(rt.pSourceBindInv);
    tf_free(rt.pSourceOf);
    tf_free(rt.pModel);
    tf_free(rt.pTargetLocal);
    tf_free(sourceBind);
    tf_free(targetBind);
    for (uint32_t c = 0; c < ANIM_CLIP_COUNT; ++c)
        samplers[c].exit();
    sourceRig.Exit();
    return ok;
}

void freeBakedAnimations(BakedAnimations* pBaked)
{
    tf_free(pBaked->pPalettes);
    pBaked->pPalettes = NULL;
}

uint32_t bakedPaletteOffset(const BakedAnimations& baked, AnimClip clip, float time)
{
    const BakedClip& c = baked.mClips[clip];
    const uint32_t   frame = time > 0.0f ? (uint32_t)(time * BAKE_FPS) % c.mFrameCount : 0u;
    return (c.mFirstFrame + frame) * baked.mJointCount;
}
