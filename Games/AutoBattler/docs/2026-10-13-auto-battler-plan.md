# 40_AutoBattler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A playable top-down auto battler sample (`40_AutoBattler`) on The Forge, with up to ~2,000+ team-tinted Stormtroopers simulated by the framework's Flecs ECS.

**Architecture:** `AutoBattlerSim.h/.cpp` owns a Flecs world, the board, the round rules and the battle systems. It has no graphics includes, so `AutoBattlerSelfTest.cpp` drives it headlessly via `40_AutoBattler.exe --selftest`. `40_AutoBattler.cpp` is the `IApp`: renderer setup, one instanced skinned draw for all units, a ground pass, a billboard pass (health bars + projectiles), camera, picking, UI. The sim produces plain CPU instance arrays (`RenderData`) that the app copies into per-frame GPU buffers.

**Tech Stack:** C++ (MSVC v142, `/W4 /WX`), The Forge renderer + FSL shaders, Flecs 4.0 C API (`Common_3/Game/ThirdParty/OpenSource/flecs`), ozz animation via The Forge's `AnimationSystem`.

**Spec:** `docs/superpowers/specs/2026-10-13-auto-battler-design.md`

## Global Constraints

- Platform: Windows only; project lives in `Examples_3/Unit_Tests/PC_VS2019/` and is built with VS2019 MSBuild (`x64`, `Debug` and `Release`).
- Warnings are errors at `/W4`: no unused parameters (use unnamed params or `UNREF_PARAM`), no unused static functions, no unused locals, explicit casts for every int↔float and signed↔unsigned conversion, unsigned literals (`25u`) when comparing with `uint32_t`.
- No `fopen`/`sprintf` (C4996 → error). Write files with `fsOpenStreamFromPath(RD_LOG, …, FM_WRITE, &fs)` + `fsWriteToStream` + `fsCloseStream`. `snprintf` is fine.
- `#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"` is the **last** include in every `.cpp`. Allocate with `tf_malloc`/`tf_calloc`/`tf_free`; no `new`/`delete`, no STL containers.
- Flecs C API only (the repo builds Flecs with `FLECS_NO_CPP`). Create systems with `ecs_system_init` (the `ECS_SYSTEM` macro cannot set `multi_threaded`). Query terms are set by id (`terms[i].id = ecs_id(T)`), never by DSL string.
- Only one `BattleSim` may exist at a time (Flecs component ids are file-scope globals re-registered per world).
- `AutoBattlerSim.*` must not include any graphics, UI, or input header.
- Assets come only from `Art/`: `stormtrooper/riggedMesh.bin`, `stormtrooper/skeleton.ozz`, `stormtrooper/animations/dance.ozz`, `Stormtrooper_D.tex`, `PBR/copper_tiles_01/1K/Albedo.tex`, `TitilliumText/TitilliumText-Bold.otf`.
- Gameplay constants, verbatim from the spec: arena 16×16 cells of 5 m; player rows 0–7; Melee squad 25 / cost 2 / 100 HP / 15 dmg per 0.8 s / range 1.2 m / 3.0 m/s / scale 1.0; Ranged 16 / 3 / 60 / 12 per 1.0 s / 8 m / 2.5 m/s / 0.9, projectile 15 m/s; Tank 9 / 4 / 250 / 8 per 1.2 s / 1.4 m / 1.8 m/s / 1.3; unit radius 0.4 m × scale; start gold 10; victory gold += 5 + new round; wave budget 6 + 4 × round; result phase 4 s; battle limit 90 s; caps 4,096 units and 1,024 projectiles; grid buckets 2 m; retarget every 0.25 s; dt clamped to 1/30 s.

## Review Focus

1. **Two units at exactly the same position** (e.g. dense squads colliding, test spawns): separation must not divide by zero; expect them to push apart along a deterministic direction, never NaN. Pinned by `testCoincidentUnitsSeparate` (Task 2).
2. **Clicking where placement isn't allowed** (enemy half, outside the arena, during battle, on an occupied cell, without gold): nothing is bought and a message explains why. Pinned by `testBuySell`, `testCellMath`, `testStartBattleGuards` (Tasks 1–2). Clicks over the UI window are filtered by `uiIsFocused()` and verified manually in Task 5.
3. **Frame hitch or bad delta time** (debugger pause, NaN, negative): the sim advances at most 1/30 s and ignores non-positive/NaN steps. Pinned by `testDeltaTimeClamp` (Task 2).
4. **Armies near the unit cap**: buying past 4,096 is refused and render arrays never overflow. Pinned by `testUnitCap` (Task 1) and the render-count checks in `testRenderDataBattle` (Task 3).
5. **Restart or resize at awkward moments** (Restart mid-battle while projectiles fly; resizing the window mid-battle): restart returns cleanly to round 1 prep with no projectiles; resize rebuilds swapchain/pipelines without touching sim state. Restart pinned by `testRestartMidBattle` (Task 2); resize verified manually in Task 5.

---

## Shared commands (used by every task)

Run from the repo root (`C:\Dev\Engine\The-Forge`) in Git Bash.

**Build** (`CONFIG` is `Debug` or `Release`; first build compiles the framework libs and takes several minutes — use a 600000 ms timeout):

```bash
MSBUILD=$(ls /c/Program\ Files\ \(x86\)/Microsoft\ Visual\ Studio/2019/*/MSBuild/Current/Bin/MSBuild.exe | head -1)
"$MSBUILD" Examples_3/Unit_Tests/PC_VS2019/Unit_Tests.sln -t:40_AutoBattler -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
```

**Self test:**

```bash
OUT=Examples_3/Unit_Tests/PC_VS2019/x64/Debug/40_AutoBattler
(cd "$OUT" && timeout 300 ./40_AutoBattler.exe --selftest; echo "exit=$?")
find "$OUT" -name '40_AutoBattler_selftest.txt' -exec cat {} \;
```

Expected on success: `exit=0` and the file ends with `RESULT: 0 failure(s)`. Any `FAIL` line names the failing check and its line in `AutoBattlerSelfTest.cpp`.

---

### Task 1: Project scaffold, board rules and self-test harness

**Files:**
- Create: `Examples_3/Unit_Tests/src/40_AutoBattler/AutoBattlerSim.h`
- Create: `Examples_3/Unit_Tests/src/40_AutoBattler/AutoBattlerSim.cpp`
- Create: `Examples_3/Unit_Tests/src/40_AutoBattler/AutoBattlerSelfTest.cpp`
- Create: `Examples_3/Unit_Tests/src/40_AutoBattler/40_AutoBattler.cpp` (minimal; replaced in Task 4)
- Create: `Examples_3/Unit_Tests/src/40_AutoBattler/Shaders/FSL/Shaders.list`
- Create: `Examples_3/Unit_Tests/src/40_AutoBattler/GPUCfg/gpu.cfg` (copy of 28_Skinning's)
- Create: `Examples_3/Unit_Tests/PC_VS2019/40_AutoBattler.vcxproj` (generated from 28_Skinning's)
- Modify: `Examples_3/Unit_Tests/PC_VS2019/Unit_Tests.sln`

**Interfaces:**
- Produces (used by every later task): everything declared in `AutoBattlerSim.h` below; `int runAutoBattlerSelfTest(const char* pResultFileName)`; component structs `Position`, `Unit`, `Health`, `Attack`, `GridSlot`, `Projectile` and static helpers `spawnUnit`, `rebuildFromBoard`, `cellIndex`, `squadSize`, `boardUnitCount` inside `AutoBattlerSim.cpp`; `struct BattleSim` fields listed below.

- [ ] **Step 1: Copy GPU config and create the shader list**

```bash
mkdir -p Examples_3/Unit_Tests/src/40_AutoBattler/GPUCfg Examples_3/Unit_Tests/src/40_AutoBattler/Shaders/FSL
cp Examples_3/Unit_Tests/src/28_Skinning/GPUCfg/gpu.cfg Examples_3/Unit_Tests/src/40_AutoBattler/GPUCfg/gpu.cfg
```

`Examples_3/Unit_Tests/src/40_AutoBattler/Shaders/FSL/Shaders.list`:

```
#include "../../../../../../Common_3/Graphics/FSL/defaults.h"

#rootsig default.rootsig
#end

#rootsig compute.rootsig
#end
```

- [ ] **Step 2: Generate the project and add it to the solution**

Save as `<scratchpad>/add_project.py` and run `python <scratchpad>/add_project.py` from the repo root:

```python
NEW_GUID = "5B2E8C41-7A3D-4F19-9E62-C4D8A1B3F705"
OLD_GUID = "ADBCFAE4-6B8F-4B7B-B5FF-B256D687B365"
DIR = "Examples_3/Unit_Tests/PC_VS2019/"

ITEMS = [
    '  <ItemGroup>',
    '    <ClCompile Include="..\\src\\40_AutoBattler\\40_AutoBattler.cpp" />',
    '    <ClCompile Include="..\\src\\40_AutoBattler\\AutoBattlerSim.cpp" />',
    '    <ClCompile Include="..\\src\\40_AutoBattler\\AutoBattlerSelfTest.cpp" />',
    '    <ClCompile Include="..\\..\\..\\Common_3\\Game\\ThirdParty\\OpenSource\\flecs\\flecs.c" />',
    '  </ItemGroup>',
    '  <ItemGroup>',
    '    <ClInclude Include="..\\src\\40_AutoBattler\\AutoBattlerSim.h" />',
    '    <ClInclude Include="..\\..\\..\\Common_3\\Game\\ThirdParty\\OpenSource\\flecs\\flecs.h" />',
    '  </ItemGroup>',
    '  <ItemGroup>',
    '    <None Include="..\\src\\40_AutoBattler\\GPUCfg\\gpu.cfg" />',
    '  </ItemGroup>',
    '  <ItemGroup>',
    '    <FSLShader Include="..\\src\\40_AutoBattler\\Shaders\\FSL\\Shaders.list" />',
    '  </ItemGroup>',
]

# vcxproj: copy 28_Skinning's, swap GUID/namespace, replace the file item groups.
src = open(DIR + "28_Skinning.vcxproj", encoding="utf-8-sig", newline="").read()
nl = "\r\n" if "\r\n" in src else "\n"
src = src.replace(OLD_GUID, NEW_GUID).replace("My01Playback", "AutoBattler")
last_idg = src.rindex("</ItemDefinitionGroup>")
start = src.rindex(nl, 0, src.index("<ItemGroup>", last_idg)) + len(nl)
end = src.rindex(nl, 0, src.index("Microsoft.Cpp.targets")) + len(nl)
src = src[:start] + nl.join(ITEMS) + nl + src[end:]
open(DIR + "40_AutoBattler.vcxproj", "w", encoding="utf-8", newline="").write(src)

# sln: duplicate every 28_Skinning entry (project block, configs, nesting) for the new project.
sln_path = DIR + "Unit_Tests.sln"
text = open(sln_path, encoding="utf-8-sig", newline="").read()
assert NEW_GUID not in text
lines = text.split("\r\n")
out = []
i = 0
while i < len(lines):
    line = lines[i]
    if line.startswith("Project(") and '"28_Skinning"' in line:
        block = []
        while True:
            block.append(lines[i])
            if lines[i] == "EndProject":
                break
            i += 1
        out.extend(block)
        out.extend(l.replace("28_Skinning", "40_AutoBattler").replace(OLD_GUID, NEW_GUID) for l in block)
    elif OLD_GUID in line:
        out.append(line)
        out.append(line.replace(OLD_GUID, NEW_GUID))
    else:
        out.append(line)
    i += 1
open(sln_path, "w", encoding="utf-8-sig", newline="").write("\r\n".join(out))
print("ok")
```

Check: `grep -c 5B2E8C41 Examples_3/Unit_Tests/PC_VS2019/Unit_Tests.sln` prints `6` (project line, 4 config lines, 1 nesting line). `git diff --stat` shows only additions to the `.sln`.

- [ ] **Step 3: Write the sim header**

`Examples_3/Unit_Tests/src/40_AutoBattler/AutoBattlerSim.h`:

```cpp
#pragma once

#include "../../../../Common_3/Utilities/Math/MathTypes.h"

// Gameplay simulation for 40_AutoBattler, built on Flecs. Has no graphics dependencies so the self
// test can drive it without a renderer. Only one BattleSim may exist at a time: Flecs component ids
// are file-scope globals that are re-registered for each world.

enum UnitType : int8_t
{
    UNIT_NONE = -1,
    UNIT_MELEE = 0,
    UNIT_RANGED,
    UNIT_TANK,
    UNIT_TYPE_COUNT
};

enum Team : uint8_t
{
    TEAM_PLAYER = 0,
    TEAM_ENEMY,
    TEAM_COUNT
};

enum Phase : uint8_t
{
    PHASE_PREP = 0,
    PHASE_BATTLE,
    PHASE_VICTORY,
    PHASE_DEFEAT
};

static const int32_t  ARENA_CELLS = 16;
static const int32_t  PLAYER_ROWS = ARENA_CELLS / 2; // rows [0, PLAYER_ROWS) belong to the player
static const float    CELL_SIZE = 5.0f;
static const float    ARENA_SIZE = ARENA_CELLS * CELL_SIZE;
static const uint32_t MAX_UNITS = 4096;
static const uint32_t MAX_PROJECTILES = 1024;
static const float    BATTLE_TIME_LIMIT = 90.0f;
static const float    RESULT_DURATION = 4.0f;
static const float    MAX_STEP = 1.0f / 30.0f;
static const int32_t  START_GOLD = 10;
static const float    UNIT_RADIUS = 0.4f;
static const float    PROJECTILE_SPEED = 15.0f;

struct UnitStats
{
    const char* pName;
    int32_t     mSquadSide; // a squad is mSquadSide x mSquadSide units
    int32_t     mCost;
    float       mHp;
    float       mDamage;
    float       mCooldown;
    float       mRange;
    float       mSpeed;
    float       mScale;
};
extern const UnitStats gUnitStats[UNIT_TYPE_COUNT];

struct BattleSim;

BattleSim* simCreate(uint32_t threadCount);
void       simDestroy(BattleSim* pSim);

// Round 1, START_GOLD, empty player board, fresh enemy wave, PHASE_PREP.
void simRestart(BattleSim* pSim);

// Board editing, PREP only. On refusal the state is unchanged and simGetMessage() says why.
bool     simCanBuySquad(const BattleSim* pSim, int32_t cellX, int32_t cellZ, UnitType type);
bool     simBuySquad(BattleSim* pSim, int32_t cellX, int32_t cellZ, UnitType type);
bool     simSellSquad(BattleSim* pSim, int32_t cellX, int32_t cellZ);
UnitType simGetCell(const BattleSim* pSim, int32_t cellX, int32_t cellZ);

Phase       simGetPhase(const BattleSim* pSim);
int32_t     simGetRound(const BattleSim* pSim);
int32_t     simGetGold(const BattleSim* pSim);
uint32_t    simGetUnitCount(BattleSim* pSim, Team team);
const char* simGetMessage(const BattleSim* pSim);

// Pure helpers, also used by the app's mouse picking.
bool   worldToCell(float x, float z, int32_t* pCellX, int32_t* pCellZ);
float2 cellCenter(int32_t cellX, int32_t cellZ);
bool   rayToCell(const float3& origin, const float3& dir, int32_t* pCellX, int32_t* pCellZ);

// Test hooks: edit either half of the board directly, ignoring gold and phase (never the unit cap).
bool simTestSetCell(BattleSim* pSim, int32_t cellX, int32_t cellZ, UnitType type);
void simTestClearBoard(BattleSim* pSim);
void simTestSetGold(BattleSim* pSim, int32_t gold);

// Runs the headless simulation tests, writes a report next to the log, returns the failure count.
int runAutoBattlerSelfTest(const char* pResultFileName);
```

- [ ] **Step 4: Write the failing tests**

`Examples_3/Unit_Tests/src/40_AutoBattler/AutoBattlerSelfTest.cpp`:

```cpp
#include "AutoBattlerSim.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../../../Common_3/Utilities/Interfaces/IFileSystem.h"

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

struct TestContext
{
    FileStream mFile;
    int32_t    mFailures;
};

static void writeLine(TestContext* pCtx, const char* pText)
{
    fsWriteToStream(&pCtx->mFile, pText, strlen(pText));
    fsWriteToStream(&pCtx->mFile, "\n", 1);
}

static void check(TestContext* pCtx, bool ok, const char* pExpr, int32_t line)
{
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "  %s line %d: %s", ok ? "pass" : "FAIL", line, pExpr);
    writeLine(pCtx, buffer);
    if (!ok)
        ++pCtx->mFailures;
}

#define CHECK(cond) check(pCtx, (cond), #cond, __LINE__)

static void testCellMath(TestContext* pCtx)
{
    int32_t x = -1;
    int32_t z = -1;
    CHECK(worldToCell(0.0f, 0.0f, &x, &z) && x == 0 && z == 0);
    CHECK(worldToCell(79.99f, 79.99f, &x, &z) && x == 15 && z == 15);
    CHECK(worldToCell(12.5f, 37.4f, &x, &z) && x == 2 && z == 7);
    CHECK(!worldToCell(-0.01f, 10.0f, &x, &z));
    CHECK(!worldToCell(10.0f, 80.0f, &x, &z));
    CHECK(!worldToCell(NAN, 10.0f, &x, &z));

    const float2 center = cellCenter(2, 7);
    CHECK(center.x == 12.5f && center.y == 37.5f);

    CHECK(rayToCell(float3(40.0f, 50.0f, 40.0f), float3(0.0f, -1.0f, 0.0f), &x, &z) && x == 8 && z == 8);
    CHECK(rayToCell(float3(40.0f, 50.0f, -7.5f), float3(0.0f, -0.70710678f, 0.70710678f), &x, &z) && x == 8 && z == 8);
    CHECK(!rayToCell(float3(40.0f, 50.0f, 40.0f), float3(0.0f, 0.0f, 1.0f), &x, &z));   // parallel to ground
    CHECK(!rayToCell(float3(40.0f, 50.0f, 40.0f), float3(0.0f, 1.0f, 0.0f), &x, &z));   // pointing away
    CHECK(!rayToCell(float3(40.0f, 50.0f, 0.0f), float3(0.0f, -0.1f, -1.0f), &x, &z));  // lands off the arena
}

static void testBuySell(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    CHECK(simGetPhase(s) == PHASE_PREP && simGetRound(s) == 1 && simGetGold(s) == START_GOLD);
    const uint32_t enemyUnits = simGetUnitCount(s, TEAM_ENEMY);
    CHECK(enemyUnits > 0u);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 0u);

    CHECK(simBuySquad(s, 3, 2, UNIT_MELEE));
    CHECK(simGetGold(s) == START_GOLD - 2);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 25u);
    CHECK(simGetCell(s, 3, 2) == UNIT_MELEE);

    CHECK(!simBuySquad(s, 3, 2, UNIT_TANK));
    CHECK(strcmp(simGetMessage(s), "That cell already has a squad") == 0);
    CHECK(!simBuySquad(s, 3, PLAYER_ROWS, UNIT_MELEE));
    CHECK(!simBuySquad(s, -1, 0, UNIT_MELEE));
    CHECK(!simBuySquad(s, 0, 0, UNIT_NONE));

    CHECK(simBuySquad(s, 0, 0, UNIT_TANK)); // 8 -> 4
    CHECK(simBuySquad(s, 1, 0, UNIT_TANK)); // 4 -> 0
    CHECK(!simBuySquad(s, 2, 0, UNIT_MELEE));
    CHECK(strcmp(simGetMessage(s), "Not enough gold") == 0);
    CHECK(simGetGold(s) == 0);

    CHECK(simSellSquad(s, 3, 2));
    CHECK(simGetGold(s) == 2 && simGetCell(s, 3, 2) == UNIT_NONE);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 18u);
    CHECK(simGetUnitCount(s, TEAM_ENEMY) == enemyUnits);
    CHECK(!simSellSquad(s, 3, 2));
    CHECK(!simSellSquad(s, 0, PLAYER_ROWS));
    simDestroy(s);
}

static void testUnitCap(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetGold(s, 100000);
    for (int32_t z = 0; z < PLAYER_ROWS; ++z)
        for (int32_t x = 0; x < ARENA_CELLS; ++x)
            if (!(x == ARENA_CELLS - 1 && z == PLAYER_ROWS - 1))
                simBuySquad(s, x, z, UNIT_MELEE);
    for (int32_t i = 0; i < 36; ++i)
        simTestSetCell(s, i % ARENA_CELLS, PLAYER_ROWS + i / ARENA_CELLS, UNIT_MELEE);

    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 127u * 25u);
    CHECK(simGetUnitCount(s, TEAM_ENEMY) == 36u * 25u);
    CHECK(!simBuySquad(s, ARENA_CELLS - 1, PLAYER_ROWS - 1, UNIT_MELEE)); // 4075 + 25 > 4096
    CHECK(strcmp(simGetMessage(s), "Unit cap reached") == 0);
    CHECK(simBuySquad(s, ARENA_CELLS - 1, PLAYER_ROWS - 1, UNIT_RANGED)); // 4075 + 16 fits
    CHECK(!simTestSetCell(s, 0, ARENA_CELLS - 1, UNIT_MELEE));             // hook respects the cap too
    CHECK(simGetUnitCount(s, TEAM_PLAYER) + simGetUnitCount(s, TEAM_ENEMY) <= MAX_UNITS);
    simDestroy(s);
}

static void testWaveDeterministic(TestContext* pCtx)
{
    int8_t first[ARENA_CELLS * ARENA_CELLS];
    BattleSim* s = simCreate(1);
    int32_t    cost = 0;
    int32_t    enemySquads = 0;
    bool       playerHalfEmpty = true;
    for (int32_t z = 0; z < ARENA_CELLS; ++z)
    {
        for (int32_t x = 0; x < ARENA_CELLS; ++x)
        {
            const UnitType type = simGetCell(s, x, z);
            first[z * ARENA_CELLS + x] = type;
            if (type == UNIT_NONE)
                continue;
            if (z < PLAYER_ROWS)
                playerHalfEmpty = false;
            else
            {
                cost += gUnitStats[type].mCost;
                ++enemySquads;
            }
        }
    }
    CHECK(enemySquads > 0);
    CHECK(cost <= 6 + 4 * 1);
    CHECK(playerHalfEmpty);
    simDestroy(s);

    s = simCreate(4);
    bool same = true;
    for (int32_t z = 0; z < ARENA_CELLS; ++z)
        for (int32_t x = 0; x < ARENA_CELLS; ++x)
            same = same && first[z * ARENA_CELLS + x] == simGetCell(s, x, z);
    CHECK(same);
    simDestroy(s);
}

typedef void (*TestFn)(TestContext*);

struct NamedTest
{
    const char* pName;
    TestFn      pFn;
};

int runAutoBattlerSelfTest(const char* pResultFileName)
{
    TestContext ctx = {};
    if (!fsOpenStreamFromPath(RD_LOG, pResultFileName, FM_WRITE, &ctx.mFile))
        return 1;

    const NamedTest tests[] = {
        { "cell math", testCellMath },
        { "buy and sell", testBuySell },
        { "unit cap", testUnitCap },
        { "wave is deterministic", testWaveDeterministic },
    };
    for (uint32_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    {
        writeLine(&ctx, tests[i].pName);
        tests[i].pFn(&ctx);
    }

    char summary[64];
    snprintf(summary, sizeof(summary), "RESULT: %d failure(s)", ctx.mFailures);
    writeLine(&ctx, summary);
    fsCloseStream(&ctx.mFile);
    return ctx.mFailures;
}
```

- [ ] **Step 5: Write the minimal app entry point**

`Examples_3/Unit_Tests/src/40_AutoBattler/40_AutoBattler.cpp` (Task 4 replaces this file):

```cpp
// 40_AutoBattler: top-down auto battler. Gameplay lives in AutoBattlerSim (Flecs); this file owns
// rendering, camera, input and UI.

#include "AutoBattlerSim.h"

#include <stdlib.h>
#include <string.h>

#include "../../../../Common_3/Application/Interfaces/IApp.h"
#include "../../../../Common_3/Graphics/Interfaces/IGraphics.h"

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

class AutoBattler: public IApp
{
public:
    bool Init()
    {
        for (int i = 1; i < argc; ++i)
        {
            if (strcmp(argv[i], "--selftest") == 0)
                exit(runAutoBattlerSelfTest("40_AutoBattler_selftest.txt") == 0 ? 0 : 1);
        }
        ShowUnsupportedMessage("Rendering is not implemented yet; run with --selftest");
        return false;
    }

    void Exit() {}

    bool Load(ReloadDesc*) { return true; }

    void Unload(ReloadDesc*) {}

    void Update(float) {}

    void Draw() {}

    const char* GetName() { return "40_AutoBattler"; }
};

DEFINE_APPLICATION_MAIN(AutoBattler)
```

- [ ] **Step 6: Build to verify the tests fail**

Run the shared **Build** command (Debug).
Expected: FAIL at link time with unresolved externals such as `simCreate`, `simBuySquad`, `worldToCell` (there is no `AutoBattlerSim.cpp` yet; create an empty one if MSBuild complains about the missing source file instead).

- [ ] **Step 7: Implement the sim**

`Examples_3/Unit_Tests/src/40_AutoBattler/AutoBattlerSim.cpp`:

```cpp
#include "AutoBattlerSim.h"

#include <math.h>
#include <string.h>

#include "../../../../Common_3/Game/ThirdParty/OpenSource/flecs/flecs.h"
#include "../../../../Common_3/Utilities/Interfaces/ILog.h"

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

const UnitStats gUnitStats[UNIT_TYPE_COUNT] = {
    // name     side cost  hp      damage cooldown range speed scale
    { "Melee", 5, 2, 100.0f, 15.0f, 0.8f, 1.2f, 3.0f, 1.0f },
    { "Ranged", 4, 3, 60.0f, 12.0f, 1.0f, 8.0f, 2.5f, 0.9f },
    { "Tank", 3, 4, 250.0f, 8.0f, 1.2f, 1.4f, 1.8f, 1.3f },
};

static const float RETARGET_INTERVAL = 0.25f;

//--------------------------------------------------------------------------------------------
// Components
//--------------------------------------------------------------------------------------------
struct Position
{
    float x, z;
};

struct Unit
{
    uint8_t  team;
    uint8_t  type;
    uint16_t cell; // board cell index, 0xFFFF when not spawned from the board
    float    radius;
    float    yaw;
};

struct Health
{
    float hp, maxHp, hitFlash;
};

struct Attack
{
    ecs_entity_t target;
    float        timer;
    float        retargetTimer;
    uint32_t     inRange;
};

struct GridSlot
{
    uint32_t slot; // index into this frame's grid snapshot
};

struct Projectile
{
    ecs_entity_t target;
    float        x, y, z;
    float        damage;
};

ECS_COMPONENT_DECLARE(Position);
ECS_COMPONENT_DECLARE(Unit);
ECS_COMPONENT_DECLARE(Health);
ECS_COMPONENT_DECLARE(Attack);
ECS_COMPONENT_DECLARE(GridSlot);
ECS_COMPONENT_DECLARE(Projectile);

// Component ids are globals; reset them so each new world registers fresh ids.
#define AB_DEFINE_COMPONENT(world, T) \
    ecs_id(T) = 0;                    \
    ECS_COMPONENT_DEFINE(world, T)

struct BattleSim
{
    ecs_world_t* pWorld;
    ecs_query_t* pUnitQuery; // Position, Unit, Health
    ecs_query_t* pProjectileQuery;
    int8_t       mBoard[ARENA_CELLS * ARENA_CELLS]; // UnitType per cell, UNIT_NONE when empty
    Phase        mPhase;
    int32_t      mRound;
    int32_t      mGold;
    float        mBattleTime;
    float        mResultTime;
    uint32_t     mProjectileCount;
    uint32_t     mSpawnCounter;
    const char*  pMessage;
};

//--------------------------------------------------------------------------------------------
// Board helpers
//--------------------------------------------------------------------------------------------
static bool cellInArena(int32_t x, int32_t z) { return x >= 0 && x < ARENA_CELLS && z >= 0 && z < ARENA_CELLS; }

static int32_t cellIndex(int32_t x, int32_t z) { return z * ARENA_CELLS + x; }

static uint32_t squadSize(UnitType type)
{
    const uint32_t side = (uint32_t)gUnitStats[type].mSquadSide;
    return side * side;
}

static uint32_t boardUnitCount(const BattleSim* p)
{
    uint32_t units = 0;
    for (int32_t i = 0; i < ARENA_CELLS * ARENA_CELLS; ++i)
        if (p->mBoard[i] != UNIT_NONE)
            units += squadSize((UnitType)p->mBoard[i]);
    return units;
}

bool worldToCell(float x, float z, int32_t* pCellX, int32_t* pCellZ)
{
    // Written as a positive test so NaN is rejected too.
    if (!(x >= 0.0f && x < ARENA_SIZE && z >= 0.0f && z < ARENA_SIZE))
        return false;
    const int32_t cellX = (int32_t)(x / CELL_SIZE);
    const int32_t cellZ = (int32_t)(z / CELL_SIZE);
    *pCellX = cellX < ARENA_CELLS ? cellX : ARENA_CELLS - 1;
    *pCellZ = cellZ < ARENA_CELLS ? cellZ : ARENA_CELLS - 1;
    return true;
}

float2 cellCenter(int32_t cellX, int32_t cellZ) { return float2(((float)cellX + 0.5f) * CELL_SIZE, ((float)cellZ + 0.5f) * CELL_SIZE); }

bool rayToCell(const float3& origin, const float3& dir, int32_t* pCellX, int32_t* pCellZ)
{
    if (!(dir.y < -1e-4f)) // parallel to the ground or pointing up
        return false;
    const float t = -origin.y / dir.y;
    if (t < 0.0f)
        return false;
    return worldToCell(origin.x + dir.x * t, origin.z + dir.z * t, pCellX, pCellZ);
}

//--------------------------------------------------------------------------------------------
// Spawning
//--------------------------------------------------------------------------------------------
static void spawnUnit(BattleSim* p, Team team, UnitType type, float x, float z, uint16_t cell)
{
    const UnitStats& stats = gUnitStats[type];
    const ecs_entity_t e = ecs_new(p->pWorld);

    const Position pos = { x, z };
    const Unit     unit = { (uint8_t)team, (uint8_t)type, cell, UNIT_RADIUS * stats.mScale, team == TEAM_PLAYER ? 0.0f : PI };
    const Health   health = { stats.mHp, stats.mHp, 0.0f };
    // Stagger retargeting so only a slice of the army searches each frame.
    const Attack   attack = { 0, 0.0f, RETARGET_INTERVAL * (float)(p->mSpawnCounter++ % 16u) / 16.0f, 0u };
    const GridSlot slot = { UINT32_MAX };

    ecs_set(p->pWorld, e, Position, pos);
    ecs_set(p->pWorld, e, Unit, unit);
    ecs_set(p->pWorld, e, Health, health);
    ecs_set(p->pWorld, e, Attack, attack);
    ecs_set(p->pWorld, e, GridSlot, slot);
}

static void spawnSquad(BattleSim* p, int32_t cellX, int32_t cellZ, UnitType type)
{
    const int32_t side = gUnitStats[type].mSquadSide;
    const float   spacing = CELL_SIZE / (float)side;
    const Team    team = cellZ < PLAYER_ROWS ? TEAM_PLAYER : TEAM_ENEMY;
    for (int32_t j = 0; j < side; ++j)
        for (int32_t i = 0; i < side; ++i)
            spawnUnit(p, team, type, (float)cellX * CELL_SIZE + ((float)i + 0.5f) * spacing,
                      (float)cellZ * CELL_SIZE + ((float)j + 0.5f) * spacing, (uint16_t)cellIndex(cellX, cellZ));
}

// Units on the field always mirror the board while not in battle.
static void rebuildFromBoard(BattleSim* p)
{
    ecs_delete_with(p->pWorld, ecs_id(Unit));
    ecs_delete_with(p->pWorld, ecs_id(Projectile));
    p->mProjectileCount = 0;
    for (int32_t z = 0; z < ARENA_CELLS; ++z)
        for (int32_t x = 0; x < ARENA_CELLS; ++x)
            if (p->mBoard[cellIndex(x, z)] != UNIT_NONE)
                spawnSquad(p, x, z, (UnitType)p->mBoard[cellIndex(x, z)]);
}

static uint32_t nextRandom(uint32_t* pState)
{
    uint32_t s = *pState;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *pState = s;
    return s;
}

// Fills the enemy half for the current round. Seeded by the round so waves are repeatable.
static void generateWave(BattleSim* p)
{
    for (int32_t z = PLAYER_ROWS; z < ARENA_CELLS; ++z)
        for (int32_t x = 0; x < ARENA_CELLS; ++x)
            p->mBoard[cellIndex(x, z)] = UNIT_NONE;

    uint32_t state = 0x9E3779B9u ^ ((uint32_t)p->mRound * 2654435761u);
    if (state == 0)
        state = 1;
    int32_t  budget = 6 + 4 * p->mRound;
    uint32_t units = boardUnitCount(p);
    for (int32_t attempt = 0; attempt < 256 && budget >= gUnitStats[UNIT_MELEE].mCost; ++attempt)
    {
        const UnitType type = (UnitType)(nextRandom(&state) % (uint32_t)UNIT_TYPE_COUNT);
        const int32_t  x = (int32_t)(nextRandom(&state) % (uint32_t)ARENA_CELLS);
        const int32_t  z = PLAYER_ROWS + (int32_t)(nextRandom(&state) % (uint32_t)(ARENA_CELLS - PLAYER_ROWS));
        if (gUnitStats[type].mCost > budget || p->mBoard[cellIndex(x, z)] != UNIT_NONE)
            continue;
        if (units + squadSize(type) > MAX_UNITS)
            break;
        p->mBoard[cellIndex(x, z)] = type;
        budget -= gUnitStats[type].mCost;
        units += squadSize(type);
    }
}

//--------------------------------------------------------------------------------------------
// Lifetime
//--------------------------------------------------------------------------------------------
BattleSim* simCreate(uint32_t threadCount)
{
    static bool sEcsInitialized = false;
    if (!sEcsInitialized)
    {
        initEntityComponentSystem();
        ecs_log_set_level(-2);
        sEcsInitialized = true;
    }

    BattleSim* p = (BattleSim*)tf_calloc(1, sizeof(BattleSim));
    p->pWorld = ecs_init();
    ecs_set_threads(p->pWorld, (int32_t)(threadCount > 0 ? threadCount : 1));

    AB_DEFINE_COMPONENT(p->pWorld, Position);
    AB_DEFINE_COMPONENT(p->pWorld, Unit);
    AB_DEFINE_COMPONENT(p->pWorld, Health);
    AB_DEFINE_COMPONENT(p->pWorld, Attack);
    AB_DEFINE_COMPONENT(p->pWorld, GridSlot);
    AB_DEFINE_COMPONENT(p->pWorld, Projectile);

    ecs_query_desc_t unitQuery = {};
    unitQuery.terms[0].id = ecs_id(Position);
    unitQuery.terms[1].id = ecs_id(Unit);
    unitQuery.terms[2].id = ecs_id(Health);
    p->pUnitQuery = ecs_query_init(p->pWorld, &unitQuery);

    ecs_query_desc_t projectileQuery = {};
    projectileQuery.terms[0].id = ecs_id(Projectile);
    p->pProjectileQuery = ecs_query_init(p->pWorld, &projectileQuery);

    simRestart(p);
    return p;
}

void simDestroy(BattleSim* p)
{
    if (!p)
        return;
    ecs_query_fini(p->pUnitQuery);
    ecs_query_fini(p->pProjectileQuery);
    ecs_fini(p->pWorld);
    tf_free(p);
}

void simRestart(BattleSim* p)
{
    memset(p->mBoard, UNIT_NONE, sizeof(p->mBoard));
    p->mRound = 1;
    p->mGold = START_GOLD;
    p->mBattleTime = 0.0f;
    p->mResultTime = 0.0f;
    p->pMessage = "";
    generateWave(p);
    rebuildFromBoard(p);
    p->mPhase = PHASE_PREP;
}

//--------------------------------------------------------------------------------------------
// Board editing
//--------------------------------------------------------------------------------------------
static const char* buyRefusal(const BattleSim* p, int32_t x, int32_t z, UnitType type)
{
    if (p->mPhase != PHASE_PREP)
        return "Squads can only be placed during prep";
    if (type < 0 || type >= UNIT_TYPE_COUNT)
        return "Pick a unit type";
    if (!cellInArena(x, z) || z >= PLAYER_ROWS)
        return "Place squads on your half (bottom)";
    if (p->mBoard[cellIndex(x, z)] != UNIT_NONE)
        return "That cell already has a squad";
    if (p->mGold < gUnitStats[type].mCost)
        return "Not enough gold";
    if (boardUnitCount(p) + squadSize(type) > MAX_UNITS)
        return "Unit cap reached";
    return NULL;
}

bool simCanBuySquad(const BattleSim* p, int32_t cellX, int32_t cellZ, UnitType type) { return buyRefusal(p, cellX, cellZ, type) == NULL; }

bool simBuySquad(BattleSim* p, int32_t cellX, int32_t cellZ, UnitType type)
{
    const char* pRefusal = buyRefusal(p, cellX, cellZ, type);
    if (pRefusal)
    {
        p->pMessage = pRefusal;
        return false;
    }
    p->mBoard[cellIndex(cellX, cellZ)] = type;
    p->mGold -= gUnitStats[type].mCost;
    p->pMessage = "";
    spawnSquad(p, cellX, cellZ, type);
    return true;
}

bool simSellSquad(BattleSim* p, int32_t cellX, int32_t cellZ)
{
    if (p->mPhase != PHASE_PREP)
    {
        p->pMessage = "Squads can only be sold during prep";
        return false;
    }
    if (!cellInArena(cellX, cellZ) || cellZ >= PLAYER_ROWS || p->mBoard[cellIndex(cellX, cellZ)] == UNIT_NONE)
    {
        p->pMessage = "Nothing of yours to sell there";
        return false;
    }
    p->mGold += gUnitStats[p->mBoard[cellIndex(cellX, cellZ)]].mCost;
    p->mBoard[cellIndex(cellX, cellZ)] = UNIT_NONE;
    p->pMessage = "";
    rebuildFromBoard(p);
    return true;
}

UnitType simGetCell(const BattleSim* p, int32_t cellX, int32_t cellZ)
{
    return cellInArena(cellX, cellZ) ? (UnitType)p->mBoard[cellIndex(cellX, cellZ)] : UNIT_NONE;
}

//--------------------------------------------------------------------------------------------
// Queries
//--------------------------------------------------------------------------------------------
Phase simGetPhase(const BattleSim* p) { return p->mPhase; }

int32_t simGetRound(const BattleSim* p) { return p->mRound; }

int32_t simGetGold(const BattleSim* p) { return p->mGold; }

const char* simGetMessage(const BattleSim* p) { return p->pMessage; }

uint32_t simGetUnitCount(BattleSim* p, Team team)
{
    uint32_t   count = 0;
    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pUnitQuery);
    while (ecs_query_next(&it))
    {
        const Unit* unit = ecs_field(&it, Unit, 1);
        for (int32_t i = 0; i < it.count; ++i)
            if (unit[i].team == team)
                ++count;
    }
    return count;
}

//--------------------------------------------------------------------------------------------
// Test hooks
//--------------------------------------------------------------------------------------------
bool simTestSetCell(BattleSim* p, int32_t cellX, int32_t cellZ, UnitType type)
{
    if (!cellInArena(cellX, cellZ))
        return false;
    const int8_t previous = p->mBoard[cellIndex(cellX, cellZ)];
    p->mBoard[cellIndex(cellX, cellZ)] = type;
    if (boardUnitCount(p) > MAX_UNITS)
    {
        p->mBoard[cellIndex(cellX, cellZ)] = previous;
        return false;
    }
    rebuildFromBoard(p);
    return true;
}

void simTestClearBoard(BattleSim* p)
{
    memset(p->mBoard, UNIT_NONE, sizeof(p->mBoard));
    rebuildFromBoard(p);
}

void simTestSetGold(BattleSim* p, int32_t gold) { p->mGold = gold; }
```

Note for the implementer: `simTestSetCell(s, 0, ARENA_CELLS - 1, UNIT_MELEE)` in `testUnitCap` must be refused because the board then holds 4,091 + 25 units.

- [ ] **Step 8: Build and run the self test**

Run the shared **Build** command (Debug), then the shared **Self test** commands.
Expected: build succeeds with no warnings; `exit=0`; report ends with `RESULT: 0 failure(s)`.

If `DEFINE_APPLICATION_MAIN` fails to compile because a type is missing, add the remaining includes that 28_Skinning uses (`IFont.h`, `IUI.h`, `IResourceLoader.h`) to `40_AutoBattler.cpp`; Task 4 includes them anyway.

- [ ] **Step 9: Commit**

```bash
git add Examples_3/Unit_Tests/src/40_AutoBattler Examples_3/Unit_Tests/PC_VS2019/40_AutoBattler.vcxproj Examples_3/Unit_Tests/PC_VS2019/Unit_Tests.sln
git commit -m "Add 40_AutoBattler project with board rules and self test

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Battle systems (spatial grid, steering, combat, round flow)

**Files:**
- Modify: `Examples_3/Unit_Tests/src/40_AutoBattler/AutoBattlerSim.h`
- Modify: `Examples_3/Unit_Tests/src/40_AutoBattler/AutoBattlerSim.cpp`
- Modify: `Examples_3/Unit_Tests/src/40_AutoBattler/AutoBattlerSelfTest.cpp`

**Interfaces:**
- Consumes (Task 1): `BattleSim`, components, `spawnUnit`, `rebuildFromBoard`, `generateWave`, `boardUnitCount`, `cellIndex`, `RETARGET_INTERVAL`.
- Produces: `bool simStartBattle(BattleSim*)`, `void simStep(BattleSim*, float deltaTime)`, `float simGetBattleTime(const BattleSim*)`, `uint32_t simGetProjectileCount(const BattleSim*)`, test hooks `simTestSpawnUnit`, `simTestSetBattleTime`, `simTestTotalHp`, `simTestAllPositionsFinite`, `simTestMinUnitDistance`. Task 4 calls `simStep` every frame.

- [ ] **Step 1: Declare the new API**

Add to `AutoBattlerSim.h` after `simRestart`:

```cpp
// PREP with at least one player squad -> BATTLE. Otherwise refused (see simGetMessage()).
bool simStartBattle(BattleSim* pSim);

// Advances the battle or result timer. Ignores NaN/non-positive steps and clamps to MAX_STEP.
void simStep(BattleSim* pSim, float deltaTime);
```

Add after `simGetUnitCount`:

```cpp
float    simGetBattleTime(const BattleSim* pSim);
uint32_t simGetProjectileCount(const BattleSim* pSim);
```

Add after `simTestSetGold`:

```cpp
void  simTestSpawnUnit(BattleSim* pSim, Team team, UnitType type, float x, float z);
void  simTestSetBattleTime(BattleSim* pSim, float seconds);
float simTestTotalHp(BattleSim* pSim, Team team);
bool  simTestAllPositionsFinite(BattleSim* pSim);
float simTestMinUnitDistance(BattleSim* pSim, Team team);
```

- [ ] **Step 2: Write the failing tests**

Add to `AutoBattlerSelfTest.cpp` above `typedef void (*TestFn)`:

```cpp
static void runBattle(BattleSim* s, float seconds)
{
    const int32_t steps = (int32_t)(seconds * 60.0f);
    for (int32_t i = 0; i < steps && simGetPhase(s) == PHASE_BATTLE; ++i)
        simStep(s, 1.0f / 60.0f);
}

static void stepFor(BattleSim* s, float seconds)
{
    const int32_t steps = (int32_t)(seconds * 60.0f);
    for (int32_t i = 0; i < steps; ++i)
        simStep(s, 1.0f / 60.0f);
}

static void setupMeleeSkirmish(BattleSim* s)
{
    simTestClearBoard(s);
    simTestSetCell(s, 7, 6, UNIT_MELEE);
    simTestSetCell(s, 8, 6, UNIT_MELEE);
    simTestSetCell(s, 7, 9, UNIT_MELEE);
}

static void testStartBattleGuards(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    CHECK(!simStartBattle(s));
    CHECK(simGetPhase(s) == PHASE_PREP);
    CHECK(strcmp(simGetMessage(s), "Place at least one squad first") == 0);
    CHECK(simBuySquad(s, 0, 0, UNIT_MELEE));
    CHECK(simStartBattle(s));
    CHECK(simGetPhase(s) == PHASE_BATTLE);
    CHECK(!simStartBattle(s));
    CHECK(!simBuySquad(s, 1, 0, UNIT_MELEE));
    CHECK(strcmp(simGetMessage(s), "Squads can only be placed during prep") == 0);
    CHECK(!simSellSquad(s, 0, 0));
    simDestroy(s);
}

static void testMeleeBattleAndNextRound(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    setupMeleeSkirmish(s);
    CHECK(simStartBattle(s));
    runBattle(s, 60.0f);
    CHECK(simGetPhase(s) == PHASE_VICTORY);
    CHECK(simGetUnitCount(s, TEAM_ENEMY) == 0u);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) > 0u);
    CHECK(simGetBattleTime(s) < 60.0f);

    stepFor(s, RESULT_DURATION + 0.5f);
    CHECK(simGetPhase(s) == PHASE_PREP);
    CHECK(simGetRound(s) == 2);
    CHECK(simGetGold(s) == START_GOLD + 5 + 2);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 50u); // army restored at full strength
    CHECK(simGetCell(s, 7, 6) == UNIT_MELEE && simGetCell(s, 8, 6) == UNIT_MELEE);
    CHECK(simGetUnitCount(s, TEAM_ENEMY) > 0u);    // new wave
    simDestroy(s);
}

static void testRangedProjectiles(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 8, 7, UNIT_RANGED);
    simTestSetCell(s, 8, 8, UNIT_TANK);
    const float enemyHpBefore = simTestTotalHp(s, TEAM_ENEMY);
    CHECK(enemyHpBefore == 9.0f * 250.0f);
    CHECK(simStartBattle(s));
    uint32_t maxProjectiles = 0;
    for (int32_t i = 0; i < 120 && simGetPhase(s) == PHASE_BATTLE; ++i)
    {
        simStep(s, 1.0f / 60.0f);
        if (simGetProjectileCount(s) > maxProjectiles)
            maxProjectiles = simGetProjectileCount(s);
    }
    CHECK(maxProjectiles > 0u);
    CHECK(maxProjectiles <= MAX_PROJECTILES);
    CHECK(simTestTotalHp(s, TEAM_ENEMY) < enemyHpBefore);
    simDestroy(s);
}

static void testDefeatAndRestart(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 7, 6, UNIT_MELEE);
    for (int32_t x = 3; x < 13; ++x)
        simTestSetCell(s, x, 9, UNIT_TANK);
    CHECK(simStartBattle(s));
    runBattle(s, 60.0f);
    CHECK(simGetPhase(s) == PHASE_DEFEAT);
    stepFor(s, 10.0f);
    CHECK(simGetPhase(s) == PHASE_DEFEAT); // stays until restart
    CHECK(simGetRound(s) == 1);
    simRestart(s);
    CHECK(simGetPhase(s) == PHASE_PREP && simGetRound(s) == 1 && simGetGold(s) == START_GOLD);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 0u);
    simDestroy(s);
}

static void testTimeout(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 0, 0, UNIT_MELEE);
    simTestSetCell(s, 15, 15, UNIT_TANK);
    CHECK(simStartBattle(s));
    simTestSetBattleTime(s, BATTLE_TIME_LIMIT - 0.001f);
    simStep(s, 1.0f / 60.0f);
    CHECK(simGetPhase(s) == PHASE_DEFEAT);
    CHECK(strcmp(simGetMessage(s), "Time ran out") == 0);
    simDestroy(s);
}

static void testCoincidentUnitsSeparate(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 0, 0, UNIT_MELEE);
    simTestSetCell(s, 15, 15, UNIT_MELEE);
    simTestSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, 40.0f, 20.0f);
    simTestSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, 40.0f, 20.0f);
    CHECK(simTestMinUnitDistance(s, TEAM_PLAYER) < 1e-6f);
    CHECK(simStartBattle(s));
    stepFor(s, 1.0f);
    CHECK(simTestAllPositionsFinite(s));
    CHECK(simTestMinUnitDistance(s, TEAM_PLAYER) > 0.2f);
    simDestroy(s);
}

static void testDeltaTimeClamp(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    setupMeleeSkirmish(s);
    CHECK(simStartBattle(s));
    simStep(s, 10.0f);
    CHECK(simGetBattleTime(s) <= MAX_STEP + 1e-6f);
    const float t = simGetBattleTime(s);
    simStep(s, NAN);
    simStep(s, -1.0f);
    simStep(s, 0.0f);
    CHECK(simGetBattleTime(s) == t);
    CHECK(simTestAllPositionsFinite(s));
    simDestroy(s);
}

static void testRestartMidBattle(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 8, 7, UNIT_RANGED);
    simTestSetCell(s, 8, 8, UNIT_TANK);
    CHECK(simStartBattle(s));
    stepFor(s, 0.1f);
    CHECK(simGetProjectileCount(s) > 0u);
    simRestart(s);
    CHECK(simGetPhase(s) == PHASE_PREP);
    CHECK(simGetProjectileCount(s) == 0u);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 0u);
    simDestroy(s);
}

struct BattleOutcome
{
    Phase    mPhase;
    float    mTime;
    uint32_t mPlayers;
    uint32_t mEnemies;
    float    mPlayerHp;
};

static BattleOutcome runScriptedBattle(uint32_t threads)
{
    BattleSim* s = simCreate(threads);
    simTestClearBoard(s);
    simTestSetCell(s, 7, 6, UNIT_MELEE);
    simTestSetCell(s, 8, 6, UNIT_MELEE);
    simTestSetCell(s, 8, 5, UNIT_RANGED);
    simTestSetCell(s, 7, 9, UNIT_MELEE);
    simTestSetCell(s, 8, 9, UNIT_TANK);
    simStartBattle(s);
    runBattle(s, 60.0f);
    const BattleOutcome outcome = { simGetPhase(s), simGetBattleTime(s), simGetUnitCount(s, TEAM_PLAYER), simGetUnitCount(s, TEAM_ENEMY),
                                    simTestTotalHp(s, TEAM_PLAYER) };
    simDestroy(s);
    return outcome;
}

static void testThreadCountDoesNotChangeOutcome(TestContext* pCtx)
{
    const BattleOutcome single = runScriptedBattle(1);
    const BattleOutcome multi = runScriptedBattle(4);
    CHECK(single.mPhase != PHASE_BATTLE);
    CHECK(single.mPhase == multi.mPhase);
    CHECK(single.mTime == multi.mTime);
    CHECK(single.mPlayers == multi.mPlayers && single.mEnemies == multi.mEnemies);
    CHECK(single.mPlayerHp == multi.mPlayerHp);
}
```

Append to the `tests[]` array in `runAutoBattlerSelfTest`:

```cpp
        { "start battle guards", testStartBattleGuards },
        { "melee battle and next round", testMeleeBattleAndNextRound },
        { "ranged projectiles", testRangedProjectiles },
        { "defeat and restart", testDefeatAndRestart },
        { "timeout", testTimeout },
        { "coincident units separate", testCoincidentUnitsSeparate },
        { "delta time clamp", testDeltaTimeClamp },
        { "restart mid battle", testRestartMidBattle },
        { "thread count does not change outcome", testThreadCountDoesNotChangeOutcome },
```

- [ ] **Step 3: Build to verify the tests fail**

Run the shared **Build** command (Debug).
Expected: FAIL at link time with unresolved `simStartBattle`, `simStep`, `simTestSpawnUnit`, …

- [ ] **Step 4: Implement the grid, systems and round flow**

In `AutoBattlerSim.cpp`:

Add includes after `<math.h>`:

```cpp
#include <float.h>
```

Add after `RETARGET_INTERVAL`:

```cpp
static const float   SEPARATION_SPEED = 4.0f;
static const float   GRID_BUCKET_SIZE = 2.0f;
static const int32_t GRID_DIM = (int32_t)(ARENA_SIZE / GRID_BUCKET_SIZE);
```

Add above `struct BattleSim`:

```cpp
struct GridEntry
{
    ecs_entity_t mEntity;
    float        mX, mZ, mRadius;
    uint32_t     mTeam;
};

// Per-frame snapshot of every unit, sorted by bucket. Systems read positions of *other* units only
// from here, so the multithreaded steering system never reads data another thread is writing.
struct SpatialGrid
{
    GridEntry* pEntries;      // MAX_UNITS, sorted by bucket
    GridEntry* pScratch;      // MAX_UNITS, query order
    uint32_t*  pBucketStart;  // GRID_DIM * GRID_DIM + 1
    uint32_t*  pBucketCursor; // GRID_DIM * GRID_DIM
    uint32_t   mCount;
};
```

Add to `struct BattleSim` (after `pMessage`):

```cpp
    SpatialGrid  mGrid;
```

Add after the spawning section:

```cpp
//--------------------------------------------------------------------------------------------
// Spatial grid
//--------------------------------------------------------------------------------------------
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int32_t bucketCoord(float v)
{
    const int32_t b = (int32_t)(v / GRID_BUCKET_SIZE);
    return b < 0 ? 0 : (b >= GRID_DIM ? GRID_DIM - 1 : b);
}

static uint32_t bucketOf(float x, float z) { return (uint32_t)(bucketCoord(z) * GRID_DIM + bucketCoord(x)); }

static void buildGrid(BattleSim* p)
{
    SpatialGrid& g = p->mGrid;
    g.mCount = 0;
    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pUnitQuery);
    while (ecs_query_next(&it))
    {
        const Position* pos = ecs_field(&it, Position, 0);
        const Unit*     unit = ecs_field(&it, Unit, 1);
        for (int32_t i = 0; i < it.count; ++i)
        {
            ASSERT(g.mCount < MAX_UNITS);
            GridEntry& e = g.pScratch[g.mCount++];
            e.mEntity = it.entities[i];
            e.mX = pos[i].x;
            e.mZ = pos[i].z;
            e.mRadius = unit[i].radius;
            e.mTeam = unit[i].team;
        }
    }

    const uint32_t bucketCount = (uint32_t)(GRID_DIM * GRID_DIM);
    memset(g.pBucketStart, 0, sizeof(uint32_t) * (bucketCount + 1));
    for (uint32_t i = 0; i < g.mCount; ++i)
        ++g.pBucketStart[bucketOf(g.pScratch[i].mX, g.pScratch[i].mZ) + 1];
    for (uint32_t b = 0; b < bucketCount; ++b)
        g.pBucketStart[b + 1] += g.pBucketStart[b];
    memcpy(g.pBucketCursor, g.pBucketStart, sizeof(uint32_t) * bucketCount);
    for (uint32_t i = 0; i < g.mCount; ++i)
        g.pEntries[g.pBucketCursor[bucketOf(g.pScratch[i].mX, g.pScratch[i].mZ)]++] = g.pScratch[i];

    for (uint32_t s = 0; s < g.mCount; ++s)
        ecs_get_mut(p->pWorld, g.pEntries[s].mEntity, GridSlot)->slot = s;
}

static const GridEntry* lookupTarget(const ecs_world_t* pWorld, const SpatialGrid& g, ecs_entity_t target)
{
    if (!target || !ecs_is_alive(pWorld, target))
        return NULL;
    const GridSlot* pSlot = ecs_get(pWorld, target, GridSlot);
    if (!pSlot || pSlot->slot >= g.mCount || g.pEntries[pSlot->slot].mEntity != target)
        return NULL;
    return &g.pEntries[pSlot->slot];
}

// Searches rings of buckets outward and stops once no closer enemy can exist. Returns a slot or -1.
static int32_t findNearestEnemy(const SpatialGrid& g, float x, float z, uint32_t team)
{
    const int32_t cx = bucketCoord(x);
    const int32_t cz = bucketCoord(z);
    int32_t       best = -1;
    float         bestD2 = FLT_MAX;
    for (int32_t r = 0; r < GRID_DIM; ++r)
    {
        const float ringMin = (float)(r - 1) * GRID_BUCKET_SIZE;
        if (best >= 0 && ringMin > 0.0f && ringMin * ringMin > bestD2)
            break;
        for (int32_t bz = cz - r; bz <= cz + r; ++bz)
        {
            if (bz < 0 || bz >= GRID_DIM)
                continue;
            const bool    edgeRow = bz == cz - r || bz == cz + r;
            const int32_t step = edgeRow || r == 0 ? 1 : 2 * r; // interior rows: only the two ring columns
            for (int32_t bx = cx - r; bx <= cx + r; bx += step)
            {
                if (bx < 0 || bx >= GRID_DIM)
                    continue;
                const uint32_t b = (uint32_t)(bz * GRID_DIM + bx);
                for (uint32_t s = g.pBucketStart[b]; s < g.pBucketStart[b + 1]; ++s)
                {
                    const GridEntry& e = g.pEntries[s];
                    if (e.mTeam == team)
                        continue;
                    const float dx = e.mX - x;
                    const float dz = e.mZ - z;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < bestD2)
                    {
                        bestD2 = d2;
                        best = (int32_t)s;
                    }
                }
            }
        }
    }
    return best;
}

static void separation(const SpatialGrid& g, ecs_entity_t self, float x, float z, float radius, float* pOutX, float* pOutZ)
{
    float         sx = 0.0f;
    float         sz = 0.0f;
    const int32_t cx = bucketCoord(x);
    const int32_t cz = bucketCoord(z);
    for (int32_t bz = cz - 1; bz <= cz + 1; ++bz)
    {
        for (int32_t bx = cx - 1; bx <= cx + 1; ++bx)
        {
            if (bx < 0 || bx >= GRID_DIM || bz < 0 || bz >= GRID_DIM)
                continue;
            const uint32_t b = (uint32_t)(bz * GRID_DIM + bx);
            for (uint32_t s = g.pBucketStart[b]; s < g.pBucketStart[b + 1]; ++s)
            {
                const GridEntry& e = g.pEntries[s];
                if (e.mEntity == self)
                    continue;
                const float dx = x - e.mX;
                const float dz = z - e.mZ;
                const float d2 = dx * dx + dz * dz;
                const float minDist = radius + e.mRadius;
                if (d2 >= minDist * minDist)
                    continue;
                if (d2 > 1e-8f)
                {
                    const float d = sqrtf(d2);
                    const float push = (minDist - d) / minDist;
                    sx += dx / d * push;
                    sz += dz / d * push;
                }
                else
                {
                    // Exactly overlapping: push apart along a direction derived from the pair of ids,
                    // opposite for each of the two units.
                    const float angle = (float)(uint32_t)((self ^ e.mEntity) & 1023u) * (2.0f * PI / 1024.0f);
                    const float sign = self < e.mEntity ? 1.0f : -1.0f;
                    sx += cosf(angle) * sign;
                    sz += sinf(angle) * sign;
                }
            }
        }
    }
    *pOutX = sx;
    *pOutZ = sz;
}

//--------------------------------------------------------------------------------------------
// Systems
//--------------------------------------------------------------------------------------------
static void applyDamage(ecs_world_t* pWorld, ecs_entity_t target, float damage)
{
    if (!ecs_is_alive(pWorld, target))
        return;
    Health* pHealth = ecs_get_mut(pWorld, target, Health);
    if (pHealth)
    {
        pHealth->hp -= damage;
        pHealth->hitFlash = 1.0f;
    }
}

// Multithreaded: writes only its own entity's components and reads others from the grid snapshot.
static void SteerSystem(ecs_iter_t* it)
{
    const BattleSim*   p = (const BattleSim*)it->ctx;
    const SpatialGrid& g = p->mGrid;
    Position*          pos = ecs_field(it, Position, 0);
    Unit*              unit = ecs_field(it, Unit, 1);
    Attack*            attack = ecs_field(it, Attack, 2);
    const float        dt = it->delta_time;

    for (int32_t i = 0; i < it->count; ++i)
    {
        const UnitStats& stats = gUnitStats[unit[i].type];
        Attack&          a = attack[i];

        const GridEntry* pTarget = lookupTarget(it->world, g, a.target);
        a.retargetTimer -= dt;
        if (!pTarget || a.retargetTimer <= 0.0f)
        {
            a.retargetTimer = RETARGET_INTERVAL;
            const int32_t slot = findNearestEnemy(g, pos[i].x, pos[i].z, unit[i].team);
            pTarget = slot >= 0 ? &g.pEntries[slot] : NULL;
            a.target = pTarget ? pTarget->mEntity : 0;
        }

        float vx = 0.0f;
        float vz = 0.0f;
        a.inRange = 0;
        if (pTarget)
        {
            const float dx = pTarget->mX - pos[i].x;
            const float dz = pTarget->mZ - pos[i].z;
            const float dist = sqrtf(dx * dx + dz * dz);
            if (dist <= stats.mRange + unit[i].radius + pTarget->mRadius)
                a.inRange = 1;
            else
            {
                vx = dx / dist * stats.mSpeed;
                vz = dz / dist * stats.mSpeed;
            }
            if (dist > 1e-4f)
                unit[i].yaw = atan2f(dx, dz);
        }

        float sx = 0.0f;
        float sz = 0.0f;
        separation(g, it->entities[i], pos[i].x, pos[i].z, unit[i].radius, &sx, &sz);
        vx += sx * SEPARATION_SPEED;
        vz += sz * SEPARATION_SPEED;

        const float r = unit[i].radius;
        pos[i].x = clampf(pos[i].x + vx * dt, r, ARENA_SIZE - r);
        pos[i].z = clampf(pos[i].z + vz * dt, r, ARENA_SIZE - r);
    }
}

// Single-threaded: damages other entities and spawns projectiles.
static void AttackSystem(ecs_iter_t* it)
{
    BattleSim*      p = (BattleSim*)it->ctx;
    const Position* pos = ecs_field(it, Position, 0);
    const Unit*     unit = ecs_field(it, Unit, 1);
    Attack*         attack = ecs_field(it, Attack, 2);

    for (int32_t i = 0; i < it->count; ++i)
    {
        Attack& a = attack[i];
        a.timer -= it->delta_time;
        if (a.timer < 0.0f)
            a.timer = 0.0f;
        if (a.timer > 0.0f || !a.inRange || !a.target || !ecs_is_alive(it->world, a.target))
            continue;

        const UnitStats& stats = gUnitStats[unit[i].type];
        a.timer = stats.mCooldown;
        if (unit[i].type == UNIT_RANGED)
        {
            if (p->mProjectileCount >= MAX_PROJECTILES)
                continue;
            const Projectile   projectile = { a.target, pos[i].x, 1.2f * stats.mScale, pos[i].z, stats.mDamage };
            const ecs_entity_t e = ecs_new(it->world);
            ecs_set(it->world, e, Projectile, projectile);
            ++p->mProjectileCount;
        }
        else
        {
            applyDamage(it->world, a.target, stats.mDamage);
        }
    }
}

static void ProjectileSystem(ecs_iter_t* it)
{
    BattleSim*  p = (BattleSim*)it->ctx;
    Projectile* projectile = ecs_field(it, Projectile, 0);
    const float step = PROJECTILE_SPEED * it->delta_time;

    for (int32_t i = 0; i < it->count; ++i)
    {
        Projectile& pr = projectile[i];
        if (!pr.target || !ecs_is_alive(it->world, pr.target))
        {
            ecs_delete(it->world, it->entities[i]);
            --p->mProjectileCount;
            continue;
        }
        const Position* pTargetPos = ecs_get(it->world, pr.target, Position);
        const float     dx = pTargetPos->x - pr.x;
        const float     dz = pTargetPos->z - pr.z;
        const float     dist = sqrtf(dx * dx + dz * dz);
        if (dist <= step + 0.3f)
        {
            applyDamage(it->world, pr.target, pr.damage);
            ecs_delete(it->world, it->entities[i]);
            --p->mProjectileCount;
            continue;
        }
        pr.x += dx / dist * step;
        pr.z += dz / dist * step;
    }
}

static void HealthSystem(ecs_iter_t* it)
{
    Health* health = ecs_field(it, Health, 0);
    for (int32_t i = 0; i < it->count; ++i)
    {
        health[i].hitFlash = health[i].hitFlash > 0.0f ? health[i].hitFlash - it->delta_time * 5.0f : 0.0f;
        if (health[i].hp <= 0.0f)
            ecs_delete(it->world, it->entities[i]);
    }
}

static void addSystem(BattleSim* p, const char* pName, ecs_entity_t phase, ecs_iter_action_t callback, bool multiThreaded, ecs_id_t c0,
                      ecs_id_t c1 = 0, ecs_id_t c2 = 0)
{
    const ecs_id_t    addIds[3] = { ecs_pair(EcsDependsOn, phase), phase, 0 };
    ecs_entity_desc_t entityDesc = {};
    entityDesc.name = pName;
    entityDesc.add = addIds;

    ecs_system_desc_t desc = {};
    desc.entity = ecs_entity_init(p->pWorld, &entityDesc);
    desc.query.terms[0].id = c0;
    desc.query.terms[1].id = c1;
    desc.query.terms[2].id = c2;
    desc.callback = callback;
    desc.ctx = p;
    desc.multi_threaded = multiThreaded;
    ecs_system_init(p->pWorld, &desc);
}

static void enterResult(BattleSim* p, Phase phase)
{
    p->mPhase = phase;
    p->mResultTime = 0.0f;
    ecs_delete_with(p->pWorld, ecs_id(Projectile));
    p->mProjectileCount = 0;
}

static bool boardHasPlayerSquad(const BattleSim* p)
{
    for (int32_t i = 0; i < ARENA_CELLS * PLAYER_ROWS; ++i)
        if (p->mBoard[i] != UNIT_NONE)
            return true;
    return false;
}
```

In `simCreate`, after the two `ecs_query_init` calls and before `simRestart(p);`, add:

```cpp
    const uint32_t bucketCount = (uint32_t)(GRID_DIM * GRID_DIM);
    p->mGrid.pEntries = (GridEntry*)tf_malloc(sizeof(GridEntry) * MAX_UNITS);
    p->mGrid.pScratch = (GridEntry*)tf_malloc(sizeof(GridEntry) * MAX_UNITS);
    p->mGrid.pBucketStart = (uint32_t*)tf_malloc(sizeof(uint32_t) * (bucketCount + 1));
    p->mGrid.pBucketCursor = (uint32_t*)tf_malloc(sizeof(uint32_t) * bucketCount);

    // Declaration order is execution order within a phase.
    addSystem(p, "SteerSystem", EcsOnUpdate, SteerSystem, true, ecs_id(Position), ecs_id(Unit), ecs_id(Attack));
    addSystem(p, "AttackSystem", EcsOnUpdate, AttackSystem, false, ecs_id(Position), ecs_id(Unit), ecs_id(Attack));
    addSystem(p, "ProjectileSystem", EcsOnUpdate, ProjectileSystem, false, ecs_id(Projectile));
    addSystem(p, "HealthSystem", EcsPostUpdate, HealthSystem, false, ecs_id(Health));
```

In `simDestroy`, before `tf_free(p);`, add:

```cpp
    tf_free(p->mGrid.pEntries);
    tf_free(p->mGrid.pScratch);
    tf_free(p->mGrid.pBucketStart);
    tf_free(p->mGrid.pBucketCursor);
```

Add after `simRestart`:

```cpp
bool simStartBattle(BattleSim* p)
{
    if (p->mPhase != PHASE_PREP)
    {
        p->pMessage = "A battle is already running";
        return false;
    }
    if (!boardHasPlayerSquad(p))
    {
        p->pMessage = "Place at least one squad first";
        return false;
    }
    p->mPhase = PHASE_BATTLE;
    p->mBattleTime = 0.0f;
    p->pMessage = "";
    return true;
}

void simStep(BattleSim* p, float deltaTime)
{
    if (!(deltaTime > 0.0f)) // also rejects NaN
        return;
    const float dt = deltaTime < MAX_STEP ? deltaTime : MAX_STEP;

    switch (p->mPhase)
    {
    case PHASE_BATTLE:
    {
        p->mBattleTime += dt;
        buildGrid(p);
        ecs_progress(p->pWorld, dt);
        const uint32_t players = simGetUnitCount(p, TEAM_PLAYER);
        const uint32_t enemies = simGetUnitCount(p, TEAM_ENEMY);
        if (enemies == 0 && players > 0)
            enterResult(p, PHASE_VICTORY);
        else if (players == 0)
            enterResult(p, PHASE_DEFEAT);
        else if (p->mBattleTime >= BATTLE_TIME_LIMIT)
        {
            enterResult(p, PHASE_DEFEAT);
            p->pMessage = "Time ran out";
        }
        break;
    }
    case PHASE_VICTORY:
        p->mResultTime += dt;
        if (p->mResultTime >= RESULT_DURATION)
        {
            ++p->mRound;
            p->mGold += 5 + p->mRound;
            generateWave(p);
            rebuildFromBoard(p);
            p->mPhase = PHASE_PREP;
        }
        break;
    case PHASE_DEFEAT:
        p->mResultTime += dt;
        break;
    case PHASE_PREP:
        break;
    }
}
```

Add to the queries section:

```cpp
float simGetBattleTime(const BattleSim* p) { return p->mBattleTime; }

uint32_t simGetProjectileCount(const BattleSim* p) { return p->mProjectileCount; }
```

Add to the test hooks section:

```cpp
void simTestSpawnUnit(BattleSim* p, Team team, UnitType type, float x, float z) { spawnUnit(p, team, type, x, z, 0xFFFF); }

void simTestSetBattleTime(BattleSim* p, float seconds) { p->mBattleTime = seconds; }

float simTestTotalHp(BattleSim* p, Team team)
{
    float      total = 0.0f;
    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pUnitQuery);
    while (ecs_query_next(&it))
    {
        const Unit*   unit = ecs_field(&it, Unit, 1);
        const Health* health = ecs_field(&it, Health, 2);
        for (int32_t i = 0; i < it.count; ++i)
            if (unit[i].team == team)
                total += health[i].hp;
    }
    return total;
}

bool simTestAllPositionsFinite(BattleSim* p)
{
    bool       finite = true;
    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pUnitQuery);
    while (ecs_query_next(&it))
    {
        const Position* pos = ecs_field(&it, Position, 0);
        for (int32_t i = 0; i < it.count; ++i)
            finite = finite && isfinite(pos[i].x) && isfinite(pos[i].z);
    }
    return finite;
}

float simTestMinUnitDistance(BattleSim* p, Team team)
{
    Position*  points = (Position*)tf_malloc(sizeof(Position) * MAX_UNITS);
    uint32_t   count = 0;
    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pUnitQuery);
    while (ecs_query_next(&it))
    {
        const Position* pos = ecs_field(&it, Position, 0);
        const Unit*     unit = ecs_field(&it, Unit, 1);
        for (int32_t i = 0; i < it.count; ++i)
            if (unit[i].team == team)
                points[count++] = pos[i];
    }
    float minD2 = FLT_MAX;
    for (uint32_t a = 0; a < count; ++a)
        for (uint32_t b = a + 1; b < count; ++b)
        {
            const float dx = points[a].x - points[b].x;
            const float dz = points[a].z - points[b].z;
            if (dx * dx + dz * dz < minD2)
                minD2 = dx * dx + dz * dz;
        }
    tf_free(points);
    return sqrtf(minD2);
}
```

- [ ] **Step 5: Build and run the self test**

Run the shared **Build** (Debug) and **Self test** commands.
Expected: `exit=0`, `RESULT: 0 failure(s)`. If `testThreadCountDoesNotChangeOutcome` fails, a system is reading another entity's live component from a worker thread. Fix the data flow so `SteerSystem` reads others only through `SpatialGrid`; do not loosen the test.

- [ ] **Step 6: Commit**

```bash
git add Examples_3/Unit_Tests/src/40_AutoBattler
git commit -m "Add 40_AutoBattler battle systems on Flecs with spatial grid

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Render data from the simulation

**Files:**
- Modify: `Examples_3/Unit_Tests/src/40_AutoBattler/AutoBattlerSim.h`
- Modify: `Examples_3/Unit_Tests/src/40_AutoBattler/AutoBattlerSim.cpp`
- Modify: `Examples_3/Unit_Tests/src/40_AutoBattler/AutoBattlerSelfTest.cpp`

**Interfaces:**
- Consumes (Tasks 1–2): `pUnitQuery`, `pProjectileQuery`, components.
- Produces (Task 4 uploads these to the GPU, byte-for-byte matching the FSL structs): `UnitInstance`, `BillboardInstance`, `RenderData`, `MAX_BILLBOARDS`, `void simFillRenderData(BattleSim*, RenderData*)`.

- [ ] **Step 1: Declare the render data types**

Add to `AutoBattlerSim.h` after `PROJECTILE_SPEED`:

```cpp
static const uint32_t MAX_BILLBOARDS = MAX_UNITS + MAX_PROJECTILES;
```

Add after `gUnitStats`:

```cpp
// GPU instance layouts. Must match UnitInstance/BillboardInstance in Shaders/FSL/AutoBattler.h.fsl.
struct UnitInstance
{
    mat4   mWorld;
    float4 mColor; // rgb team colour, a = hit flash [0,1]
};

struct BillboardInstance
{
    float4 mCenterHalfWidth;      // xyz world centre, w half width
    float4 mFullColorFill;        // rgb colour left of the fill line, a = fill fraction [0,1]
    float4 mEmptyColorHalfHeight; // rgb colour right of the fill line, a = half height
};

struct RenderData
{
    UnitInstance*      pUnits;      // capacity MAX_UNITS
    BillboardInstance* pBillboards; // capacity MAX_BILLBOARDS
    uint32_t           mUnitCount;
    uint32_t           mBillboardCount;
};
```

Add after `simGetMessage`:

```cpp
// Writes one UnitInstance per unit, a health bar per damaged unit and a billboard per projectile.
void simFillRenderData(BattleSim* pSim, RenderData* pOut);
```

- [ ] **Step 2: Write the failing tests**

Add to `AutoBattlerSelfTest.cpp` above `typedef void (*TestFn)`:

```cpp
static RenderData allocRenderData()
{
    RenderData data = {};
    data.pUnits = (UnitInstance*)tf_malloc(sizeof(UnitInstance) * MAX_UNITS);
    data.pBillboards = (BillboardInstance*)tf_malloc(sizeof(BillboardInstance) * MAX_BILLBOARDS);
    return data;
}

static void freeRenderData(RenderData* pData)
{
    tf_free(pData->pUnits);
    tf_free(pData->pBillboards);
}

static void testRenderDataPrep(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 0, 0, UNIT_MELEE);
    simTestSetCell(s, 15, 15, UNIT_TANK);
    RenderData data = allocRenderData();
    simFillRenderData(s, &data);
    CHECK(data.mUnitCount == 25u + 9u);
    CHECK(data.mBillboardCount == 0u); // full health, no projectiles

    uint32_t blue = 0;
    bool     onGround = true;
    for (uint32_t i = 0; i < data.mUnitCount; ++i)
    {
        const Vector4 t = data.pUnits[i].mWorld.getCol3();
        onGround = onGround && t.getY() == 0.0f && t.getX() > 0.0f && t.getX() < ARENA_SIZE && t.getZ() > 0.0f && t.getZ() < ARENA_SIZE;
        if (data.pUnits[i].mColor.z > data.pUnits[i].mColor.x)
            ++blue;
        CHECK(data.pUnits[i].mColor.w == 0.0f);
    }
    CHECK(onGround);
    CHECK(blue == 25u);
    freeRenderData(&data);
    simDestroy(s);
}

static void testRenderDataBattle(TestContext* pCtx)
{
    BattleSim* s = simCreate(1);
    simTestClearBoard(s);
    simTestSetCell(s, 8, 7, UNIT_RANGED);
    simTestSetCell(s, 8, 8, UNIT_TANK);
    CHECK(simStartBattle(s));
    stepFor(s, 0.75f); // first volley has landed, so at least one tank is damaged
    RenderData data = allocRenderData();
    simFillRenderData(s, &data);
    CHECK(data.mUnitCount == simGetUnitCount(s, TEAM_PLAYER) + simGetUnitCount(s, TEAM_ENEMY));
    CHECK(data.mUnitCount <= MAX_UNITS);
    CHECK(data.mBillboardCount >= simGetProjectileCount(s) + 1u); // projectiles + at least one damaged tank
    CHECK(data.mBillboardCount <= MAX_BILLBOARDS);
    bool fillInRange = true;
    for (uint32_t i = 0; i < data.mBillboardCount; ++i)
        fillInRange = fillInRange && data.pBillboards[i].mFullColorFill.w >= 0.0f && data.pBillboards[i].mFullColorFill.w <= 1.0f;
    CHECK(fillInRange);
    freeRenderData(&data);
    simDestroy(s);
}
```

Append to `tests[]`:

```cpp
        { "render data in prep", testRenderDataPrep },
        { "render data in battle", testRenderDataBattle },
```

- [ ] **Step 3: Build to verify the tests fail**

Run the shared **Build** command (Debug). Expected: FAIL with unresolved external `simFillRenderData`.

- [ ] **Step 4: Implement `simFillRenderData`**

Add to `AutoBattlerSim.cpp` after the queries section:

```cpp
//--------------------------------------------------------------------------------------------
// Render data
//--------------------------------------------------------------------------------------------
static const float3 TEAM_COLORS[TEAM_COUNT] = { float3(0.30f, 0.55f, 1.00f), float3(1.00f, 0.30f, 0.25f) };
static const float  HEALTH_BAR_HEIGHT = 2.2f;

void simFillRenderData(BattleSim* p, RenderData* pOut)
{
    pOut->mUnitCount = 0;
    pOut->mBillboardCount = 0;

    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pUnitQuery);
    while (ecs_query_next(&it))
    {
        const Position* pos = ecs_field(&it, Position, 0);
        const Unit*     unit = ecs_field(&it, Unit, 1);
        const Health*   health = ecs_field(&it, Health, 2);
        for (int32_t i = 0; i < it.count; ++i)
        {
            ASSERT(pOut->mUnitCount < MAX_UNITS);
            const float   scale = gUnitStats[unit[i].type].mScale;
            const float3& color = TEAM_COLORS[unit[i].team];
            UnitInstance& instance = pOut->pUnits[pOut->mUnitCount++];
            instance.mWorld = mat4::translation(vec3(pos[i].x, 0.0f, pos[i].z)) * mat4::rotationY(unit[i].yaw) * mat4::scale(vec3(scale));
            instance.mColor = float4(color.x, color.y, color.z, health[i].hitFlash > 0.0f ? health[i].hitFlash : 0.0f);

            if (health[i].hp < health[i].maxHp)
            {
                ASSERT(pOut->mBillboardCount < MAX_BILLBOARDS);
                const float        fill = health[i].hp > 0.0f ? health[i].hp / health[i].maxHp : 0.0f;
                BillboardInstance& bar = pOut->pBillboards[pOut->mBillboardCount++];
                bar.mCenterHalfWidth = float4(pos[i].x, HEALTH_BAR_HEIGHT * scale, pos[i].z, 0.4f);
                bar.mFullColorFill = float4(0.2f, 0.9f, 0.25f, fill);
                bar.mEmptyColorHalfHeight = float4(0.55f, 0.08f, 0.08f, 0.06f);
            }
        }
    }

    it = ecs_query_iter(p->pWorld, p->pProjectileQuery);
    while (ecs_query_next(&it))
    {
        const Projectile* projectile = ecs_field(&it, Projectile, 0);
        for (int32_t i = 0; i < it.count; ++i)
        {
            ASSERT(pOut->mBillboardCount < MAX_BILLBOARDS);
            BillboardInstance& b = pOut->pBillboards[pOut->mBillboardCount++];
            b.mCenterHalfWidth = float4(projectile[i].x, projectile[i].y, projectile[i].z, 0.12f);
            b.mFullColorFill = float4(1.0f, 0.85f, 0.2f, 1.0f);
            b.mEmptyColorHalfHeight = float4(1.0f, 0.85f, 0.2f, 0.12f);
        }
    }
}
```

- [ ] **Step 5: Build and run the self test**

Expected: `exit=0`, `RESULT: 0 failure(s)`.

- [ ] **Step 6: Commit**

```bash
git add Examples_3/Unit_Tests/src/40_AutoBattler
git commit -m "Produce 40_AutoBattler instance data from the simulation

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Renderer — ground, instanced Stormtroopers, billboards, victory dance, HUD, smoke mode

**Files:**
- Create: `Examples_3/Unit_Tests/src/40_AutoBattler/Shaders/Shared.h`
- Create: `Examples_3/Unit_Tests/src/40_AutoBattler/Shaders/FSL/AutoBattler.h.fsl`
- Create: `Examples_3/Unit_Tests/src/40_AutoBattler/Shaders/FSL/Global.srt.h`
- Create: `.../Shaders/FSL/Unit.vert.fsl`, `Unit.frag.fsl`, `Ground.vert.fsl`, `Ground.frag.fsl`, `Billboard.vert.fsl`, `Billboard.frag.fsl`
- Modify: `Examples_3/Unit_Tests/src/40_AutoBattler/Shaders/FSL/Shaders.list`
- Replace: `Examples_3/Unit_Tests/src/40_AutoBattler/40_AutoBattler.cpp`
- Modify: `Examples_3/Unit_Tests/PC_VS2019/40_AutoBattler.vcxproj`

**Interfaces:**
- Consumes: `simCreate/simDestroy/simStep/simGetPhase/simGetRound/simGetGold/simGetUnitCount/simGetBattleTime/simGetMessage/simFillRenderData`, `RenderData`, `UnitInstance`, `BillboardInstance`; test hooks `simTestClearBoard/simTestSetGold/simTestSetCell` and `simBuySquad`/`simStartBattle` for `--smoke`.
- Produces (Task 5 extends): globals `gFrameUniforms` (`mHoverCell` is written by Task 5), `gCameraTarget`, `gCameraDistance`, `gCameraEye/gCameraForward/gCameraRight/gCameraUp`, `CAMERA_FOV_X`; methods `updateCamera(float)`, `addGui()` hook points in `Load`/`Unload`.

- [ ] **Step 1: Write the shaders**

`Shaders/Shared.h`:

```cpp
#pragma once

#define MAX_NUM_BONES 128
```

`Shaders/FSL/AutoBattler.h.fsl`:

```
#ifndef AUTOBATTLER_H
#define AUTOBATTLER_H

#include "../Shared.h"

// Layouts must match FrameUniforms in 40_AutoBattler.cpp and UnitInstance/BillboardInstance in AutoBattlerSim.h.
STRUCT(FrameUniforms)
{
    DATA(float4x4, viewProj, None);
    DATA(float4, camRight, None);  // xyz camera right
    DATA(float4, camUp, None);     // xyz camera up
    DATA(float4, hoverCell, None); // x, z = cell; y = 0 none, 1 can place, 2 cannot; w = cell size
    DATA(float4, arena, None);     // x = arena size, y = world z where the enemy half starts
};

STRUCT(BoneMatrices)
{
    DATA(float4x4, boneMatrix[MAX_NUM_BONES], None);
};

STRUCT(UnitInstance)
{
    DATA(float4x4, world, None);
    DATA(float4, color, None);
};

STRUCT(BillboardInstance)
{
    DATA(float4, centerHalfWidth, None);
    DATA(float4, fullColorFill, None);
    DATA(float4, emptyColorHalfHeight, None);
};

#include "Global.srt.h"

#endif
```

`Shaders/FSL/Global.srt.h`:

```
#pragma once

// for low end iOS devices, do not use Argument buffers
BEGIN_SRT_NO_AB(SrtData)
    BEGIN_SRT_SET(Persistent)
        DECL_TEXTURE(Persistent, Tex2D(float4), gUnitTexture)
        DECL_TEXTURE(Persistent, Tex2D(float4), gGroundTexture)
    END_SRT_SET(Persistent)
    BEGIN_SRT_SET(PerFrame)
        DECL_CBUFFER(PerFrame, CBUFFER(FrameUniforms), gFrame)
        DECL_CBUFFER(PerFrame, CBUFFER(BoneMatrices), gBoneMatrices)
        DECL_BUFFER(PerFrame, Buffer(UnitInstance), gUnitInstances)
        DECL_BUFFER(PerFrame, Buffer(BillboardInstance), gBillboards)
    END_SRT_SET(PerFrame)
END_SRT(SrtData)
```

`Shaders/FSL/Unit.vert.fsl`:

```
#include "../../../../../../Common_3/Graphics/ShaderUtilities.h.fsl"
#include "AutoBattler.h.fsl"

STRUCT(VSInput)
{
    DATA(float3, Position, POSITION);
    DATA(uint, Normal, NORMAL);
    DATA(uint, UV, TEXCOORD0);
    DATA(float4, BoneWeights, WEIGHTS);
    DATA(uint4, BoneIndices, JOINTS);
};

STRUCT(VSOutput)
{
    DATA(float4, Position, SV_Position);
    DATA(float3, Normal, NORMAL);
    DATA(float2, UV, TEXCOORD0);
    DATA(float4, Color, COLOR0);
};

ROOT_SIGNATURE(DefaultRootSignature)
VSOutput VS_MAIN(VSInput In, SV_InstanceID(uint) instanceId)
{
    INIT_MAIN;
    VSOutput Out;

    float4x4 boneTransform = gBoneMatrices.boneMatrix[In.BoneIndices[0]] * In.BoneWeights[0];
    boneTransform += gBoneMatrices.boneMatrix[In.BoneIndices[1]] * In.BoneWeights[1];
    boneTransform += gBoneMatrices.boneMatrix[In.BoneIndices[2]] * In.BoneWeights[2];
    boneTransform += gBoneMatrices.boneMatrix[In.BoneIndices[3]] * In.BoneWeights[3];

    float4x4 world = gUnitInstances[instanceId].world;
    float4   worldPos = mul(world, mul(boneTransform, float4(In.Position, 1.0f)));
    Out.Position = mul(gFrame.viewProj, worldPos);
    float3 normal = decodeDir(unpackUnorm2x16(In.Normal));
    Out.Normal = normalize(mul(world, mul(boneTransform, float4(normal, 0.0f))).xyz);
    Out.UV = unpack2Floats(In.UV);
    Out.Color = gUnitInstances[instanceId].color;
    RETURN(Out);
}
```

`Shaders/FSL/Unit.frag.fsl`:

```
#include "AutoBattler.h.fsl"

STRUCT(VSOutput)
{
    DATA(float4, Position, SV_Position);
    DATA(float3, Normal, NORMAL);
    DATA(float2, UV, TEXCOORD0);
    DATA(float4, Color, COLOR0);
};

ROOT_SIGNATURE(DefaultRootSignature)
float4 PS_MAIN(VSOutput In)
{
    INIT_MAIN;
    float4 Out;
    float  nDotL = saturate((dot(normalize(In.Normal), normalize(float3(0.3f, 1.0f, -0.4f))) + 1.0f) * 0.5f);
    float3 albedo = SampleTex2D(gUnitTexture, gSamplerTrilinearWrap, In.UV).rgb;
    float  luminance = dot(albedo, float3(0.333f, 0.333f, 0.333f));
    // Tint the white armour with the team colour, then flash towards white when hit.
    float3 color = lerp(albedo, In.Color.rgb * luminance * 1.6f, 0.65f);
    color = lerp(color, float3(1.0f, 1.0f, 1.0f), In.Color.a * 0.8f);
    Out = float4(color * (0.35f + 0.65f * nDotL), 1.0f);
    RETURN(Out);
}
```

`Shaders/FSL/Ground.vert.fsl`:

```
#include "AutoBattler.h.fsl"

STRUCT(VSOutput)
{
    DATA(float4, Position, SV_Position);
    DATA(float2, WorldXZ, TEXCOORD0);
};

ROOT_SIGNATURE(DefaultRootSignature)
VSOutput VS_MAIN(SV_VertexID(uint) vertexId)
{
    INIT_MAIN;
    VSOutput Out;
    // Two triangles over the arena: (0,0) (1,0) (1,1) and (0,0) (1,1) (0,1).
    float2 corner = float2((vertexId == 1 || vertexId == 2 || vertexId == 4) ? 1.0f : 0.0f,
                           (vertexId == 2 || vertexId == 4 || vertexId == 5) ? 1.0f : 0.0f);
    Out.WorldXZ = corner * gFrame.arena.x;
    Out.Position = mul(gFrame.viewProj, float4(Out.WorldXZ.x, 0.0f, Out.WorldXZ.y, 1.0f));
    RETURN(Out);
}
```

`Shaders/FSL/Ground.frag.fsl`:

```
#include "AutoBattler.h.fsl"

STRUCT(VSOutput)
{
    DATA(float4, Position, SV_Position);
    DATA(float2, WorldXZ, TEXCOORD0);
};

ROOT_SIGNATURE(DefaultRootSignature)
float4 PS_MAIN(VSOutput In)
{
    INIT_MAIN;
    float4 Out;
    float  cellSize = gFrame.hoverCell.w;
    float2 cellUV = In.WorldXZ / cellSize;
    float3 albedo = SampleTex2D(gGroundTexture, gSamplerTrilinearWrap, cellUV * 0.5f).rgb;

    // Cool tint on the player half, warm and darker on the enemy half.
    albedo *= In.WorldXZ.y >= gFrame.arena.y ? float3(0.75f, 0.55f, 0.55f) : float3(0.7f, 0.8f, 0.95f);

    float2 f = frac(cellUV);
    float2 edge = min(f, 1.0f - f);
    if (min(edge.x, edge.y) < 0.02f)
        albedo *= 0.45f;

    float2 cell = floor(cellUV);
    if (gFrame.hoverCell.y > 0.5f && cell.x == gFrame.hoverCell.x && cell.y == gFrame.hoverCell.z)
        albedo = lerp(albedo, gFrame.hoverCell.y < 1.5f ? float3(0.2f, 0.9f, 0.3f) : float3(0.9f, 0.2f, 0.2f), 0.45f);

    Out = float4(albedo, 1.0f);
    RETURN(Out);
}
```

`Shaders/FSL/Billboard.vert.fsl` (6 vertices per billboard, no instancing: `cmdDraw(6 * count)`):

```
#include "AutoBattler.h.fsl"

STRUCT(VSOutput)
{
    DATA(float4, Position, SV_Position);
    DATA(float2, UV, TEXCOORD0);
    DATA(float4, FullColorFill, COLOR0);
    DATA(float3, EmptyColor, COLOR1);
};

ROOT_SIGNATURE(DefaultRootSignature)
VSOutput VS_MAIN(SV_VertexID(uint) vertexId)
{
    INIT_MAIN;
    VSOutput Out;
    uint   index = vertexId / 6;
    uint   v = vertexId % 6;
    float2 corner = float2((v == 1 || v == 2 || v == 4) ? 1.0f : 0.0f, (v == 2 || v == 4 || v == 5) ? 1.0f : 0.0f);

    float4 centerHalfWidth = gBillboards[index].centerHalfWidth;
    float4 emptyColorHalfHeight = gBillboards[index].emptyColorHalfHeight;
    float2 offset = (corner * 2.0f - 1.0f) * float2(centerHalfWidth.w, emptyColorHalfHeight.w);
    float3 worldPos = centerHalfWidth.xyz + gFrame.camRight.xyz * offset.x + gFrame.camUp.xyz * offset.y;

    Out.Position = mul(gFrame.viewProj, float4(worldPos, 1.0f));
    Out.UV = corner;
    Out.FullColorFill = gBillboards[index].fullColorFill;
    Out.EmptyColor = emptyColorHalfHeight.rgb;
    RETURN(Out);
}
```

`Shaders/FSL/Billboard.frag.fsl`:

```
#include "AutoBattler.h.fsl"

STRUCT(VSOutput)
{
    DATA(float4, Position, SV_Position);
    DATA(float2, UV, TEXCOORD0);
    DATA(float4, FullColorFill, COLOR0);
    DATA(float3, EmptyColor, COLOR1);
};

ROOT_SIGNATURE(DefaultRootSignature)
float4 PS_MAIN(VSOutput In)
{
    INIT_MAIN;
    float4 Out;
    float3 color = In.UV.x <= In.FullColorFill.a ? In.FullColorFill.rgb : In.EmptyColor;
    Out = float4(color, 1.0f);
    RETURN(Out);
}
```

Append to `Shaders/FSL/Shaders.list`:

```

#vert unit.vert
#include "Unit.vert.fsl"
#end

#frag unit.frag
#include "Unit.frag.fsl"
#end

#vert ground.vert
#include "Ground.vert.fsl"
#end

#frag ground.frag
#include "Ground.frag.fsl"
#end

#vert billboard.vert
#include "Billboard.vert.fsl"
#end

#frag billboard.frag
#include "Billboard.frag.fsl"
#end
```

- [ ] **Step 2: Register the shaders in the project**

In `40_AutoBattler.vcxproj`, replace the `ClInclude` item group with:

```xml
  <ItemGroup>
    <ClInclude Include="..\src\40_AutoBattler\AutoBattlerSim.h" />
    <ClInclude Include="..\src\40_AutoBattler\Shaders\Shared.h" />
    <ClInclude Include="..\src\40_AutoBattler\Shaders\FSL\Global.srt.h" />
    <ClInclude Include="..\..\..\Common_3\Game\ThirdParty\OpenSource\flecs\flecs.h" />
  </ItemGroup>
```

and the `FSLShader` item group with:

```xml
  <ItemGroup>
    <FSLShader Include="..\src\40_AutoBattler\Shaders\FSL\AutoBattler.h.fsl" />
    <FSLShader Include="..\src\40_AutoBattler\Shaders\FSL\Billboard.frag.fsl" />
    <FSLShader Include="..\src\40_AutoBattler\Shaders\FSL\Billboard.vert.fsl" />
    <FSLShader Include="..\src\40_AutoBattler\Shaders\FSL\Ground.frag.fsl" />
    <FSLShader Include="..\src\40_AutoBattler\Shaders\FSL\Ground.vert.fsl" />
    <FSLShader Include="..\src\40_AutoBattler\Shaders\FSL\Shaders.list" />
    <FSLShader Include="..\src\40_AutoBattler\Shaders\FSL\Unit.frag.fsl" />
    <FSLShader Include="..\src\40_AutoBattler\Shaders\FSL\Unit.vert.fsl" />
  </ItemGroup>
```

- [ ] **Step 3: Write the app**

Replace `Examples_3/Unit_Tests/src/40_AutoBattler/40_AutoBattler.cpp` with:

```cpp
// 40_AutoBattler: top-down auto battler. Gameplay lives in AutoBattlerSim (Flecs); this file owns
// rendering, camera, input and UI.

#include "Shaders/Shared.h"
#include "AutoBattlerSim.h"

#include <stdlib.h>
#include <string.h>

// Interfaces
#include "../../../../Common_3/Application/Interfaces/IApp.h"
#include "../../../../Common_3/Application/Interfaces/ICameraController.h"
#include "../../../../Common_3/Application/Interfaces/IFont.h"
#include "../../../../Common_3/Application/Interfaces/IProfiler.h"
#include "../../../../Common_3/Application/Interfaces/IUI.h"
#include "../../../../Common_3/OS/Interfaces/IInput.h"
#include "../../../../Common_3/Utilities/Interfaces/IFileSystem.h"
#include "../../../../Common_3/Utilities/Interfaces/ILog.h"
#include "../../../../Common_3/Utilities/Interfaces/IThread.h"

// Rendering
#include "../../../../Common_3/Graphics/Interfaces/IGraphics.h"
#include "../../../../Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"
#include "../../../../Common_3/Utilities/RingBuffer.h"

// Animation
#include "../../../../Common_3/Resources/AnimationSystem/Animation/AnimatedObject.h"
#include "../../../../Common_3/Resources/AnimationSystem/Animation/Animation.h"
#include "../../../../Common_3/Resources/AnimationSystem/Animation/Clip.h"
#include "../../../../Common_3/Resources/AnimationSystem/Animation/ClipController.h"
#include "../../../../Common_3/Resources/AnimationSystem/Animation/Rig.h"

#include "../../../../Common_3/Utilities/Math/MathTypes.h"

// fsl
#include "../../../../Common_3/Graphics/FSL/defaults.h"
#include "./Shaders/FSL/Global.srt.h"

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

const uint32_t gDataBufferCount = 2;

Renderer*     pRenderer = NULL;
Queue*        pGraphicsQueue = NULL;
GpuCmdRing    gGraphicsCmdRing = {};
SwapChain*    pSwapChain = NULL;
RenderTarget* pDepthBuffer = NULL;
Semaphore*    pImageAcquiredSemaphore = NULL;
uint32_t      gFrameIndex = 0;
ProfileToken  gGpuProfileToken;

Shader*        pUnitShader = NULL;
Shader*        pGroundShader = NULL;
Shader*        pBillboardShader = NULL;
Pipeline*      pUnitPipeline = NULL;
Pipeline*      pGroundPipeline = NULL;
Pipeline*      pBillboardPipeline = NULL;
DescriptorSet* pSetPersistent = NULL;
DescriptorSet* pSetPerFrame = NULL;

VertexLayout  gUnitVertexLayout = {};
Geometry*     pUnitGeom = NULL;
GeometryData* pUnitGeomData = NULL;
Texture*      pUnitTexture = NULL;
Texture*      pGroundTexture = NULL;

Buffer* pFrameUniformBuffers[gDataBufferCount] = {};
Buffer* pBoneBuffers[gDataBufferCount] = {};
Buffer* pUnitInstanceBuffers[gDataBufferCount] = {};
Buffer* pBillboardBuffers[gDataBufferCount] = {};

// Must match FrameUniforms in Shaders/FSL/AutoBattler.h.fsl.
struct FrameUniforms
{
    mat4   mViewProj;
    float4 mCamRight;
    float4 mCamUp;
    float4 mHoverCell; // x, z = cell; y = 0 none, 1 can place, 2 cannot; w = cell size
    float4 mArena;     // x = arena size, y = world z where the enemy half starts
};
FrameUniforms gFrameUniforms = {};
mat4          gBoneMatrices[MAX_NUM_BONES];
mat4          gBindPoseBones[MAX_NUM_BONES];

// Victory dance: one animated object drives the shared bone palette of every unit.
Rig            gRig;
Clip           gDanceClip;
ClipController gDanceController;
Animation      gDanceAnimation;
AnimatedObject gDanceObject;
float          gDanceTime = 0.0f;

BattleSim* pSim = NULL;
RenderData gRenderData = {};
Phase      gLastPhase = PHASE_PREP;

// Camera orbits a ground target at a fixed pitch.
const float CAMERA_PITCH = 55.0f * PI / 180.0f;
const float CAMERA_FOV_X = PI / 3.0f;
float2      gCameraTarget = float2(ARENA_SIZE * 0.5f, ARENA_SIZE * 0.45f);
float       gCameraDistance = 75.0f;
vec3        gCameraEye;
vec3        gCameraForward;
vec3        gCameraRight;
vec3        gCameraUp;

uint32_t     gFontID = 0;
FontDrawDesc gTextDraw;
char         gHudLines[3][256] = {};

// --smoke: plays a large battle unattended, writes a summary file and quits.
bool     gSmokeMode = false;
bool     gSmokeDone = false;
uint32_t gSmokeFrames = 0;
uint32_t gSmokeResultFrame = 0;
uint32_t gSmokeStartUnits = 0;
float    gSmokeSeconds = 0.0f;
Phase    gSmokeResult = PHASE_PREP;

static const char* PHASE_NAMES[] = { "Prep", "Battle", "Victory", "Defeat" };

static void writeTextFile(const char* pFileName, const char* pText)
{
    FileStream fs = {};
    if (!fsOpenStreamFromPath(RD_LOG, pFileName, FM_WRITE, &fs))
        return;
    fsWriteToStream(&fs, pText, strlen(pText));
    fsCloseStream(&fs);
}

class AutoBattler: public IApp
{
public:
    bool Init()
    {
        for (int i = 1; i < argc; ++i)
        {
            if (strcmp(argv[i], "--selftest") == 0)
                exit(runAutoBattlerSelfTest("40_AutoBattler_selftest.txt") == 0 ? 0 : 1);
            if (strcmp(argv[i], "--smoke") == 0)
                gSmokeMode = true;
        }

        // Animation setup, as in 28_Skinning.
        gRig.Initialize(RD_ANIMATIONS, "stormtrooper/skeleton.ozz");
        gDanceClip.Initialize(RD_ANIMATIONS, "stormtrooper/animations/dance.ozz", &gRig);
        gDanceController.Initialize(gDanceClip.GetDuration(), &gDanceTime);
        AnimationDesc animationDesc{};
        animationDesc.mRig = &gRig;
        animationDesc.mNumLayers = 1;
        animationDesc.mLayerProperties[0].mClip = &gDanceClip;
        animationDesc.mLayerProperties[0].mClipController = &gDanceController;
        gDanceAnimation.Initialize(animationDesc);
        gDanceObject.Initialize(&gRig, &gDanceAnimation);

        RendererDesc settings;
        memset(&settings, 0, sizeof(settings));
        initGPUConfiguration(settings.pExtendedSettings);
        initRenderer(GetName(), &settings, &pRenderer);
        if (!pRenderer)
        {
            ShowUnsupportedMessage(getUnsupportedGPUMsg());
            return false;
        }
        setupGPUConfigurationPlatformParameters(pRenderer, settings.pExtendedSettings);

        QueueDesc queueDesc = {};
        queueDesc.mType = QUEUE_TYPE_GRAPHICS;
        queueDesc.mFlag = QUEUE_FLAG_INIT_MICROPROFILE;
        initQueue(pRenderer, &queueDesc, &pGraphicsQueue);

        GpuCmdRingDesc cmdRingDesc = {};
        cmdRingDesc.pQueue = pGraphicsQueue;
        cmdRingDesc.mPoolCount = gDataBufferCount;
        cmdRingDesc.mCmdPerPoolCount = 1;
        cmdRingDesc.mAddSyncPrimitives = true;
        initGpuCmdRing(pRenderer, &cmdRingDesc, &gGraphicsCmdRing);

        initSemaphore(pRenderer, &pImageAcquiredSemaphore);
        initResourceLoaderInterface(pRenderer);

        RootSignatureDesc rootDesc = {};
        INIT_RS_DESC(rootDesc, "default.rootsig", "compute.rootsig");
        initRootSignature(pRenderer, &rootDesc);

        FontDesc font = {};
        font.pFontPath = "TitilliumText/TitilliumText-Bold.otf";
        fntDefineFonts(&font, 1, &gFontID);
        FontSystemDesc fontRenderDesc = {};
        fontRenderDesc.pRenderer = pRenderer;
        if (!initFontSystem(&fontRenderDesc))
            return false;

        UserInterfaceDesc uiRenderDesc = {};
        uiRenderDesc.pRenderer = pRenderer;
        initUserInterface(&uiRenderDesc);

        ProfilerDesc profiler = {};
        profiler.pRenderer = pRenderer;
        initProfiler(&profiler);
        gGpuProfileToken = initGpuProfiler(pRenderer, pGraphicsQueue, "Graphics");

        // Per-frame buffers.
        BufferLoadDesc ubDesc = {};
        ubDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
        ubDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
        for (uint32_t i = 0; i < gDataBufferCount; ++i)
        {
            ubDesc.mDesc.mSize = sizeof(FrameUniforms);
            ubDesc.ppBuffer = &pFrameUniformBuffers[i];
            addResource(&ubDesc, NULL);
            ubDesc.mDesc.mSize = sizeof(mat4) * MAX_NUM_BONES;
            ubDesc.ppBuffer = &pBoneBuffers[i];
            addResource(&ubDesc, NULL);
        }

        BufferLoadDesc instanceDesc = {};
        instanceDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER;
        instanceDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
        instanceDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_NONE;
        instanceDesc.mDesc.mStartState = RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        instanceDesc.mDesc.mFirstElement = 0;
        for (uint32_t i = 0; i < gDataBufferCount; ++i)
        {
            instanceDesc.mDesc.mElementCount = MAX_UNITS;
            instanceDesc.mDesc.mStructStride = sizeof(UnitInstance);
            instanceDesc.mDesc.mSize = (uint64_t)MAX_UNITS * sizeof(UnitInstance);
            instanceDesc.ppBuffer = &pUnitInstanceBuffers[i];
            addResource(&instanceDesc, NULL);
            instanceDesc.mDesc.mElementCount = MAX_BILLBOARDS;
            instanceDesc.mDesc.mStructStride = sizeof(BillboardInstance);
            instanceDesc.mDesc.mSize = (uint64_t)MAX_BILLBOARDS * sizeof(BillboardInstance);
            instanceDesc.ppBuffer = &pBillboardBuffers[i];
            addResource(&instanceDesc, NULL);
        }

        // Skinned Stormtrooper, same vertex layout as 28_Skinning.
        gUnitVertexLayout.mBindingCount = 1;
        gUnitVertexLayout.mAttribCount = 5;
        gUnitVertexLayout.mAttribs[0].mSemantic = SEMANTIC_POSITION;
        gUnitVertexLayout.mAttribs[0].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
        gUnitVertexLayout.mAttribs[0].mBinding = 0;
        gUnitVertexLayout.mAttribs[0].mLocation = 0;
        gUnitVertexLayout.mAttribs[0].mOffset = 0;
        gUnitVertexLayout.mAttribs[1].mSemantic = SEMANTIC_NORMAL;
        gUnitVertexLayout.mAttribs[1].mFormat = TinyImageFormat_R32_UINT;
        gUnitVertexLayout.mAttribs[1].mBinding = 0;
        gUnitVertexLayout.mAttribs[1].mLocation = 1;
        gUnitVertexLayout.mAttribs[1].mOffset = 3 * sizeof(float);
        gUnitVertexLayout.mAttribs[2].mSemantic = SEMANTIC_TEXCOORD0;
        gUnitVertexLayout.mAttribs[2].mFormat = TinyImageFormat_R32_UINT;
        gUnitVertexLayout.mAttribs[2].mBinding = 0;
        gUnitVertexLayout.mAttribs[2].mLocation = 2;
        gUnitVertexLayout.mAttribs[2].mOffset = 3 * sizeof(float) + sizeof(uint32_t);
        gUnitVertexLayout.mAttribs[3].mSemantic = SEMANTIC_WEIGHTS;
        gUnitVertexLayout.mAttribs[3].mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
        gUnitVertexLayout.mAttribs[3].mBinding = 0;
        gUnitVertexLayout.mAttribs[3].mLocation = 3;
        gUnitVertexLayout.mAttribs[3].mOffset = 3 * sizeof(float) + sizeof(uint32_t) * 2;
        gUnitVertexLayout.mAttribs[4].mSemantic = SEMANTIC_JOINTS;
        gUnitVertexLayout.mAttribs[4].mFormat = TinyImageFormat_R16G16B16A16_UINT;
        gUnitVertexLayout.mAttribs[4].mBinding = 0;
        gUnitVertexLayout.mAttribs[4].mLocation = 4;
        gUnitVertexLayout.mAttribs[4].mOffset = 7 * sizeof(float) + sizeof(uint32_t) * 2;

        GeometryLoadDesc geomDesc = {};
        geomDesc.pFileName = "stormtrooper/riggedMesh.bin";
        geomDesc.pVertexLayout = &gUnitVertexLayout;
        geomDesc.ppGeometry = &pUnitGeom;
        geomDesc.ppGeometryData = &pUnitGeomData;
        geomDesc.mFlags = GEOMETRY_LOAD_FLAG_SHADOWED;
        addResource(&geomDesc, NULL);

        TextureLoadDesc textureDesc = {};
        textureDesc.mCreationFlag = TEXTURE_CREATION_FLAG_SRGB;
        textureDesc.pFileName = "Stormtrooper_D.tex";
        textureDesc.ppTexture = &pUnitTexture;
        addResource(&textureDesc, NULL);
        textureDesc.pFileName = "PBR/copper_tiles_01/1K/Albedo.tex";
        textureDesc.ppTexture = &pGroundTexture;
        addResource(&textureDesc, NULL);

        AddCustomInputBindings();

        gRenderData.pUnits = (UnitInstance*)tf_malloc(sizeof(UnitInstance) * MAX_UNITS);
        gRenderData.pBillboards = (BillboardInstance*)tf_malloc(sizeof(BillboardInstance) * MAX_BILLBOARDS);

        const uint32_t cores = getNumCPUCores();
        pSim = simCreate(cores > 1 ? cores - 1 : 1);

        waitForAllResourceLoads();

        // Bone palette for the bind pose, used whenever nobody is dancing.
        ASSERT(pUnitGeomData->mJointCount <= MAX_NUM_BONES);
        gDanceObject.ComputeBindPose(gDanceObject.mRootTransform);
        for (uint32_t i = 0; i < pUnitGeomData->mJointCount; ++i)
            gBindPoseBones[i] = gDanceObject.mJointWorldMats[pUnitGeomData->pJointRemaps[i]] * pUnitGeomData->pInverseBindPoses[i];

        gFrameIndex = 0;
        return true;
    }

    void Exit()
    {
        simDestroy(pSim);
        pSim = NULL;
        tf_free(gRenderData.pUnits);
        tf_free(gRenderData.pBillboards);

        gRig.Exit();
        gDanceClip.Exit();
        gDanceAnimation.Exit();
        gDanceObject.Exit();

        exitProfiler();
        exitUserInterface();
        exitFontSystem();

        for (uint32_t i = 0; i < gDataBufferCount; ++i)
        {
            removeResource(pFrameUniformBuffers[i]);
            removeResource(pBoneBuffers[i]);
            removeResource(pUnitInstanceBuffers[i]);
            removeResource(pBillboardBuffers[i]);
        }
        removeResource(pUnitGeomData);
        pUnitGeomData = NULL;
        removeResource(pUnitGeom);
        pUnitGeom = NULL;
        removeResource(pUnitTexture);
        removeResource(pGroundTexture);

        exitSemaphore(pRenderer, pImageAcquiredSemaphore);
        exitGpuCmdRing(pRenderer, &gGraphicsCmdRing);
        exitRootSignature(pRenderer);
        exitResourceLoaderInterface(pRenderer);
        exitQueue(pRenderer, pGraphicsQueue);
        exitRenderer(pRenderer);
        exitGPUConfiguration();
        pRenderer = NULL;
    }

    bool Load(ReloadDesc* pReloadDesc)
    {
        if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
        {
            addShaders();
            addDescriptorSets();
        }

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            loadProfilerUI(mSettings.mWidth, mSettings.mHeight);
            if (!addSwapChain())
                return false;
            if (!addDepthBuffer())
                return false;
        }

        if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
            addPipelines();

        prepareDescriptorSets();

        UserInterfaceLoadDesc uiLoad = {};
        uiLoad.mColorFormat = pSwapChain->ppRenderTargets[0]->mFormat;
        uiLoad.mHeight = mSettings.mHeight;
        uiLoad.mWidth = mSettings.mWidth;
        uiLoad.mLoadType = pReloadDesc->mType;
        loadUserInterface(&uiLoad);

        FontSystemLoadDesc fontLoad = {};
        fontLoad.mColorFormat = pSwapChain->ppRenderTargets[0]->mFormat;
        fontLoad.mHeight = mSettings.mHeight;
        fontLoad.mWidth = mSettings.mWidth;
        fontLoad.mLoadType = pReloadDesc->mType;
        loadFontSystem(&fontLoad);
        return true;
    }

    void Unload(ReloadDesc* pReloadDesc)
    {
        waitQueueIdle(pGraphicsQueue);
        unloadFontSystem(pReloadDesc->mType);
        unloadUserInterface(pReloadDesc->mType);

        if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
            removePipelines();

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            removeSwapChain(pRenderer, pSwapChain);
            removeRenderTarget(pRenderer, pDepthBuffer);
            unloadProfilerUI();
        }

        if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
        {
            removeDescriptorSets();
            removeShaders();
        }
    }

    void Update(float deltaTime)
    {
        if (!uiIsFocused())
        {
            if (inputGetValue(0, CUSTOM_TOGGLE_FULLSCREEN))
                toggleFullscreen(pWindow);
            if (inputGetValue(0, CUSTOM_TOGGLE_UI))
                uiToggleActive();
            if (inputGetValue(0, CUSTOM_EXIT))
                requestShutdown();
        }

        if (gSmokeMode)
            updateSmoke(deltaTime);

        simStep(pSim, deltaTime);
        updateCamera(deltaTime);
        updateBones(deltaTime);
        simFillRenderData(pSim, &gRenderData);
        updateHud();
    }

    void updateCamera(float deltaTime)
    {
        UNREF_PARAM(deltaTime);
        const vec3 target = vec3(gCameraTarget.x, 0.0f, gCameraTarget.y);
        gCameraEye = target + vec3(0.0f, sinf(CAMERA_PITCH), -cosf(CAMERA_PITCH)) * gCameraDistance;
        gCameraForward = normalize(target - gCameraEye);
        gCameraRight = normalize(cross(vec3(0.0f, 1.0f, 0.0f), gCameraForward));
        gCameraUp = cross(gCameraForward, gCameraRight);

        const float        aspectInverse = (float)mSettings.mHeight / (float)mSettings.mWidth;
        const mat4         view = mat4::lookAtLH(Point3(gCameraEye), Point3(target), vec3(0.0f, 1.0f, 0.0f));
        const CameraMatrix proj = CameraMatrix::perspectiveReverseZ(CAMERA_FOV_X, aspectInverse, 0.5f, 500.0f);
        gFrameUniforms.mViewProj = proj.mCamera * view;
        gFrameUniforms.mCamRight = float4(gCameraRight.getX(), gCameraRight.getY(), gCameraRight.getZ(), 0.0f);
        gFrameUniforms.mCamUp = float4(gCameraUp.getX(), gCameraUp.getY(), gCameraUp.getZ(), 0.0f);
        gFrameUniforms.mArena = float4(ARENA_SIZE, (float)PLAYER_ROWS * CELL_SIZE, 0.0f, 0.0f);
        gFrameUniforms.mHoverCell = float4(0.0f, 0.0f, 0.0f, CELL_SIZE);
    }

    void updateBones(float deltaTime)
    {
        const Phase phase = simGetPhase(pSim);
        const bool  dancing = phase == PHASE_VICTORY || phase == PHASE_DEFEAT;
        if (dancing && gLastPhase != phase)
            gDanceController.SetTimeRatioHard(0.0f);
        gLastPhase = phase;

        if (!dancing)
        {
            memcpy(gBoneMatrices, gBindPoseBones, sizeof(gBoneMatrices));
            return;
        }
        gDanceObject.Update(deltaTime);
        gDanceObject.ComputePose(gDanceObject.mRootTransform);
        for (uint32_t i = 0; i < pUnitGeomData->mJointCount; ++i)
            gBoneMatrices[i] = gDanceObject.mJointWorldMats[pUnitGeomData->pJointRemaps[i]] * pUnitGeomData->pInverseBindPoses[i];
    }

    void updateHud()
    {
        const Phase phase = simGetPhase(pSim);
        snprintf(gHudLines[0], sizeof(gHudLines[0]), "Round %d    Gold %d    Blue %u vs Red %u    %s %.0f / %.0f s", simGetRound(pSim),
                 simGetGold(pSim), simGetUnitCount(pSim, TEAM_PLAYER), simGetUnitCount(pSim, TEAM_ENEMY), PHASE_NAMES[phase],
                 simGetBattleTime(pSim), BATTLE_TIME_LIMIT);
        static const char* HINTS[] = {
            "Left-click your (blue) half to buy the selected squad, right-click to sell. WASD pans, wheel zooms.",
            "Battle in progress",
            "VICTORY! Next round starting...",
            "DEFEAT - press Restart",
        };
        snprintf(gHudLines[1], sizeof(gHudLines[1]), "%s", HINTS[phase]);
        snprintf(gHudLines[2], sizeof(gHudLines[2]), "%s", simGetMessage(pSim));
    }

    void updateSmoke(float deltaTime)
    {
        if (gSmokeDone)
            return;
        if (gSmokeFrames == 0)
        {
            simTestClearBoard(pSim);
            simTestSetGold(pSim, 100000);
            for (int32_t x = 0; x < ARENA_CELLS; ++x)
            {
                for (int32_t z = 0; z < 3; ++z)
                    simBuySquad(pSim, x, z, UNIT_MELEE);
                simBuySquad(pSim, x, 3, UNIT_RANGED);
                simBuySquad(pSim, x, 4, UNIT_TANK);
                for (int32_t z = PLAYER_ROWS; z < PLAYER_ROWS + 3; ++z)
                    simTestSetCell(pSim, x, z, UNIT_MELEE);
            }
            gSmokeStartUnits = simGetUnitCount(pSim, TEAM_PLAYER) + simGetUnitCount(pSim, TEAM_ENEMY);
            simStartBattle(pSim);
        }
        ++gSmokeFrames;
        gSmokeSeconds += deltaTime;

        const Phase phase = simGetPhase(pSim);
        if (gSmokeResult == PHASE_PREP && (phase == PHASE_VICTORY || phase == PHASE_DEFEAT))
        {
            gSmokeResult = phase;
            gSmokeResultFrame = gSmokeFrames;
        }
        // Keep running ~2 s into the result so the dance path is exercised too.
        if ((gSmokeResult != PHASE_PREP && gSmokeFrames > gSmokeResultFrame + 120) || gSmokeSeconds > 150.0f)
        {
            char summary[256];
            snprintf(summary, sizeof(summary), "units_at_start=%u\nframes=%u\nseconds=%.1f\navg_frame_ms=%.2f\nresult=%s\n", gSmokeStartUnits,
                     gSmokeFrames, gSmokeSeconds, 1000.0f * gSmokeSeconds / (float)gSmokeFrames, PHASE_NAMES[gSmokeResult]);
            writeTextFile("40_AutoBattler_smoke.txt", summary);
            gSmokeDone = true;
            requestShutdown();
        }
    }

    void Draw()
    {
        if ((bool)pSwapChain->mEnableVsync != mSettings.mVSyncEnabled)
        {
            waitQueueIdle(pGraphicsQueue);
            ::toggleVSync(pRenderer, &pSwapChain);
        }

        uint32_t swapchainImageIndex;
        acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, NULL, &swapchainImageIndex);

        GpuCmdRingElement elem = getNextGpuCmdRingElement(&gGraphicsCmdRing, true, 1);
        FenceStatus       fenceStatus;
        getFenceStatus(pRenderer, elem.pFence, &fenceStatus);
        if (fenceStatus == FENCE_STATUS_INCOMPLETE)
            waitForFences(pRenderer, 1, &elem.pFence);
        resetCmdPool(pRenderer, elem.pCmdPool);

        // Upload this frame's data (after the fence, so the GPU is done with these buffers).
        BufferUpdateDesc frameUpdate = { pFrameUniformBuffers[gFrameIndex] };
        beginUpdateResource(&frameUpdate);
        memcpy(frameUpdate.pMappedData, &gFrameUniforms, sizeof(gFrameUniforms));
        endUpdateResource(&frameUpdate);

        BufferUpdateDesc boneUpdate = { pBoneBuffers[gFrameIndex] };
        beginUpdateResource(&boneUpdate);
        memcpy(boneUpdate.pMappedData, gBoneMatrices, sizeof(mat4) * pUnitGeomData->mJointCount);
        endUpdateResource(&boneUpdate);

        if (gRenderData.mUnitCount > 0)
        {
            BufferUpdateDesc unitUpdate = { pUnitInstanceBuffers[gFrameIndex] };
            unitUpdate.mCurrentState = RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            beginUpdateResource(&unitUpdate);
            memcpy(unitUpdate.pMappedData, gRenderData.pUnits, sizeof(UnitInstance) * gRenderData.mUnitCount);
            endUpdateResource(&unitUpdate);
        }
        if (gRenderData.mBillboardCount > 0)
        {
            BufferUpdateDesc billboardUpdate = { pBillboardBuffers[gFrameIndex] };
            billboardUpdate.mCurrentState = RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            beginUpdateResource(&billboardUpdate);
            memcpy(billboardUpdate.pMappedData, gRenderData.pBillboards, sizeof(BillboardInstance) * gRenderData.mBillboardCount);
            endUpdateResource(&billboardUpdate);
        }

        RenderTarget* pRenderTarget = pSwapChain->ppRenderTargets[swapchainImageIndex];
        Cmd*          cmd = elem.pCmds[0];
        beginCmd(cmd);
        cmdBeginGpuFrameProfile(cmd, gGpuProfileToken);

        RenderTargetBarrier barriers[] = { { pRenderTarget, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET } };
        cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriers);

        BindRenderTargetsDesc bindRenderTargets = {};
        bindRenderTargets.mRenderTargetCount = 1;
        bindRenderTargets.mRenderTargets[0] = { pRenderTarget, LOAD_ACTION_CLEAR };
        bindRenderTargets.mDepthStencil = { pDepthBuffer, LOAD_ACTION_CLEAR };
        cmdBindRenderTargets(cmd, &bindRenderTargets);
        cmdSetViewport(cmd, 0.0f, 0.0f, (float)pRenderTarget->mWidth, (float)pRenderTarget->mHeight, 0.0f, 1.0f);
        cmdSetScissor(cmd, 0, 0, pRenderTarget->mWidth, pRenderTarget->mHeight);

        cmdBeginDebugMarker(cmd, 1, 0, 1, "Ground");
        cmdBindPipeline(cmd, pGroundPipeline);
        cmdBindDescriptorSet(cmd, 0, pSetPersistent);
        cmdBindDescriptorSet(cmd, gFrameIndex, pSetPerFrame);
        cmdDraw(cmd, 6, 0);
        cmdEndDebugMarker(cmd);

        if (gRenderData.mUnitCount > 0)
        {
            cmdBeginDebugMarker(cmd, 1, 0, 1, "Units");
            cmdBindPipeline(cmd, pUnitPipeline);
            cmdBindDescriptorSet(cmd, 0, pSetPersistent);
            cmdBindDescriptorSet(cmd, gFrameIndex, pSetPerFrame);
            cmdBindVertexBuffer(cmd, 1, &pUnitGeom->pVertexBuffers[0], pUnitGeom->mVertexStrides, (uint64_t*)NULL);
            cmdBindIndexBuffer(cmd, pUnitGeom->pIndexBuffer, pUnitGeom->mIndexType, (uint64_t)NULL);
            cmdDrawIndexedInstanced(cmd, pUnitGeom->mIndexCount, 0, gRenderData.mUnitCount, 0, 0);
            cmdEndDebugMarker(cmd);
        }

        if (gRenderData.mBillboardCount > 0)
        {
            cmdBeginDebugMarker(cmd, 1, 0, 1, "Billboards");
            cmdBindPipeline(cmd, pBillboardPipeline);
            cmdBindDescriptorSet(cmd, 0, pSetPersistent);
            cmdBindDescriptorSet(cmd, gFrameIndex, pSetPerFrame);
            cmdDraw(cmd, 6 * gRenderData.mBillboardCount, 0);
            cmdEndDebugMarker(cmd);
        }

        cmdBeginDebugMarker(cmd, 0, 1, 0, "UI");
        bindRenderTargets = {};
        bindRenderTargets.mRenderTargetCount = 1;
        bindRenderTargets.mRenderTargets[0] = { pRenderTarget, LOAD_ACTION_LOAD };
        cmdBindRenderTargets(cmd, &bindRenderTargets);

        gTextDraw.mFontID = gFontID;
        gTextDraw.mFontColor = 0xff00ffff;
        gTextDraw.mFontSize = 18.0f;
        const float2 profileSize = cmdDrawCpuProfile(cmd, float2(8.0f, 15.0f), &gTextDraw);
        if (uiIsRenderingEnabled())
        {
            gTextDraw.mFontColor = 0xffffffff;
            gTextDraw.mFontSize = 22.0f;
            for (uint32_t i = 0; i < 3; ++i)
            {
                gTextDraw.pText = gHudLines[i];
                cmdDrawTextWithFont(cmd, float2(8.0f, profileSize.y + 40.0f + 28.0f * (float)i), &gTextDraw);
            }
        }
        cmdDrawUserInterface(cmd);
        cmdBindRenderTargets(cmd, NULL);
        cmdEndDebugMarker(cmd);

        barriers[0] = { pRenderTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PRESENT };
        cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriers);
        cmdEndGpuFrameProfile(cmd, gGpuProfileToken);
        endCmd(cmd);

        FlushResourceUpdateDesc flushUpdateDesc = {};
        flushUpdateDesc.mNodeIndex = 0;
        flushResourceUpdates(&flushUpdateDesc);
        Semaphore* waitSemaphores[2] = { flushUpdateDesc.pOutSubmittedSemaphore, pImageAcquiredSemaphore };

        QueueSubmitDesc submitDesc = {};
        submitDesc.mCmdCount = 1;
        submitDesc.mSignalSemaphoreCount = 1;
        submitDesc.mWaitSemaphoreCount = TF_ARRAY_COUNT(waitSemaphores);
        submitDesc.ppCmds = &cmd;
        submitDesc.ppSignalSemaphores = &elem.pSemaphore;
        submitDesc.ppWaitSemaphores = waitSemaphores;
        submitDesc.pSignalFence = elem.pFence;
        queueSubmit(pGraphicsQueue, &submitDesc);

        QueuePresentDesc presentDesc = {};
        presentDesc.mIndex = (uint8_t)swapchainImageIndex;
        presentDesc.mWaitSemaphoreCount = 1;
        presentDesc.ppWaitSemaphores = &elem.pSemaphore;
        presentDesc.pSwapChain = pSwapChain;
        presentDesc.mSubmitDone = true;
        queuePresent(pGraphicsQueue, &presentDesc);
        flipProfiler();

        gFrameIndex = (gFrameIndex + 1) % gDataBufferCount;
    }

    const char* GetName() { return "40_AutoBattler"; }

    bool addSwapChain()
    {
        SwapChainDesc swapChainDesc = {};
        swapChainDesc.mWindowHandle = pWindow->handle;
        swapChainDesc.mPresentQueueCount = 1;
        swapChainDesc.ppPresentQueues = &pGraphicsQueue;
        swapChainDesc.mWidth = mSettings.mWidth;
        swapChainDesc.mHeight = mSettings.mHeight;
        swapChainDesc.mImageCount = getRecommendedSwapchainImageCount(pRenderer, &pWindow->handle);
        swapChainDesc.mColorFormat = getSupportedSwapchainFormat(pRenderer, &swapChainDesc, COLOR_SPACE_SDR_SRGB);
        swapChainDesc.mColorSpace = COLOR_SPACE_SDR_SRGB;
        swapChainDesc.mColorClearValue = { { 0.08f, 0.09f, 0.11f, 1.0f } };
        swapChainDesc.mEnableVsync = mSettings.mVSyncEnabled;
        ::addSwapChain(pRenderer, &swapChainDesc, &pSwapChain);
        return pSwapChain != NULL;
    }

    bool addDepthBuffer()
    {
        ESRAM_BEGIN_ALLOC(pRenderer, "Depth", 0);
        RenderTargetDesc depthRT = {};
        depthRT.mArraySize = 1;
        depthRT.mClearValue.depth = 0.0f;
        depthRT.mClearValue.stencil = 0;
        depthRT.mDepth = 1;
        depthRT.mFormat = TinyImageFormat_D32_SFLOAT;
        depthRT.mStartState = RESOURCE_STATE_DEPTH_WRITE;
        depthRT.mHeight = mSettings.mHeight;
        depthRT.mSampleCount = SAMPLE_COUNT_1;
        depthRT.mSampleQuality = 0;
        depthRT.mWidth = mSettings.mWidth;
        depthRT.mFlags = TEXTURE_CREATION_FLAG_ESRAM | TEXTURE_CREATION_FLAG_ON_TILE | TEXTURE_CREATION_FLAG_VR_MULTIVIEW;
        addRenderTarget(pRenderer, &depthRT, &pDepthBuffer);
        ESRAM_END_ALLOC(pRenderer);
        return pDepthBuffer != NULL;
    }

    void addShaders()
    {
        ShaderLoadDesc unitShader = {};
        unitShader.mVert.pFileName = "unit.vert";
        unitShader.mFrag.pFileName = "unit.frag";
        addShader(pRenderer, &unitShader, &pUnitShader);

        ShaderLoadDesc groundShader = {};
        groundShader.mVert.pFileName = "ground.vert";
        groundShader.mFrag.pFileName = "ground.frag";
        addShader(pRenderer, &groundShader, &pGroundShader);

        ShaderLoadDesc billboardShader = {};
        billboardShader.mVert.pFileName = "billboard.vert";
        billboardShader.mFrag.pFileName = "billboard.frag";
        addShader(pRenderer, &billboardShader, &pBillboardShader);
    }

    void removeShaders()
    {
        removeShader(pRenderer, pUnitShader);
        removeShader(pRenderer, pGroundShader);
        removeShader(pRenderer, pBillboardShader);
    }

    void addDescriptorSets()
    {
        DescriptorSetDesc setDesc = SRT_SET_DESC(SrtData, Persistent, 1, 0);
        addDescriptorSet(pRenderer, &setDesc, &pSetPersistent);
        setDesc = SRT_SET_DESC(SrtData, PerFrame, gDataBufferCount, 0);
        addDescriptorSet(pRenderer, &setDesc, &pSetPerFrame);
    }

    void removeDescriptorSets()
    {
        removeDescriptorSet(pRenderer, pSetPersistent);
        removeDescriptorSet(pRenderer, pSetPerFrame);
    }

    void prepareDescriptorSets()
    {
        DescriptorData persistent[2] = {};
        persistent[0].mIndex = SRT_RES_IDX(SrtData, Persistent, gUnitTexture);
        persistent[0].ppTextures = &pUnitTexture;
        persistent[1].mIndex = SRT_RES_IDX(SrtData, Persistent, gGroundTexture);
        persistent[1].ppTextures = &pGroundTexture;
        updateDescriptorSet(pRenderer, 0, pSetPersistent, 2, persistent);

        for (uint32_t i = 0; i < gDataBufferCount; ++i)
        {
            DescriptorData perFrame[4] = {};
            perFrame[0].mIndex = SRT_RES_IDX(SrtData, PerFrame, gFrame);
            perFrame[0].ppBuffers = &pFrameUniformBuffers[i];
            perFrame[1].mIndex = SRT_RES_IDX(SrtData, PerFrame, gBoneMatrices);
            perFrame[1].ppBuffers = &pBoneBuffers[i];
            perFrame[2].mIndex = SRT_RES_IDX(SrtData, PerFrame, gUnitInstances);
            perFrame[2].ppBuffers = &pUnitInstanceBuffers[i];
            perFrame[3].mIndex = SRT_RES_IDX(SrtData, PerFrame, gBillboards);
            perFrame[3].ppBuffers = &pBillboardBuffers[i];
            updateDescriptorSet(pRenderer, i, pSetPerFrame, 4, perFrame);
        }
    }

    void addPipelines()
    {
        RasterizerStateDesc cullNone = {};
        cullNone.mCullMode = CULL_MODE_NONE;
        RasterizerStateDesc cullFront = {}; // the Stormtrooper mesh is wound this way, as in 28_Skinning
        cullFront.mCullMode = CULL_MODE_FRONT;

        DepthStateDesc depthState = {};
        depthState.mDepthTest = true;
        depthState.mDepthWrite = true;
        depthState.mDepthFunc = CMP_GEQUAL;

        PipelineDesc desc = {};
        desc.mType = PIPELINE_TYPE_GRAPHICS;
        PIPELINE_LAYOUT_DESC(desc, SRT_LAYOUT_DESC(SrtData, Persistent), SRT_LAYOUT_DESC(SrtData, PerFrame), NULL, NULL);
        GraphicsPipelineDesc& pipelineSettings = desc.mGraphicsDesc;
        pipelineSettings.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
        pipelineSettings.mRenderTargetCount = 1;
        pipelineSettings.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
        pipelineSettings.mSampleCount = pSwapChain->ppRenderTargets[0]->mSampleCount;
        pipelineSettings.mSampleQuality = pSwapChain->ppRenderTargets[0]->mSampleQuality;
        pipelineSettings.mDepthStencilFormat = pDepthBuffer->mFormat;
        pipelineSettings.pDepthState = &depthState;

        pipelineSettings.pRasterizerState = &cullNone;
        pipelineSettings.pVertexLayout = NULL;
        pipelineSettings.pShaderProgram = pGroundShader;
        addPipeline(pRenderer, &desc, &pGroundPipeline);

        pipelineSettings.pShaderProgram = pBillboardShader;
        addPipeline(pRenderer, &desc, &pBillboardPipeline);

        pipelineSettings.pRasterizerState = &cullFront;
        pipelineSettings.pVertexLayout = &gUnitVertexLayout;
        pipelineSettings.pShaderProgram = pUnitShader;
        addPipeline(pRenderer, &desc, &pUnitPipeline);
    }

    void removePipelines()
    {
        removePipeline(pRenderer, pUnitPipeline);
        removePipeline(pRenderer, pGroundPipeline);
        removePipeline(pRenderer, pBillboardPipeline);
    }
};

DEFINE_APPLICATION_MAIN(AutoBattler)
```

- [ ] **Step 4: Build, then run the self test (regression)**

Run the shared **Build** (Debug) and **Self test** commands. Expected: build has no warnings, shaders compile for Vulkan and D3D12, `exit=0`.

- [ ] **Step 5: Run the smoke test**

```bash
OUT=Examples_3/Unit_Tests/PC_VS2019/x64/Debug/40_AutoBattler
(cd "$OUT" && timeout 200 ./40_AutoBattler.exe --smoke; echo "exit=$?")
find "$OUT" -name '40_AutoBattler_smoke.txt' -exec cat {} \;
grep -iE "error|assert|validation" "$OUT/40_AutoBattler.log" | head -20
```

Expected: `exit=0`; the smoke file shows `units_at_start=` at least 2800 and `result=Victory` or `result=Defeat` (not `Prep`, which means the 150 s cap was hit). The log grep shows no GPU validation errors and no asserts.

- [ ] **Step 6: Look at it**

Launch `40_AutoBattler.exe --smoke` once without the timeout and check these by eye. Fix anything that's wrong before committing.
- Stormtroopers stand upright on the copper ground: blue on the near half, red on the far half.
- Blue units face the red side before the battle starts. If they face away, the mesh's forward axis is −Z: add `+ PI` to the yaw in `simFillRenderData`, and re-run the self test.
- Health bars appear over damaged units, yellow projectiles fly from ranged units, and hit units flash.
- Survivors dance after the battle ends.

If the units are mirrored or inside out (back faces showing), change `cullFront` to `CULL_MODE_BACK`.

- [ ] **Step 7: Commit**

```bash
git add Examples_3/Unit_Tests/src/40_AutoBattler Examples_3/Unit_Tests/PC_VS2019/40_AutoBattler.vcxproj
git commit -m "Render 40_AutoBattler: instanced skinned units, ground, billboards, dance

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Player interaction (camera, picking, buy/sell, UI) and final verification

**Files:**
- Modify: `Examples_3/Unit_Tests/src/40_AutoBattler/40_AutoBattler.cpp`

**Interfaces:**
- Consumes (Task 4): `gCameraTarget`, `gCameraDistance`, `gCameraEye/Forward/Right/Up`, `CAMERA_FOV_X`, `gFrameUniforms.mHoverCell`, `updateCamera`; sim calls `simCanBuySquad/simBuySquad/simSellSquad/simStartBattle/simRestart`; `rayToCell` (Task 1).

- [ ] **Step 1: Add input bindings, UI state and helpers**

Add after the `gSmoke*` globals:

```cpp
UIComponent* pGuiWindow = NULL;
int32_t      gSelectedType = UNIT_MELEE;
bool         gStartRequested = false;
bool         gRestartRequested = false;
InputEnum    CUSTOM_PLACE = {};
InputEnum    CUSTOM_SELL = {};
InputEnum    CUSTOM_ZOOM = {};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
```

In `Init`, right after `AddCustomInputBindings();`:

```cpp
        inputAddCustomBindings(R"(
ab_place; button; MOUSE_1; released
ab_sell; button; MOUSE_2; released
ab_zoom; buttonanalog; MOUSE_WHEEL_DOWN; 1.0f; MOUSE_WHEEL_UP; -1.0f)");
        CUSTOM_PLACE = inputGetCustomBindingEnum("ab_place");
        CUSTOM_SELL = inputGetCustomBindingEnum("ab_sell");
        CUSTOM_ZOOM = inputGetCustomBindingEnum("ab_zoom");
```

- [ ] **Step 2: Add the UI window**

Add as a member of `AutoBattler`:

```cpp
    void addGui()
    {
        UIComponentDesc guiDesc = {};
        guiDesc.mStartPosition = vec2(mSettings.mWidth * 0.78f, mSettings.mHeight * 0.05f);
        guiDesc.mStartSize = vec2(300.0f, 240.0f);
        guiDesc.mFontID = 0;
        guiDesc.mFontSize = 16.0f;
        uiAddComponent("Auto Battler", &guiDesc, &pGuiWindow);

        for (int32_t t = 0; t < UNIT_TYPE_COUNT; ++t)
        {
            char label[64];
            snprintf(label, sizeof(label), "%s squad x%d (%d gold)", gUnitStats[t].pName, gUnitStats[t].mSquadSide * gUnitStats[t].mSquadSide,
                     gUnitStats[t].mCost);
            RadioButtonWidget radio = {};
            radio.pData = &gSelectedType;
            radio.mRadioId = t;
            uiAddComponentWidget(pGuiWindow, label, &radio, WIDGET_TYPE_RADIO_BUTTON);
        }

        ButtonWidget startButton = {};
        UIWidget*    pStart = uiAddComponentWidget(pGuiWindow, "Start Battle", &startButton, WIDGET_TYPE_BUTTON);
        uiSetWidgetOnEditedCallback(pStart, NULL, [](void*) { gStartRequested = true; });

        ButtonWidget restartButton = {};
        UIWidget*    pRestart = uiAddComponentWidget(pGuiWindow, "Restart", &restartButton, WIDGET_TYPE_BUTTON);
        uiSetWidgetOnEditedCallback(pRestart, NULL, [](void*) { gRestartRequested = true; });
    }
```

In `Load`, inside the `RESIZE | RENDERTARGET` block, after `loadProfilerUI(...)`:

```cpp
            addGui();
```

In `Unload`, inside the `RESIZE | RENDERTARGET` block, after `unloadProfilerUI();`:

```cpp
            uiRemoveComponent(pGuiWindow);
```

- [ ] **Step 3: Camera input and picking**

Add as members:

```cpp
    void updateCameraInput(float deltaTime)
    {
        if (uiIsFocused())
            return;
        const float panSpeed = gCameraDistance * 0.8f;
        gCameraTarget.x = clampf(gCameraTarget.x + inputGetValue(0, CUSTOM_MOVE_X) * panSpeed * deltaTime, 0.0f, ARENA_SIZE);
        gCameraTarget.y = clampf(gCameraTarget.y + inputGetValue(0, CUSTOM_MOVE_Y) * panSpeed * deltaTime, 0.0f, ARENA_SIZE);
        gCameraDistance = clampf(gCameraDistance + inputGetValue(0, CUSTOM_ZOOM) * 5.0f, 20.0f, 120.0f);
    }

    // Casts the mouse ray onto the ground, highlights the cell and buys/sells on click (prep only).
    void updatePicking()
    {
        if (simGetPhase(pSim) != PHASE_PREP || uiIsFocused())
            return;
        const float ndcX = 2.0f * inputGetValue(0, CUSTOM_PT_X) / (float)mSettings.mWidth - 1.0f;
        const float ndcY = 1.0f - 2.0f * inputGetValue(0, CUSTOM_PT_Y) / (float)mSettings.mHeight;
        const float tanHalfX = tanf(CAMERA_FOV_X * 0.5f);
        const float tanHalfY = tanHalfX * (float)mSettings.mHeight / (float)mSettings.mWidth;
        const vec3  dir = normalize(gCameraForward + gCameraRight * (ndcX * tanHalfX) + gCameraUp * (ndcY * tanHalfY));

        int32_t cellX = 0;
        int32_t cellZ = 0;
        if (!rayToCell(float3(gCameraEye.getX(), gCameraEye.getY(), gCameraEye.getZ()), float3(dir.getX(), dir.getY(), dir.getZ()), &cellX,
                       &cellZ))
            return;

        const bool canBuy = simCanBuySquad(pSim, cellX, cellZ, (UnitType)gSelectedType);
        gFrameUniforms.mHoverCell = float4((float)cellX, canBuy ? 1.0f : 2.0f, (float)cellZ, CELL_SIZE);
        if (inputGetValue(0, CUSTOM_PLACE))
            simBuySquad(pSim, cellX, cellZ, (UnitType)gSelectedType);
        if (inputGetValue(0, CUSTOM_SELL))
            simSellSquad(pSim, cellX, cellZ);
    }
```

In `Update`, replace

```cpp
        simStep(pSim, deltaTime);
        updateCamera(deltaTime);
```

with

```cpp
        if (gStartRequested)
        {
            gStartRequested = false;
            simStartBattle(pSim);
        }
        if (gRestartRequested)
        {
            gRestartRequested = false;
            simRestart(pSim);
        }

        simStep(pSim, deltaTime);
        updateCameraInput(deltaTime);
        updateCamera(deltaTime);
        updatePicking();
```

(`updateCamera` resets `mHoverCell` every frame, then `updatePicking` sets it when the cursor is over a cell.)

- [ ] **Step 4: Build Debug and Release; run self test and smoke in both**

For `CONFIG` in `Debug` and `Release`: run the shared **Build** with `-p:Configuration=$CONFIG`, then the **Self test** and Task 4 Step 5 smoke commands with `OUT=Examples_3/Unit_Tests/PC_VS2019/x64/$CONFIG/40_AutoBattler`.
Expected for both: no build warnings; self test `exit=0` / `RESULT: 0 failure(s)`; smoke `exit=0` with `result=Victory` or `result=Defeat`; no errors or asserts in the log. Record Release `avg_frame_ms` from the smoke file for the final report.

- [ ] **Step 5: Manual play check**

Launch `Examples_3/Unit_Tests/PC_VS2019/x64/Release/40_AutoBattler/40_AutoBattler.exe` with no arguments and confirm:
- Hovering the blue half shows a green cell; hovering an occupied or red-half cell shows a red cell.
- Left-click buys the selected type and gold drops by its cost. Right-click on your squad sells it with a full refund. Clicking inside the UI window never places a squad.
- With nothing placed, Start Battle shows "Place at least one squad first".
- Win a round: after the dance, round 2 begins, gold rises by 5 + round, your squads are back at full strength, and a new red wave appears.
- Lose a round: the Defeat text shows; Restart returns to round 1 with 10 gold.
- WASD pans and the mouse wheel zooms, both clamped to the arena.
- Resize the window mid-battle: rendering continues and the battle state is unaffected.

- [ ] **Step 6: Commit**

```bash
git add Examples_3/Unit_Tests/src/40_AutoBattler/40_AutoBattler.cpp
git commit -m "Add 40_AutoBattler camera, picking, squad shop UI and round controls

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
