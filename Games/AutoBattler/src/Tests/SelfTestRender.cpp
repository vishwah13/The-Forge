#include "SelfTest.h"

#include "../App/Render/AnimationBake.h"
#include "../App/Render/UnitCulling.h"
#include "../App/Render/UnitLod.h"
#include "../Game/UnitInstance.h"

#include <math.h>
#include <string.h>

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

void testUnitInstancePacking(TestContext* pCtx)
{
    CHECK(sizeof(UnitInstance) == 24u);

    // World matrix rebuilt from the packed fields equals the one the sim used to send.
    const float yaws[] = { 0.0f, 0.7f, -2.5f, PI };
    bool        same = true;
    for (uint32_t i = 0; i < TF_ARRAY_COUNT(yaws); ++i)
    {
        UnitInstance u = {};
        u.mX = 12.5f;
        u.mZ = -3.0f;
        u.mYaw = yaws[i];
        u.mScale = 1.3f;
        const mat4 expected = mat4::translation(vec3(12.5f, 0.0f, -3.0f)) * mat4::rotationY(yaws[i]) * mat4::scale(vec3(1.3f));
        const mat4 actual = unitWorldMatrix(u);
        for (int c = 0; c < 4; ++c)
            same = same && (float)length(actual[c] - expected[c]) < 1e-5f;
    }
    CHECK(same);

    UnitInstance u = {};
    u.mTintFlash = packTintFlash(TEAM_ENEMY, 1.0f);
    CHECK(unitTeam(u) == TEAM_ENEMY && unitFlash(u) == 1.0f);
    u.mTintFlash = packTintFlash(TEAM_PLAYER, 0.0f);
    CHECK(unitTeam(u) == TEAM_PLAYER && unitFlash(u) == 0.0f);
    u.mTintFlash = packTintFlash(TEAM_PLAYER, 0.5f);
    CHECK(fabsf(unitFlash(u) - 0.5f) < 1.0f / 255.0f);

    const uint32_t pose = packUnitPose((1u << 24) - 1u, 0.25f);
    CHECK(unitPoseOffset(pose) == (1u << 24) - 1u);
    CHECK(fabsf(unitPoseBlend(pose) - 0.25f) < 1.0f / 255.0f);
    CHECK(unitPoseBlend(packUnitPose(7u, 0.0f)) == 0.0f && unitPoseOffset(packUnitPose(7u, 0.0f)) == 7u);
}

void testSkinTable(TestContext* pCtx)
{
    CHECK(sizeof(Affine) == 48u && sizeof(SkinJoint) == 64u);

    // Two rig joints, three mesh joints; mesh joint 2 reuses rig joint 0 with its own bind pose.
    const Affine poses[4] = { toAffine(mat4::translation(vec3(1, 0, 0))), toAffine(mat4::rotationY(0.5f)),   // frame 0
                              toAffine(mat4::translation(vec3(2, 0, 0))), toAffine(mat4::rotationY(1.0f)) }; // frame 1
    SkinJoint    skin[3] = {};
    skin[0].mRigJoint = 0;
    skin[0].mInverseBind = toAffine(mat4::identity());
    skin[1].mRigJoint = 1;
    skin[1].mInverseBind = toAffine(mat4::translation(vec3(0, -1, 0)));
    skin[2].mRigJoint = 0;
    skin[2].mInverseBind = toAffine(mat4::scale(vec3(2.0f)));

    // Frame 1, mesh joint 1, a point.
    const vec4   v(0.3f, 1.2f, -0.7f, 1.0f);
    const vec3   expected = (mat4::rotationY(1.0f) * mat4::translation(vec3(0, -1, 0)) * v).getXYZ();
    const float3 actual = skinnedPoint(poses, skin, 2u, 1u, v4ToF4(v));
    CHECK((float)length(f3Tov3(actual) - expected) < 1e-5f);
    // Frame 0, mesh joint 2 -> rig joint 0, a direction (translation ignored).
    const vec4 dir(0.0f, 0.0f, 1.0f, 0.0f);
    const vec3 expectedDir = (mat4::translation(vec3(1, 0, 0)) * mat4::scale(vec3(2.0f)) * dir).getXYZ();
    CHECK((float)length(f3Tov3(skinnedPoint(poses, skin, 0u, 2u, v4ToF4(dir))) - expectedDir) < 1e-5f);
}

void testBakedPoseBlend(TestContext* pCtx)
{
    BakedAnimations baked = {};
    baked.mJointCount = 10;
    for (uint32_t c = 0; c < ANIM_CLIP_COUNT; ++c)
    {
        baked.mClips[c].mFirstFrame = c * 5u; // 4 distinct frames + 1 wrap frame each
        baked.mClips[c].mFrameCount = 4u;
    }
    baked.mFrameTotal = ANIM_CLIP_COUNT * 5u;

    const float dt = 1.0f / BAKE_FPS;
    BakedPose   p = bakedPose(baked, ANIM_WALK, 1.25f * dt); // a quarter past frame 1 of clip 1
    CHECK(p.mOffset == (5u + 1u) * 10u && fabsf(p.mBlend - 0.25f) < 1e-3f);
    p = bakedPose(baked, ANIM_WALK, 3.5f * dt); // halfway from the last frame into the wrap frame
    CHECK(p.mOffset == (5u + 3u) * 10u && fabsf(p.mBlend - 0.5f) < 1e-3f);
    CHECK(p.mOffset + baked.mJointCount < (5u + 5u) * 10u); // next frame is still this clip's (the wrap frame)
    p = bakedPose(baked, ANIM_WALK, 4.0f * dt);             // looped back to frame 0
    CHECK(p.mOffset == 5u * 10u && p.mBlend < 1e-3f);
    p = bakedPose(baked, ANIM_IDLE, 0.0f);
    CHECK(p.mOffset == 0u && p.mBlend == 0.0f);
    p = bakedPose(baked, ANIM_DANCE, -1.0f); // negative times clamp to frame 0
    CHECK(p.mOffset == 15u * 10u && p.mBlend == 0.0f);

    // Every pose offset must fit UnitInstance::mPose's 24 bits, or units would play another clip's frames.
    CHECK(bakedPosesFitInstance(baked));
    baked.mJointCount = 70u;
    baked.mFrameTotal = (1u << UNIT_POSE_OFFSET_BITS) / 70u; // offsets up to 16,777,149
    CHECK(bakedPosesFitInstance(baked));
    baked.mFrameTotal += 1u;                                 // one frame past 2^24 matrices
    CHECK(!bakedPosesFitInstance(baked));
}

// Feet of a unit whose bounding sphere centre is at (x, y, z) in world space.
static float3 unitAt(float x, float y, float z, float scale) { return float3(x, y - UNIT_BOUND_CENTER_Y * scale, z); }

void testUnitCulling(TestContext* pCtx)
{
    // Camera at z = -50 looking down +z, so a point's depth is z + 50 and the right frustum edge at
    // depth d is x = d * tan(fovX / 2). Same projection as the app.
    const float zNear = 0.5f;
    const float zFar = 2000.0f;
    const float tanHalfX = tanf(PI / 6.0f);
    const float aspectInverse = 9.0f / 16.0f;
    const mat4  view = mat4::lookAtLH(Point3(0.0f, 0.0f, -50.0f), Point3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));
    const mat4  proj = mat4::perspectiveLH_ReverseZ(PI / 3.0f, aspectInverse, zNear, zFar);
    float4      planes[6];
    frustumPlanesFromViewProj(proj * view, planes);

    const float s = UNIT_MODEL_SCALE * gUnitStats[UNIT_MELEE].mScale; // melee instance scale
    const float r = UNIT_BOUND_RADIUS * s;                            // world radius
    const float edgeX = 50.0f * tanHalfX;     // right edge at z = 0
    const float edgeY = edgeX * aspectInverse; // top edge at z = 0
    const float c30 = cosf(PI / 6.0f);        // a sphere at edge + o is o * c30 from the side plane
    CHECK(r > 1.0f * c30 && r < 2.0f * c30);  // so +1 m straddles the edge and +2 m is fully outside

    CHECK(unitInFrustum(planes, unitAt(0.0f, 0.0f, 0.0f, s), s));
    CHECK(unitInFrustum(planes, unitAt(edgeX + 1.0f, 0.0f, 0.0f, s), s));   // straddles the right edge
    CHECK(!unitInFrustum(planes, unitAt(edgeX + 2.0f, 0.0f, 0.0f, s), s));  // right of it
    CHECK(unitInFrustum(planes, unitAt(-edgeX - 1.0f, 0.0f, 0.0f, s), s));  // straddles the left edge
    CHECK(!unitInFrustum(planes, unitAt(-edgeX - 2.0f, 0.0f, 0.0f, s), s)); // left of it
    CHECK(!unitInFrustum(planes, unitAt(0.0f, edgeY + 2.0f / cosf(atanf(tanHalfX * aspectInverse)), 0.0f, s), s)); // above
    CHECK(!unitInFrustum(planes, unitAt(0.0f, -edgeY - 2.0f / cosf(atanf(tanHalfX * aspectInverse)), 0.0f, s), s)); // below
    CHECK(!unitInFrustum(planes, unitAt(0.0f, 0.0f, -60.0f, s), s));                       // behind the camera
    CHECK(unitInFrustum(planes, unitAt(0.0f, 0.0f, -50.0f + zNear - 1.0f, s), s));          // straddles the near plane
    // The reverse-Z matrix stores depth as 1 - f/(f-n) in floats, which places the far plane up to ~1 m
    // off at 2 km (the GPU clips with the same matrix), so these two use wider margins.
    CHECK(unitInFrustum(planes, unitAt(0.0f, 0.0f, -50.0f + zFar + 0.3f, s), s));           // straddles the far plane
    CHECK(!unitInFrustum(planes, unitAt(0.0f, 0.0f, -50.0f + zFar + 3.0f, s), s));          // beyond it
    CHECK(unitInFrustum(planes, unitAt(edgeX + 2.0f, 0.0f, 0.0f, 2.0f * s), 2.0f * s));          // a bigger unit still reaches in
}

void testUnitLodSelection(TestContext* pCtx)
{
    // Same camera as testUnitCulling: at z = -50 looking down +z.
    const float3 eye(0.0f, 0.0f, -50.0f);
    const mat4   view = mat4::lookAtLH(Point3(f3Tov3(eye)), Point3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));
    const mat4   proj = mat4::perspectiveLH_ReverseZ(PI / 3.0f, 9.0f / 16.0f, 0.5f, 2000.0f);
    float4       planes[6];
    frustumPlanesFromViewProj(proj * view, planes);

    const float s = 0.5f;
    const float r = UNIT_BOUND_RADIUS * s;
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z + 10.0f * r, s), s) == 0);
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z + (UNIT_LOD_DISTANCES[0] - 1.0f) * r, s), s) == 0);
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z + (UNIT_LOD_DISTANCES[0] + 1.0f) * r, s), s) == 1);
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z + (UNIT_LOD_DISTANCES[1] - 1.0f) * r, s), s) == 1);
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z + (UNIT_LOD_DISTANCES[1] + 1.0f) * r, s), s) == 2);
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z + 1000.0f, s), s) == 2);
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z - 10.0f, s), s) == -1); // behind the camera
    // Distances are in bounding radii, so a bigger unit keeps detail further out.
    CHECK(unitLod(planes, eye, unitAt(0.0f, 0.0f, eye.z + (UNIT_LOD_DISTANCES[0] + 1.0f) * r, 2.0f * s), 2.0f * s) == 0);
}

// UV sphere with a duplicated seam column, like a textured mesh.
static void makeSphere(uint32_t rings, uint32_t segments, float** ppPositions, uint32_t** ppIndices, uint32_t* pVertexCount,
                       uint32_t* pIndexCount)
{
    *pVertexCount = (rings + 1) * (segments + 1);
    *pIndexCount = rings * segments * 6;
    float*    positions = (float*)tf_malloc(sizeof(float) * 3 * *pVertexCount);
    uint32_t* indices = (uint32_t*)tf_malloc(sizeof(uint32_t) * *pIndexCount);
    for (uint32_t ring = 0; ring <= rings; ++ring)
        for (uint32_t seg = 0; seg <= segments; ++seg)
        {
            const float theta = PI * (float)ring / (float)rings;
            const float phi = 2.0f * PI * (float)seg / (float)segments;
            float*      p = &positions[3 * (ring * (segments + 1) + seg)];
            p[0] = sinf(theta) * cosf(phi);
            p[1] = cosf(theta);
            p[2] = sinf(theta) * sinf(phi);
        }
    uint32_t n = 0;
    for (uint32_t ring = 0; ring < rings; ++ring)
        for (uint32_t seg = 0; seg < segments; ++seg)
        {
            const uint32_t a = ring * (segments + 1) + seg;
            const uint32_t b = a + segments + 1;
            const uint32_t quad[6] = { a, b, a + 1, a + 1, b, b + 1 };
            for (uint32_t k = 0; k < 6; ++k)
                indices[n++] = quad[k];
        }
    *ppPositions = positions;
    *ppIndices = indices;
}

void testUnitLodBuild(TestContext* pCtx)
{
    float*    positions = NULL;
    uint32_t* indices = NULL;
    uint32_t  vertexCount = 0;
    uint32_t  indexCount = 0;
    makeSphere(48, 96, &positions, &indices, &vertexCount, &indexCount);

    UnitLods lods = {};
    CHECK(!buildUnitLods(indices, 0, positions, vertexCount, 3 * sizeof(float), &lods)); // nothing to simplify
    CHECK(buildUnitLods(indices, indexCount, positions, vertexCount, 3 * sizeof(float), &lods));

    // LOD0 is the mesh unchanged; each further level is a clearly smaller, non-empty triangle list.
    CHECK(lods.mFirstIndex[0] == 0 && lods.mIndexCount[0] == indexCount);
    CHECK(lods.pIndices && memcmp(lods.pIndices, indices, sizeof(uint32_t) * indexCount) == 0);
    CHECK(lods.mIndexCount[1] > 0 && lods.mIndexCount[1] * 2 < lods.mIndexCount[0]);
    CHECK(lods.mIndexCount[2] > 0 && lods.mIndexCount[2] * 2 < lods.mIndexCount[1]);

    // Levels are packed back to back, whole triangles, and only reference the shared vertices.
    bool     packed = true;
    uint32_t total = 0;
    for (uint32_t lod = 0; lod < UNIT_LOD_COUNT; ++lod)
    {
        packed = packed && lods.mFirstIndex[lod] == total && lods.mIndexCount[lod] % 3 == 0;
        total += lods.mIndexCount[lod];
    }
    CHECK(packed && lods.mTotalIndexCount == total);
    bool inRange = true;
    for (uint32_t i = 0; i < lods.mTotalIndexCount; ++i)
        inRange = inRange && lods.pIndices[i] < vertexCount;
    CHECK(inRange);

    freeUnitLods(&lods);
    CHECK(lods.pIndices == NULL);
    tf_free(positions);
    tf_free(indices);
}
