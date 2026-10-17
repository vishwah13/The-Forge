# GPU Unit Data and Animation Table Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every on-screen unit cheaper and smoother to draw, and make the animation table shareable between meshes, so the game scales to large tower-defense waves with several enemy types.

**Architecture:** Four changes, in order:
1. Each unit sends 24 bytes to the GPU (position, yaw, scale, team, hit flash, pose offset) instead of 96, and the shaders build the world matrix themselves.
2. The baked table stores skeleton-joint poses; each mesh supplies its own joint remap and inverse bind poses, which are applied in the vertex shader. Any mesh rigged to the same skeleton reuses the table.
3. Each unit blends two neighbouring baked frames (LOD0/LOD1 only).
4. Poses and inverse bind poses are stored as 3x4 affine rows (48 B) instead of 4x4 (64 B).

CPU reference functions in headers are the single source of truth: the self test checks them, and the shaders mirror them line for line.

**Tech Stack:** The Forge 1.63 (C++, FSL shaders compiled for Vulkan and D3D12), ozz via The Forge `Rig`/`Clip`, Flecs sim, MSBuild with VS2019 (v142).

**Spec:** The design section below. It turns the improvement list agreed on 2026-10-15 (memory note `next-session-resume`) into concrete layouts. The game design itself is in `Games/AutoBattler/docs/2026-10-13-auto-battler-design.md`.

## Design

### Baseline (Release, RTX 4070 SUPER, `AutoBattler_stress.txt` at e1578af1)

| units | view  | frame ms | sim ms | gpu ms | gpu Mtris |
|------:|-------|---------:|-------:|-------:|----------:|
| 65536 | field |    11.87 |   3.32 |  11.66 |    100.50 |
| 65536 | zoom  |     5.04 |   2.97 |   2.22 |     15.89 |

What to expect from this plan:
- **Field view:** triangle-bound, because LOD2 is 1,632 triangles. Changes 1–4 shave vertex fetch and skinning work, but they don't change the triangle count, so expect small gains there.
- **Zoom view:** CPU-bound (5.04 ms frame against 2.22 ms GPU). Change 1 targets the CPU side: no mat4 build per unit, and 1.5 MB instead of 6 MB to write and upload at 64k.

### Instance layout (change 1)

The C++ and FSL layouts must match byte for byte. The struct uses scalar members only: in Vulkan std430, a struct holding a `float4` gets 16-byte alignment, which would round the array stride up to 32 while C++ stays at 24.

```
UnitInstance (24 B)
  float x, z     world position on the ground (the sim keeps every unit at y = 0)
  float yaw      radians about +y, same sense as mat4::rotationY
  float scale    UNIT_MODEL_SCALE * type scale
  uint  tintFlash  bits 0-7 team index into FrameUniforms.teamColors, bits 8-15 hit flash as unorm8
  uint  pose       bits 0-23 first pose matrix in the baked table; bits 24-31 blend to the next frame as unorm8 (change 3)
```

The sim writes `pose = 0`. It hands the clip and time to the app in a separate CPU-only array, `RenderData::pUnitAnims` (`UnitAnim { AnimClip mClip; float mTime; }`), and the app fills `pose`. This replaces the current trick of overwriting `mAnim.x`.

World matrix: `translation(x, 0, z) * rotationY(yaw) * scale(scale)`. A model point (px, py, pz) maps to world `(x + s*(c*px + sn*pz), s*py, z + s*(-sn*px + c*pz))`, where c = cos(yaw) and sn = sin(yaw). This is the Sony vectormath convention, and the self test pins it against `mat4`.

### Animation table (changes 2–4)

- **Pose table.** Each frame stores `rigJointCount` model-space joint matrices (`Rig::mNumJoints`), with no inverse bind folded in. Every clip gets one extra wrap frame (a copy of frame 0), so "next frame" is always `pose + rigJointCount` without a modulo.
- **Skin table** (one entry per mesh joint): `SkinJoint { Affine inverseBind; uint rigJoint; uint pad[3]; }`, i.e. 48 + 16 = 64 B. During change 2 it uses `float4x4`, 80 B.
- **Vertex shader.** For each influence k with mesh joint j = `BoneIndices[k]`:
  - `skin = gSkinJoints[j]`
  - `p = pose(base + skin.rigJoint) * (skin.inverseBind * v)`
  - `skinned += w_k * p`

  Transforming the vertex instead of multiplying matrices keeps the cost at two matrix-vector products per influence.
- **Blending (change 3).** `pose(i) = lerp(table[i], table[i + J], blend)` row by row, used for LOD0 and LOD1. LOD2 reads one frame.
- **Affine (change 4).** `struct Affine { float4 rows[3]; }` holds rows 0–2 of the matrix, since the last row is always (0, 0, 0, 1). `mul(a, v) = float3(dot(rows[0], v), dot(rows[1], v), dot(rows[2], v))`.

**The Paladin is a placeholder.** All units will move to models from one character kit later, and kits usually share a single skeleton across all their characters, so change 2 lets the whole kit share one pose table. Nothing in this plan tunes for the Paladin: the far-LOD triangle count and the bounds constants stay as they are until the kit arrives.

**Sharing, honestly stated.** One pose table serves every mesh skinned to that same skeleton (reskins, armour variants, meshes exported against the same Mixamo rig). Two different Mixamo characters usually have different bone lengths, so each still gets its own table (about 1.4 MB after change 4), shared by all instances of that type. Drawing several models (a draw per model per LOD, a model index in the instance) belongs to the tower-defense work and is out of scope here. This plan puts the data layout in place that it needs.

## Global Constraints

- Work on branch `feature/auto-battler-1.63` in `C:\Dev\Engine\The-Forge`. Do not push. The user pushes to GitHub `origin` only when they ask.
- Do not touch `Common_3/`, `Examples_3/`, or the untracked `Art/` folder. All changes are under `Games/AutoBattler/`.
- 1.63 idioms: un-prefixed types, vectormath `mat4`/`vec3`/`vec4`, `v4ToF4`/`f3Tov3` at float boundaries, `tf_malloc`/`tf_free`, `LOGF`.
- Shaders build for `VULKAN DIRECT3D12` through `src/Shaders/FSL/Shaders.list`, as part of the MSBuild build.
- Build (Debug): `"/c/Program Files (x86)/Microsoft Visual Studio/2019/Community/MSBuild/Current/Bin/MSBuild.exe" Games/AutoBattler/PC_VS2019/AutoBattler.sln -p:Configuration=Debug -p:Platform=x64 -m -v:minimal` from the repo root. Release: the same with `-p:Configuration=Release`. If Release fails with LNK1103 on flecs.obj, kill `mspdbsrv` and delete the AutoBattler Intermediate folder.
- Run from the exe folder: `cd Games/AutoBattler/PC_VS2019/x64/<Config>/AutoBattler && ./AutoBattler.exe --selftest` (writes `AutoBattler_selftest.txt`, which must end `RESULT: 0 failure(s)`, exit code 0). `--smoke` writes `AutoBattler_smoke.txt` (expect `result=Victory`). `--stress` writes `AutoBattler_stress.txt`.
- Every stress table must show `worst_gpu_minus_cpu_tris` = 0 in every row except a few thousand on a single zoom row. That one-off was present in the baseline as a one-frame camera change. Any persistent mismatch means the GPU cull and the CPU reference disagree.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **Yaw sign or axis swapped in the shader.** Units would face away from their target, or the mirror image would render. The self test pins `unitWorldMatrix` against `mat4`. Task 1 also compares a smoke screenshot before and after: units must face the enemy.
2. **std430 stride mismatch on Vulkan.** If an FSL struct grows a vector member, the GPU would read garbage instances. Task 1 checks with `static_assert(sizeof(UnitInstance) == 24)` and keeps the FSL members scalar. Task 4 asserts `sizeof(Affine) == 48` and `sizeof(SkinJoint) == 64`.
3. **Pose offset over 24 bits** (a much larger table later). The offset would silently wrap. Task 1's `packUnitPose` asserts `offset < 1u << 24`, and the self test checks the largest offset in the baked table fits.
4. **Blend across the clip end.** The last frame would blend into the next clip. Task 3's wrap frame and its self test (time just before the loop point gives `frame = count - 1`, and `pose + J` is still inside the clip) cover this.
5. **Hit flash or team colour lost in packing.** Flash rounds to unorm8: 0 must stay 0 (the self test's `noFlash` check) and 1 must give 255. The team index must round-trip for both teams. Task 1's tests cover both.

---

### Task 0: Baseline

**Files:** none.

- [ ] **Step 1:** Build Release and run `--selftest`, `--smoke`, `--stress`. Expected: `RESULT: 0 failure(s)`, `result=Victory`, and a stress table close to the baseline table above.
- [ ] **Step 2:** Capture a screenshot of a `--smoke` run about 8 s in, as the visual reference. Save it to the session scratchpad as `baseline_smoke.png`. Capture method (PowerShell):

```powershell
$p = Start-Process .\AutoBattler.exe -ArgumentList '--smoke' -PassThru; Start-Sleep 8
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds; $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
[System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$bmp.Save("$env:SCRATCH\baseline_smoke.png"); Stop-Process $p
```

---

### Task 1: 24-byte unit instances

**Files:**
- Create: `Games/AutoBattler/src/UnitInstance.h` (layout plus pack/unpack/world-matrix reference functions)
- Modify: `Games/AutoBattler/src/AutoBattlerSim.h` (remove the old `UnitInstance`, add `UnitAnim` and `RenderData::pUnitAnims`)
- Modify: `Games/AutoBattler/src/AutoBattlerSim.cpp:951-975` (`simFillRenderData`)
- Modify: `Games/AutoBattler/src/UnitCulling.h` (cull from base position + scale instead of `mat4`)
- Modify: `Games/AutoBattler/src/AnimationBake.h/.cpp` (`bakedPaletteOffset` → `bakedPose`)
- Modify: `Games/AutoBattler/src/AutoBattler.cpp` (allocation, per-frame pose fill, team colours, `cullOnCpu`)
- Modify: `Games/AutoBattler/src/Shaders/FSL/AutoBattler.h.fsl`, `Unit.vert.fsl`, `Cull.comp.fsl`
- Test: `Games/AutoBattler/src/AutoBattlerSelfTest.cpp`

**Interfaces:**
- Produces (in `UnitInstance.h`):
  - `struct UnitInstance { float mX, mZ, mYaw, mScale; uint32_t mTintFlash, mPose; };`
  - `uint32_t packTintFlash(uint32_t team, float flash)`
  - `uint32_t unitTeam(const UnitInstance&)`
  - `float unitFlash(const UnitInstance&)`
  - `uint32_t packUnitPose(uint32_t offset, float blend)`
  - `uint32_t unitPoseOffset(uint32_t pose)`
  - `float unitPoseBlend(uint32_t pose)`
  - `mat4 unitWorldMatrix(const UnitInstance&)`
- Produces (in `AutoBattlerSim.h`): `struct UnitAnim { AnimClip mClip; float mTime; };` and `RenderData::pUnitAnims` (capacity = unit capacity)
- Produces (in `UnitCulling.h`): `bool unitInFrustum(const float4 planes[6], const float3& base, float scale)` and `int32_t unitLod(const float4 planes[6], const float3& cameraPos, const float3& base, float scale)`
- Produces (in `AnimationBake.h`): `struct BakedPose { uint32_t mOffset; float mBlend; };` and `BakedPose bakedPose(const BakedAnimations&, AnimClip, float time)`. Change 1 always returns `mBlend = 0`; Task 3 fills it.

- [ ] **Step 1: Write the failing tests.** Add to `AutoBattlerSelfTest.cpp`, include `"UnitInstance.h"`, and register `{ "unit instance packing", testUnitInstancePacking }` after `"render data in battle"`:

```cpp
static void testUnitInstancePacking(TestContext* pCtx)
{
    CHECK(sizeof(UnitInstance) == 24u);

    // World matrix rebuilt from the packed fields equals the one the sim used to send.
    const float    yaws[] = { 0.0f, 0.7f, -2.5f, PI };
    bool           same = true;
    for (uint32_t i = 0; i < TF_ARRAY_COUNT(yaws); ++i)
    {
        UnitInstance u = {};
        u.mX = 12.5f; u.mZ = -3.0f; u.mYaw = yaws[i]; u.mScale = 1.3f;
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
```

Update the existing tests to the new fields:
- `allocRenderData` also allocates `data.pUnitAnims = (UnitAnim*)tf_malloc(sizeof(UnitAnim) * unitCapacity);`, and `freeRenderData` frees it.
- In `testRenderDataPrep`: replace `v4ToF4(data.pUnits[i].mWorld[3])` with `float4(data.pUnits[i].mX, 0.0f, data.pUnits[i].mZ, 1.0f)`, the blue test `mColor.z > mColor.x` with `unitTeam(data.pUnits[i]) == TEAM_PLAYER`, `mColor.w == 0.0f` with `unitFlash(data.pUnits[i]) == 0.0f`, and `length(mWorld[0].getXYZ())` with `data.pUnits[i].mScale`.
- In `countClip`: use `unitTeam(...) == TEAM_PLAYER` and `data.pUnitAnims[i].mClip == clip`.
- In `testAnimationState`: replace `data.pUnits[k].mAnim.y` with `data.pUnitAnims[k].mTime`.
- In `allInsideWorld` (line ~502): read `mX` and `mZ` instead of `mWorld[3]`.
- In `testUnitCulling` and `testUnitLodSelection`: replace `unitAt(x, y, z, s)` with `float3(x, y, z), s`. Delete `unitAt`. For example, `CHECK(unitInFrustum(planes, float3(0.0f, 0.0f, 0.0f), s));` and `CHECK(unitLod(planes, eye, float3(0.0f, 0.0f, eye.z + 10.0f * r), s) == 0);`.

- [ ] **Step 2: Build Debug.** Expected: compile errors (`UnitInstance.h` missing, `mX` not a member). This is the failing state.

- [ ] **Step 3: Implement.** Create `UnitInstance.h`:

```cpp
#pragma once

#include "../../../Common_3/Utilities/Math/MathTypes.h"

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

static const uint32_t UNIT_POSE_OFFSET_BITS = 24;

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
```

`clampf` lives in `AutoBattlerSim.h`, so include `"AutoBattlerSim.h"` at the top of `UnitInstance.h`. `AutoBattlerSim.h` must then no longer define `UnitInstance`. It forward-declares `struct UnitInstance;` for `RenderData`, so there is no include cycle.

In `AutoBattlerSim.h`, delete the old `UnitInstance` and add:

```cpp
struct UnitInstance; // UnitInstance.h

// Clip and time a unit is playing; the app turns it into UnitInstance::mPose. CPU only.
struct UnitAnim
{
    AnimClip mClip;
    float    mTime; // seconds
};
```

Add `UnitAnim* pUnitAnims; // capacity MAX_UNITS, parallel to pUnits` to `RenderData`. Update the `simFillRenderData` comment to say it also writes one `UnitAnim` per unit.

In `AutoBattlerSim.cpp`, include `"UnitInstance.h"` and replace the instance fill (lines 966–974):

```cpp
            UnitInstance& instance = pOut->pUnits[pOut->mUnitCount];
            instance.mX = pos[i].x;
            instance.mZ = pos[i].z;
            instance.mYaw = unit[i].yaw;
            instance.mScale = scale;
            instance.mTintFlash = packTintFlash(unit[i].team, health[i].hitFlash);
            instance.mPose = 0; // the app fills it from pUnitAnims
            const bool winner = (p->mPhase == PHASE_VICTORY && unit[i].team == TEAM_PLAYER) ||
                                (p->mPhase == PHASE_DEFEAT && unit[i].team == TEAM_ENEMY);
            pOut->pUnitAnims[pOut->mUnitCount].mClip =
                winner ? ANIM_DANCE : (unit[i].moving ? ANIM_WALK : (unit[i].attacking ? ANIM_ATTACK : ANIM_IDLE));
            pOut->pUnitAnims[pOut->mUnitCount].mTime = unit[i].animTime;
            ++pOut->mUnitCount;
```

Delete `TEAM_COLORS` from `AutoBattlerSim.cpp` and move it to `AutoBattler.cpp` (it now feeds `FrameUniforms.teamColors`).

In `UnitCulling.h`, change the two functions to take the ground position and scale:

```cpp
// True unless the unit's bounding sphere lies entirely outside one plane. base: the unit's feet.
inline bool unitInFrustum(const float4 planes[6], const float3& base, float scale)
{
    const vec3  center = f3Tov3(base) + vec3(0.0f, UNIT_BOUND_CENTER_Y * scale, 0.0f);
    const float radius = UNIT_BOUND_RADIUS * scale;
    for (int i = 0; i < 6; ++i)
        if ((float)dot(f3Tov3(planes[i].getXYZ()), center) + planes[i].w < -radius)
            return false;
    return true;
}

// -1 when culled, otherwise the level of detail to draw the unit with.
inline int32_t unitLod(const float4 planes[6], const float3& cameraPos, const float3& base, float scale)
{
    if (!unitInFrustum(planes, base, scale))
        return -1;
    const vec3  center = f3Tov3(base) + vec3(0.0f, UNIT_BOUND_CENTER_Y * scale, 0.0f);
    const float distance = (float)length(center - f3Tov3(cameraPos)) / (UNIT_BOUND_RADIUS * scale);
    return distance >= UNIT_LOD_DISTANCES[1] ? 2 : (distance >= UNIT_LOD_DISTANCES[0] ? 1 : 0);
}
```

Also update the comment that says "The instance matrix scales it to world size" to "The instance scale scales it to world size".

In `AnimationBake.h/.cpp`, replace `bakedPaletteOffset`:

```cpp
// Where a unit's pose starts in the baked table for `clip` at `time` seconds (looping), and how far it
// is towards the next frame.
struct BakedPose
{
    uint32_t mOffset; // first matrix
    float    mBlend;  // [0,1) towards the frame mJointCount matrices later
};
BakedPose bakedPose(const BakedAnimations& baked, AnimClip clip, float time);
```

```cpp
BakedPose bakedPose(const BakedAnimations& baked, AnimClip clip, float time)
{
    const BakedClip& c = baked.mClips[clip];
    const uint32_t   frame = time > 0.0f ? (uint32_t)(time * BAKE_FPS) % c.mFrameCount : 0u;
    return { (c.mFirstFrame + frame) * baked.mJointCount, 0.0f };
}
```

In `AutoBattler.cpp`:
- Include `"UnitInstance.h"`.
- Allocate `gRenderData.pUnitAnims = (UnitAnim*)tf_malloc(sizeof(UnitAnim) * unitCapacity);` next to `pUnits` and free it in `Exit` next to `pUnits`.
- Add `static const float3 TEAM_COLORS[TEAM_COUNT] = { float3(0.30f, 0.55f, 1.00f), float3(1.00f, 0.30f, 0.25f) };` near the asset paths.
- Add `float4 mTeamColors[2]; // TEAM_COUNT, rgb` to `FrameUniforms` after `mCameraPos`, and set it in `updateCullUniforms` with `gFrameUniforms.mTeamColors[t] = float4(TEAM_COLORS[t], 1.0f);`.
- Replace the per-frame loop at lines 556–561:

```cpp
        // The sim says which clip and time each unit plays; turn that into its pose in the baked table.
        for (uint32_t i = 0; i < gRenderData.mUnitCount; ++i)
        {
            const BakedPose pose = bakedPose(gBaked, gRenderData.pUnitAnims[i].mClip, gRenderData.pUnitAnims[i].mTime);
            gRenderData.pUnits[i].mPose = packUnitPose(pose.mOffset, pose.mBlend);
        }
```

- In `cullOnCpu`, replace the `unitLod` call with `unitLod(gFrameUniforms.mCullPlanes, eye, float3(u.mX, 0.0f, u.mZ), u.mScale)`, where `const UnitInstance& u = gRenderData.pUnits[i];`.

In `Shaders/FSL/AutoBattler.h.fsl`:
- Add `DATA(float4, teamColors[2], None); // rgb per Team` after `cameraPos` in `FrameUniforms`. The C++ struct must match in the same position.
- Replace `UnitInstance`:

```
// Scalar members only so Vulkan's std430 stride is 24 bytes, like UnitInstance.h.
STRUCT(UnitInstance)
{
    DATA(float, x, None);
    DATA(float, z, None);
    DATA(float, yaw, None);
    DATA(float, scale, None);
    DATA(uint, tintFlash, None); // bits 0-7 team, 8-15 hit flash unorm8
    DATA(uint, pose, None);      // bits 0-23 first pose matrix, 24-31 blend unorm8
};
#define UNIT_POSE_OFFSET_MASK 0xffffffu // must match UNIT_POSE_OFFSET_BITS in UnitInstance.h
```

In `Unit.vert.fsl`, replace the body from `uint palette` through `Out.Color` with:

```
    UnitInstance inst = gUnitInstances[unit];
    uint     palette = inst.pose & UNIT_POSE_OFFSET_MASK;
    float4x4 boneTransform = gBonePalettes[palette + In.BoneIndices[0]] * In.BoneWeights[0];
    boneTransform += gBonePalettes[palette + In.BoneIndices[1]] * In.BoneWeights[1];
    boneTransform += gBonePalettes[palette + In.BoneIndices[2]] * In.BoneWeights[2];
    boneTransform += gBonePalettes[palette + In.BoneIndices[3]] * In.BoneWeights[3];

    // World = translation(x, 0, z) * rotationY(yaw) * scale, as unitWorldMatrix() in UnitInstance.h.
    float  c = cos(inst.yaw);
    float  s = sin(inst.yaw);
    float3 model = mul(boneTransform, float4(In.Position, 1.0f)).xyz;
    float3 worldPos = float3(inst.x + inst.scale * (c * model.x + s * model.z), inst.scale * model.y,
                             inst.z + inst.scale * (c * model.z - s * model.x));
    Out.Position = mul(gFrame.viewProj, float4(worldPos, 1.0f));
    float3 normal = mul(boneTransform, float4(decodeDir(unpackUnorm2x16(In.Normal)), 0.0f)).xyz;
    Out.Normal = normalize(float3(c * normal.x + s * normal.z, normal.y, c * normal.z - s * normal.x));
    Out.UV = unpack2Floats(In.UV);
    Out.Color = float4(gFrame.teamColors[inst.tintFlash & 0xffu].rgb, float((inst.tintFlash >> 8) & 0xffu) / 255.0f);
```

In `Cull.comp.fsl`, replace the `world`, `center`, and `radius` lines:

```
        UnitInstance inst = gUnitInstances[unit];
        float3       center = float3(inst.x, gFrame.unitBounds.x * inst.scale, inst.z);
        float        radius = gFrame.unitBounds.y * inst.scale;
```

- [ ] **Step 4: Build Debug and run `--selftest`.** Expected: `RESULT: 0 failure(s)`, with the new "unit instance packing" checks passing.
- [ ] **Step 5: Run `--smoke` and take a screenshot** (Task 0 method, saved as `task1_smoke.png`). Expected: `result=Victory`. Compared with `baseline_smoke.png`, units face the enemy, team tints and hit flashes show, and animation plays.
- [ ] **Step 6: Build Release and run `--stress`.** Expected: `worst_gpu_minus_cpu_tris` is 0 (see Global Constraints), and the 64k zoom frame ms is lower than 5.04. Record the 64k rows.
- [ ] **Step 7: Commit.**

```bash
git add Games/AutoBattler/src
git commit -m "Send 24-byte unit instances and build the world matrix in the shaders

<64k field/zoom frame ms before -> after>

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Skeleton pose table plus per-mesh skin table

**Files:**
- Modify: `Games/AutoBattler/src/AnimationBake.h/.cpp` (bake poses per rig joint; build the skin table)
- Modify: `Games/AutoBattler/src/AutoBattler.cpp` (upload two buffers, bind `gSkinJoints`)
- Modify: `Games/AutoBattler/src/Shaders/FSL/SharedSets.h`, `AutoBattler.h.fsl`, `Unit.vert.fsl`
- Test: `Games/AutoBattler/src/AutoBattlerSelfTest.cpp`

**Interfaces:**
- Consumes: `bakedPose` and `UnitInstance::mPose` from Task 1.
- Produces:
  - `struct SkinJoint { mat4 mInverseBind; uint32_t mRigJoint; uint32_t mPad[3]; };` (80 B, 16-aligned)
  - `BakedAnimations` keeps `pPalettes` and `mJointCount`, which now mean "pose matrices" and "rig joints per frame"
  - `bool buildSkinJoints(const GeometryData* pGeom, uint32_t rigJointCount, SkinJoint** ppOut, uint32_t* pCount)`
  - `mat4 skinnedJointMatrix(const mat4* pPoses, const SkinJoint* pSkin, uint32_t poseOffset, uint32_t meshJoint)`, the CPU reference returning `pPoses[poseOffset + skin.mRigJoint] * skin.mInverseBind`

- [ ] **Step 1: Write the failing test.** The self test runs without loading assets, so test the pure functions on synthetic data. Register `{ "skin table", testSkinTable }`:

```cpp
static void testSkinTable(TestContext* pCtx)
{
    // Two rig joints, three mesh joints; mesh joint 2 reuses rig joint 0 with its own bind pose.
    const mat4 poses[4] = { mat4::translation(vec3(1, 0, 0)), mat4::rotationY(0.5f),    // frame 0
                            mat4::translation(vec3(2, 0, 0)), mat4::rotationY(1.0f) };  // frame 1
    SkinJoint skin[3] = {};
    skin[0].mRigJoint = 0; skin[0].mInverseBind = mat4::identity();
    skin[1].mRigJoint = 1; skin[1].mInverseBind = mat4::translation(vec3(0, -1, 0));
    skin[2].mRigJoint = 0; skin[2].mInverseBind = mat4::scale(vec3(2.0f));
    const mat4 a = skinnedJointMatrix(poses, skin, 2u, 1u); // frame 1, mesh joint 1
    const mat4 b = mat4::rotationY(1.0f) * mat4::translation(vec3(0, -1, 0));
    const mat4 c = skinnedJointMatrix(poses, skin, 0u, 2u); // frame 0, mesh joint 2 -> rig joint 0
    const mat4 d = mat4::translation(vec3(1, 0, 0)) * mat4::scale(vec3(2.0f));
    bool same = true;
    for (int k = 0; k < 4; ++k)
        same = same && (float)length(a[k] - b[k]) < 1e-6f && (float)length(c[k] - d[k]) < 1e-6f;
    CHECK(same);
    CHECK(sizeof(SkinJoint) % 16u == 0u);
}
```

`testSkinTable` needs `AnimationBake.h`, which drags in no engine headers beyond forward declarations. If `AnimationBake.h` gets heavier, move `SkinJoint` and `skinnedJointMatrix` into a small `SkinTable.h` that both include.

- [ ] **Step 2: Build Debug.** Expected: compile error (`SkinJoint` undefined).

- [ ] **Step 3: Implement the bake.** In `AnimationBake.h`, update the struct comment and add:

```cpp
// One per mesh joint: which rig joint drives it and the mesh's inverse bind pose for it. Lets every mesh
// skinned to the same skeleton share one pose table. Must match SkinJoint in Shaders/FSL/AutoBattler.h.fsl.
struct SkinJoint
{
    mat4     mInverseBind;
    uint32_t mRigJoint;
    uint32_t mPad[3];
};

// pOut->mJointCount is the rig's joint count: each baked frame holds one model-space matrix per rig joint.
bool bakeAnimations(Rig* pRig, const char* const pClipFiles[ANIM_CLIP_COUNT], BakedAnimations* pOut);

// Skin table for one mesh. Fails if a joint remap points outside the rig. Free with tf_free.
bool buildSkinJoints(const GeometryData* pGeom, uint32_t rigJointCount, SkinJoint** ppOut, uint32_t* pCount);

// CPU reference of the vertex shader's per-influence matrix.
inline mat4 skinnedJointMatrix(const mat4* pPoses, const SkinJoint* pSkin, uint32_t poseOffset, uint32_t meshJoint)
{
    return pPoses[poseOffset + pSkin[meshJoint].mRigJoint] * pSkin[meshJoint].mInverseBind;
}
```

In `AnimationBake.cpp`:
- `bakeAnimations` drops `pGeom`, sets `pOut->mJointCount = pRig->mNumJoints`, and replaces `writePalette` with a copy of the rig's joint model matrices: `memcpy(dst, samplers[c].sample(t), sizeof(mat4) * pOut->mJointCount)`. `Matrix4` is `mat4` in 1.63; if the compiler disagrees, copy element-wise.
- Add:

```cpp
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
```

- [ ] **Step 4: Implement the GPU side.**
  - In `SharedSets.h`, add `DECL_BUFFER(Persistent, Buffer(SkinJoint), gSkinJoints)` after `gBonePalettes`.
  - In `AutoBattler.h.fsl`:

```
STRUCT(SkinJoint) // must match SkinJoint in AnimationBake.h
{
    DATA(float4x4, inverseBind, None);
    DATA(uint, rigJoint, None);
    DATA(uint, pad0, None);
    DATA(uint, pad1, None);
    DATA(uint, pad2, None);
};
```

  - In `AutoBattler.cpp`, call `bakeAnimations(&gRig, clips, &gBaked)`, then `buildSkinJoints(pUnitGeomData, gBaked.mJointCount, &pSkin, &skinCount)`. Upload it as `pSkinJointBuffer` (GPU_ONLY, `DESCRIPTOR_TYPE_BUFFER`, stride `sizeof(SkinJoint)`), exactly like `paletteDesc`. `tf_free` it after `waitForAllResourceLoads()`, remove the buffer in `Exit`, and bind it in the persistent descriptor set next to `gBonePalettes` (grow the `persistent[]` array by one, `SRT_RES_IDX(SrtData, Persistent, gSkinJoints)`).
  - In `Unit.vert.fsl`, replace the four `boneTransform` lines and the `model`/`normal` computation with:

```
    // Each influence: mesh joint -> rig joint pose in the shared table, times this mesh's inverse bind.
    float3 model = float3(0.0f, 0.0f, 0.0f);
    float3 normalModel = float3(0.0f, 0.0f, 0.0f);
    float3 normalIn = decodeDir(unpackUnorm2x16(In.Normal));
    for (uint k = 0; k < 4; ++k)
    {
        SkinJoint skin = gSkinJoints[In.BoneIndices[k]];
        float4x4  pose = gBonePalettes[palette + skin.rigJoint];
        float4    bindPos = mul(skin.inverseBind, float4(In.Position, 1.0f));
        float4    bindNormal = mul(skin.inverseBind, float4(normalIn, 0.0f));
        model += In.BoneWeights[k] * mul(pose, bindPos).xyz;
        normalModel += In.BoneWeights[k] * mul(pose, bindNormal).xyz;
    }
```

  Then use `normalModel` where Task 1 used `normal`.

- [ ] **Step 5: Build Debug, run `--selftest` and `--smoke`, and take a screenshot.** Expected: 0 failures and Victory. The image must match `task1_smoke.png` in pose and shape, because the maths is identical with only the multiplication order regrouped.
- [ ] **Step 6: Build Release and run `--stress`.** Expected: mismatch 0. The 64k field GPU ms may rise slightly (two fetches per influence). Record it, and if it rose more than 10 %, say so in the commit message.
- [ ] **Step 7: Commit.** Message: `Bake skeleton poses once and skin each mesh with its own joint table`, with the 64k numbers and the co-author line.

---

### Task 3: Blend two baked frames

**Files:**
- Modify: `Games/AutoBattler/src/AnimationBake.h/.cpp` (wrap frame per clip, blend in `bakedPose`)
- Modify: `Games/AutoBattler/src/Shaders/FSL/Unit.vert.fsl`
- Test: `Games/AutoBattler/src/AutoBattlerSelfTest.cpp`

**Interfaces:**
- Consumes: `BakedPose`, `packUnitPose`, the pose table layout from Task 2.
- Produces:
  - `BakedClip::mFrameCount` is still the number of distinct frames. The table stores `mFrameCount + 1` frames per clip, and `BakedAnimations::mFrameTotal` counts the stored frames.
  - `bakedPose` returns a real blend.
  - `BAKE_FPS` moves to `AnimationBake.h` so the test can use it.

- [ ] **Step 1: Write the failing test.** `bakedPose` needs only a `BakedAnimations` with clip ranges, so build one by hand. Register `{ "baked pose blend", testBakedPoseBlend }`:

```cpp
static void testBakedPoseBlend(TestContext* pCtx)
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
    BakedPose p = bakedPose(baked, ANIM_WALK, 1.25f * dt); // a quarter past frame 1 of clip 1
    CHECK(p.mOffset == (5u + 1u) * 10u && fabsf(p.mBlend - 0.25f) < 1e-3f);
    p = bakedPose(baked, ANIM_WALK, 3.5f * dt);            // halfway from the last frame into the wrap frame
    CHECK(p.mOffset == (5u + 3u) * 10u && fabsf(p.mBlend - 0.5f) < 1e-3f);
    CHECK(p.mOffset + baked.mJointCount < (5u + 5u) * 10u); // next frame is still this clip's (the wrap frame)
    p = bakedPose(baked, ANIM_WALK, 4.0f * dt);            // looped back to frame 0
    CHECK(p.mOffset == 5u * 10u && p.mBlend < 1e-3f);
    p = bakedPose(baked, ANIM_IDLE, 0.0f);
    CHECK(p.mOffset == 0u && p.mBlend == 0.0f);
    p = bakedPose(baked, ANIM_DANCE, -1.0f);               // negative times clamp to frame 0
    CHECK(p.mOffset == 15u * 10u && p.mBlend == 0.0f);
}
```

- [ ] **Step 2: Build Debug and run `--selftest`.** Expected: FAIL on the blend checks, since `mBlend` is 0 today.

- [ ] **Step 3: Implement.** In `AnimationBake.h`, declare `static const float BAKE_FPS = 30.0f;` and remove it from the `.cpp`. In `bakeAnimations`:
  - Advance `mFrameTotal` by `mFrameCount + 1`.
  - After writing a clip's frames, copy frame 0 into slot `mFrameCount`: `memcpy(&pOut->pPalettes[(clip.mFirstFrame + clip.mFrameCount) * J], &pOut->pPalettes[clip.mFirstFrame * J], sizeof(mat4) * J);`.
  - Update the struct comment to "each clip stores mFrameCount frames plus a copy of its first frame, so blending to the next frame never leaves the clip".

`bakedPose` becomes:

```cpp
BakedPose bakedPose(const BakedAnimations& baked, AnimClip clip, float time)
{
    const BakedClip& c = baked.mClips[clip];
    const float      frames = time > 0.0f ? time * BAKE_FPS : 0.0f;
    const float      whole = floorf(frames);
    const uint32_t   frame = (uint32_t)fmodf(whole, (float)c.mFrameCount);
    return { (c.mFirstFrame + frame) * baked.mJointCount, frames - whole };
}
```

In `Unit.vert.fsl`, after computing `palette`:

```
    // Blend towards the next baked frame (each clip ends with a copy of its first frame); LOD2 is too
    // small on screen to notice and reads one frame.
    float blend = (entry >> VISIBLE_LOD_SHIFT) < 2u ? float(inst.pose >> 24) / 255.0f : 0.0f;
```

and replace `float4x4 pose = gBonePalettes[palette + skin.rigJoint];` with:

```
        float4x4 pose = gBonePalettes[palette + skin.rigJoint];
        if (blend > 0.0f)
            pose = lerp(pose, gBonePalettes[palette + gFrame.rigJointCount + skin.rigJoint], blend);
```

Add `DATA(uint, rigJointCount, None);` to `FrameUniforms` in place of `pad0`, and add `uint32_t mRigJointCount;` to the C++ struct in place of `mPad`. Set it in `updateCullUniforms` from `gBaked.mJointCount`.

- [ ] **Step 4: Build Debug and run `--selftest` and `--smoke`.** Expected: 0 failures and Victory. To check motion visually, run the game normally, zoom into units walking, and compare against the stepping before. The user's eye is the judge here; offer them a Release build to look at.
- [ ] **Step 5: Build Release and run `--stress`.** Expected: mismatch 0. The 64k zoom GPU ms rises slightly (LOD0/1 do double reads); field is unchanged (all LOD2). Record both.
- [ ] **Step 6: Commit.** Message: `Blend two baked animation frames for near units`, with the numbers and the co-author line.

---

### Task 4: 3x4 affine storage

**Files:**
- Modify: `Games/AutoBattler/src/AnimationBake.h/.cpp`, `AutoBattler.cpp` (buffer strides)
- Modify: `Games/AutoBattler/src/Shaders/FSL/SharedSets.h`, `AutoBattler.h.fsl`, `Unit.vert.fsl`
- Test: `Games/AutoBattler/src/AutoBattlerSelfTest.cpp`

**Interfaces:**
- Consumes: everything from Tasks 2–3.
- Produces:
  - `struct Affine { float4 mRows[3]; };` (48 B)
  - `Affine toAffine(const mat4&)`
  - `float3 affineMul(const Affine&, const float4& v)`
  - `BakedAnimations::pPalettes` becomes `Affine*`
  - `SkinJoint { Affine mInverseBind; uint32_t mRigJoint; uint32_t mPad[3]; }` (64 B)
  - `skinnedJointMatrix` is replaced by `float3 skinnedPoint(const Affine* pPoses, const SkinJoint* pSkin, uint32_t poseOffset, uint32_t meshJoint, const float4& v)`

- [ ] **Step 1: Write the failing test.** Rewrite `testSkinTable` for the new types, keeping the same synthetic poses built with `toAffine(mat4::...)`. Compare `skinnedPoint(...)` against `(pose * bind * v).getXYZ()` computed with `mat4` for a point (w = 1) and a direction (w = 0):

```cpp
    CHECK(sizeof(Affine) == 48u && sizeof(SkinJoint) == 64u);
    const vec4  v(0.3f, 1.2f, -0.7f, 1.0f);
    const vec3  expected = (mat4::rotationY(1.0f) * mat4::translation(vec3(0, -1, 0)) * v).getXYZ();
    const float3 actual = skinnedPoint(poses, skin, 2u, 1u, v4ToF4(v));
    CHECK((float)length(f3Tov3(actual) - expected) < 1e-5f);
    const vec4  dir(0.0f, 0.0f, 1.0f, 0.0f);
    const vec3  expectedDir = (mat4::translation(vec3(1, 0, 0)) * mat4::scale(vec3(2.0f)) * dir).getXYZ();
    CHECK((float)length(f3Tov3(skinnedPoint(poses, skin, 0u, 2u, v4ToF4(dir))) - expectedDir) < 1e-5f);
```

Here `poses` is `const Affine poses[4] = { toAffine(mat4::translation(...)), ... }`, and the `skin[i].mInverseBind` values are wrapped in `toAffine`.

- [ ] **Step 2: Build Debug.** Expected: compile error (`Affine` undefined).

- [ ] **Step 3: Implement.** In `AnimationBake.h`:

```cpp
// Rows 0-2 of an affine matrix (row 3 is always 0 0 0 1): 48 bytes instead of 64. Must match Affine in
// Shaders/FSL/AutoBattler.h.fsl.
struct Affine
{
    float4 mRows[3];
};

inline Affine toAffine(const mat4& m) { return { { v4ToF4(m.getRow(0)), v4ToF4(m.getRow(1)), v4ToF4(m.getRow(2)) } }; }

inline float3 affineMul(const Affine& a, const float4& v)
{
    const vec4 x = f4Tov4(v);
    return float3((float)dot(f4Tov4(a.mRows[0]), x), (float)dot(f4Tov4(a.mRows[1]), x), (float)dot(f4Tov4(a.mRows[2]), x));
}

// CPU reference of the vertex shader's per-influence transform: pose(rig joint) * inverseBind * v.
inline float3 skinnedPoint(const Affine* pPoses, const SkinJoint* pSkin, uint32_t poseOffset, uint32_t meshJoint, const float4& v)
{
    const float3 bind = affineMul(pSkin[meshJoint].mInverseBind, v);
    return affineMul(pPoses[poseOffset + pSkin[meshJoint].mRigJoint], float4(bind.x, bind.y, bind.z, v.w));
}
```

Declare `SkinJoint` after `Affine`, with `Affine mInverseBind;`. In `bakeAnimations`:
- Allocate `Affine` instead of `mat4`.
- Write each joint as `toAffine(jointModel[j])`.
- Make the non-finite check loop over `12` floats per entry.
- Use `buildSkinJoints` with `toAffine(pGeom->pInverseBindPoses[i])`.

Add `static_assert(sizeof(Affine) == 48)` and `static_assert(sizeof(SkinJoint) == 64)`.

In `AutoBattler.cpp`, set the palette buffer stride and size to `sizeof(Affine)`.

In `AutoBattler.h.fsl`:

```
STRUCT(Affine) // rows 0-2 of an affine matrix; must match Affine in AnimationBake.h
{
    DATA(float4, row0, None);
    DATA(float4, row1, None);
    DATA(float4, row2, None);
};
STRUCT(SkinJoint) // must match SkinJoint in AnimationBake.h (64 bytes)
{
    DATA(Affine, inverseBind, None);
    DATA(uint, rigJoint, None);
    DATA(uint, pad0, None);
    DATA(uint, pad1, None);
    DATA(uint, pad2, None);
};
```

If FSL rejects a nested struct, flatten `inverseBind` to three `float4` members `bind0..bind2` and adjust the shader. Either way, the layout is 64 B.

In `SharedSets.h`, change `gBonePalettes` to `Buffer(Affine)`. In `Unit.vert.fsl`, add helpers above `VS_MAIN`:

```
float3 affineMul(Affine a, float4 v) { return float3(dot(a.row0, v), dot(a.row1, v), dot(a.row2, v)); }
Affine affineLerp(Affine a, Affine b, float t)
{
    Affine r;
    r.row0 = lerp(a.row0, b.row0, t);
    r.row1 = lerp(a.row1, b.row1, t);
    r.row2 = lerp(a.row2, b.row2, t);
    return r;
}
```

The influence loop becomes:

```
        SkinJoint skin = gSkinJoints[In.BoneIndices[k]];
        Affine    pose = gBonePalettes[palette + skin.rigJoint];
        if (blend > 0.0f)
            pose = affineLerp(pose, gBonePalettes[palette + gFrame.rigJointCount + skin.rigJoint], blend);
        float3 bindPos = affineMul(skin.inverseBind, float4(In.Position, 1.0f));
        float3 bindNormal = affineMul(skin.inverseBind, float4(normalIn, 0.0f));
        model += In.BoneWeights[k] * affineMul(pose, float4(bindPos, 1.0f));
        normalModel += In.BoneWeights[k] * affineMul(pose, float4(bindNormal, 0.0f));
```

- [ ] **Step 4: Build Debug, run `--selftest` and `--smoke`, and take a screenshot.** Expected: 0 failures, Victory, and an image identical to Task 3's.
- [ ] **Step 5: Build Release and run `--stress`.** Expected: mismatch 0. The palette buffer shrinks from 64 to 48 B per entry; log the table size with a `LOGF(eINFO, "AnimationBake: %u frames x %u joints = %u KB", ...)` in `bakeAnimations`. Record the 64k rows.
- [ ] **Step 6: Commit.** Message: `Store baked poses and bind poses as 3x4 affine rows`, with the numbers and the co-author line.

---

### Task 5: Wrap-up

- [ ] **Step 1:** Make a final Release `--selftest`, `--smoke`, `--stress` run, and write a before/after table for the 64k rows (baseline, then after each task) for the user.
- [ ] **Step 2:** Update `Games/AutoBattler/docs/2026-10-13-auto-battler-design.md` if it describes the old 96-byte instance or the per-mesh palette. Search with `grep -n -i "palette\|UnitInstance" Games/AutoBattler/docs/*.md` and correct only those sentences. Commit the result.
- [ ] **Step 3:** Hand over to the user for Release testing. Do not push.
