# Conquest Plan 1 — Foundation (reorganise, replace the old rules) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put AutoBattler's code into the spec's folder layout and replace the old place-squads auto-battler rules with a GameMode that runs a Sandbox battle, keeping the game running and every remaining test green.

**Architecture:**
- The headless sim is split into focused `Game/` modules (components, unit types, spatial grid, steering,
  combat, health, sim core) behind the existing `sim*` API.
- The app, its rendering helpers and the self test move into `App/`, `App/Render/` and `Tests/`.
- The board, phases, rounds, waves and buy/sell code are deleted. A new `GameMode` module owns the one
  ruleset that exists so far, Sandbox: clear the world, spawn two facing armies, step the sim, and declare
  a winner when one army is gone. `--stress`, `--smoke` and the interactive app all run Sandbox.

**Tech Stack:** The Forge 1.63 (C++14, FSL shaders for Vulkan and D3D12), flecs 4.0.1, MSBuild with VS2019 (v142).

**Spec:** `Games/AutoBattler/docs/2026-10-17-conquest-design.md` (build-order steps 1–2). This is plan 1 of 3:
- Plan 2 covers steps 3–5: map, places, garrisons, the king, `PlayerController` and steering.
- Plan 3 covers steps 6–8: capture, income, recruiting, defeat and victory, the HUD, rings and smoke.

## Global Constraints

- Orthodox C++, as in the spec's "Code style" section:
  - plain structs plus free functions; the only class is `AutoBattler : IApp`;
  - no exceptions, RTTI, STL containers/strings/streams or virtual interfaces;
  - templates, lambdas and `auto` only where they make code simpler;
  - C headers (`<math.h>`, `<string.h>`, `<stdio.h>`), `tf_malloc` / `tf_calloc` / `tf_free`, `snprintf`;
  - C++14 at most.
- Dependencies go one way: App → Game. No file in `Game/` includes anything from `App/` or engine graphics
  headers (`IGraphics.h`, `IResourceLoader.h`, `IUI.h`, ...).
- Branch `feature/auto-battler-1.63` in `C:\Dev\Engine\The-Forge`. Commit per task. Push only when the user
  asks.
- Do not touch `Common_3/` or `Art/`.
- Build:
  - Debug: `"/c/Program Files (x86)/Microsoft Visual Studio/2019/Community/MSBuild/Current/Bin/MSBuild.exe" Games/AutoBattler/PC_VS2019/AutoBattler.sln -p:Configuration=Debug -p:Platform=x64 -m -v:minimal`.
  - Release: the same with `-p:Configuration=Release`.
  - On LNK1103 for flecs.obj, kill `mspdbsrv` and delete `Games/AutoBattler/PC_VS2019/x64/<Config>/Intermediate/AutoBattler`, then rebuild.
- Run from `Games/AutoBattler/PC_VS2019/x64/<Config>/AutoBattler/`, always with a timeout so a hang cannot
  block: `timeout 900 ./AutoBattler.exe --selftest` (then `--smoke`, `--stress`). The outputs are
  `AutoBattler_selftest.txt` (must end `RESULT: 0 failure(s)`), `AutoBattler_smoke.txt` and
  `AutoBattler_stress.txt`.
- Engine includes: `../../../../Common_3/...` from `src/Game/`, `src/App/` and `src/Tests/`;
  `../../../../../Common_3/...` from `src/App/Render/`. Shaders stay in `src/Shaders/FSL/` and keep their
  include paths.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **A unit spawned outside the world or past the unit capacity.** `simSpawnUnit` must refuse and return
   false, not assert or write out of bounds. Test in Task 3.
2. **A Sandbox army bigger than the capacity.** `gameModeSandboxStart` must refuse and leave the sim empty
   (or unchanged), not half-spawn. Test in Task 3.
3. **Sandbox restarted mid-battle**, with projectiles in flight and a winner already declared. Restart must
   clear the units, projectiles, winner and celebration. Test in Task 3.
4. **A NaN, negative or huge `dt` passed to `gameModeUpdate`.** It must be ignored, or clamped like
   `simStep`, and never declare a winner by accident. Test in Task 3.
5. **Moved files are actually compiled.** A reorganised `.cpp` missing from the project would compile
   nothing and link an older object. Tasks 1 and 2 check that the self-test count stays at 214 and that
   `git status` shows no stray old files.

---

### Task 1: Split the sim into `Game/` modules (no behaviour change)

**Files:**
- Create:
  - `Games/AutoBattler/src/Game/Sim.h`, the public API (today's `AutoBattlerSim.h` content);
  - `Game/SimInternal.h`, shared private state: `BattleSim`, `SpatialGrid`, `GridEntry`, `ScopedUSec`,
    `SteerChunk` and cross-module function declarations;
  - `Game/Components.h`;
  - `Game/UnitTypes.h`, `Game/UnitTypes.cpp`;
  - `Game/SpatialGrid.cpp`;
  - `Game/Steering.cpp`;
  - `Game/Combat.cpp`;
  - `Game/Health.cpp`;
  - `Game/Sim.cpp`;
  - `Game/UnitInstance.h`, moved from `src/UnitInstance.h` (see the ruling note below).
- Delete: `Games/AutoBattler/src/AutoBattlerSim.h`, `AutoBattlerSim.cpp`, `UnitInstance.h`.
- Modify: `Games/AutoBattler/PC_VS2019/AutoBattler.vcxproj` and `.vcxproj.filters`. Every `#include` of
  `AutoBattlerSim.h` / `UnitInstance.h` in `AutoBattler.cpp`, `AutoBattlerSelfTest.cpp`, `AnimationBake.h`
  and `UnitCulling.h` becomes `Game/Sim.h` / `Game/UnitInstance.h`.

**Ruling note:** `UnitInstance.h` goes in `Game/`, not `App/Render/` as the spec's folder list suggests.
The sim writes `UnitInstance` in `simFillRenderData`, so placing it in `App/` would make Game include App
and break the spec's one-way dependency rule. It has no graphics dependencies; it is the Game → renderer
data contract.

**Interfaces:**
- Produces (in `Game/SimInternal.h`, for later tasks and plans):
  - `struct BattleSim` (every field it has today);
  - `void buildGrid(BattleSim* p)`;
  - `const GridEntry* lookupTarget(const ecs_world_t*, const SpatialGrid&, ecs_entity_t)`;
  - `int32_t findNearestEnemy(const SpatialGrid&, float x, float z, uint32_t team, uint32_t* pBucketsVisited)`;
  - `void separation(const SpatialGrid&, ecs_entity_t self, float x, float z, float radius, float* pOutX, float* pOutZ)`;
  - `void steerUnits(BattleSim* p, float dt)`;
  - `void combatRegisterSystems(BattleSim* p)`;
  - `void healthRegisterSystems(BattleSim* p)`;
  - `void applyDamage(ecs_world_t* pWorld, ecs_entity_t target, float damage)`.
- Produces (in `Game/Components.h`): the structs `Position`, `Unit`, `Health`, `Attack`, `GridSlot`,
  `Projectile`, with `extern ECS_COMPONENT_DECLARE(T);` for each, and the `AB_DEFINE_COMPONENT` macro.
- The public `sim*` API in `Game/Sim.h` is unchanged.

- [ ] **Step 1: Record the baseline.** Build Debug and run `--selftest`. Expected: `RESULT: 0 failure(s)`
  with 214 `pass` lines (`grep -c pass AutoBattler_selftest.txt` → 214). Write the number down; every
  later step in this task must reproduce it exactly.

- [ ] **Step 2: Create `Game/Components.h`.** Move the component structs and the component macros out of
  `AutoBattlerSim.cpp` (today's lines 30–83) unchanged, and declare the component ids `extern` so several
  `.cpp` files share them:

```cpp
#pragma once

#include "../../../../Common_3/Game/ThirdParty/OpenSource/flecs/flecs.h"

// Flecs components of the battle simulation. Ids are defined (ECS_COMPONENT_DECLARE) once in Sim.cpp and
// registered per world with AB_DEFINE_COMPONENT.
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
    float    animTime;  // animation clock, offset per unit so crowds are not in lockstep
    uint32_t moving;    // walked this frame (drives the walk vs idle clip)
    uint32_t attacking; // in range of its target this frame (drives the attack clip)
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

extern ECS_COMPONENT_DECLARE(Position);
extern ECS_COMPONENT_DECLARE(Unit);
extern ECS_COMPONENT_DECLARE(Health);
extern ECS_COMPONENT_DECLARE(Attack);
extern ECS_COMPONENT_DECLARE(GridSlot);
extern ECS_COMPONENT_DECLARE(Projectile);

// Component ids are globals; reset them so each new world registers fresh ids.
#define AB_DEFINE_COMPONENT(world, T) \
    ecs_id(T) = 0;                    \
    ECS_COMPONENT_DEFINE(world, T)
```

  The non-extern `ECS_COMPONENT_DECLARE(Position);` (and the other five) stay in `Sim.cpp`.

- [ ] **Step 3: Create `Game/UnitTypes.h` / `.cpp`.**
  - Move `enum UnitType`, `struct UnitStats` and `extern const UnitStats gUnitStats[]` from the sim header
    into `UnitTypes.h`. `Sim.h` includes it.
  - Move the `gUnitStats` table definition into `UnitTypes.cpp` unchanged.

- [ ] **Step 4: Create `Game/SimInternal.h`.** It holds, moved unchanged from `AutoBattlerSim.cpp`:
  - `GridEntry`, `SpatialGrid` and `NO_ENEMY`;
  - `struct BattleSim` and `struct SteerChunk` with `STEER_CHUNK_SIZE`;
  - `ScopedUSec`;
  - the constants that more than one module uses: `RETARGET_INTERVAL`, `SEPARATION_SPEED`,
    `WALK_ANIM_SPEED` and `GRID_BUCKET_SIZE`;
  - the declarations listed under Interfaces.

  It includes `Sim.h`, `Components.h`, `ITime.h`, `Atomics.h` and `ThreadSystem.h` (the last three via
  `../../../../Common_3/Utilities/...`). Functions that were `static` and are now called from another
  module lose `static`; everything still used by only one module stays `static` in that module.

- [ ] **Step 5: Create the module `.cpp` files.** Move each function unchanged:

| File | Functions moved from `AutoBattlerSim.cpp` |
|---|---|
| `SpatialGrid.cpp` | `bucketCoord`, `bucketOf`, `buildEnemyRings`, `buildGrid`, `lookupTarget`, `findNearestEnemy`, `separation`, and the body of `simTestNearestEnemySearch` |
| `Steering.cpp` | `steerChunk`, `steerWorker`, `steerUnits` |
| `Health.cpp` | `applyDamage`, `HealthSystem`, plus new `healthRegisterSystems(p)`, which holds the `addSystem(p, "HealthSystem", ...)` call |
| `Combat.cpp` | `AttackSystem`, `ProjectileSystem`, plus new `combatRegisterSystems(p)`, which holds the two `addSystem` calls for them, in that order |
| `Sim.cpp` | everything else: board helpers, spawning, waves, `stressLayout`, `addSystem` (now non-static, declared in `SimInternal.h`), `enterResult`, create/destroy/restart/step, board editing, render-data fill, getters, test hooks, timings, and the six `ECS_COMPONENT_DECLARE` definitions |

  In `simCreate`, replace the three `addSystem` calls with `combatRegisterSystems(p); healthRegisterSystems(p);`.
  This keeps the same registration order, so system execution order is unchanged.

- [ ] **Step 6: Update includes and the project.**
  - In `AutoBattler.vcxproj`: remove `..\src\AutoBattlerSim.cpp` and `..\src\AutoBattlerSim.h`; add a
    `ClCompile` for each of `..\src\Game\Sim.cpp`, `UnitTypes.cpp`, `SpatialGrid.cpp`, `Steering.cpp`,
    `Combat.cpp` and `Health.cpp`; add a `ClInclude` for `Sim.h`, `SimInternal.h`, `Components.h`,
    `UnitTypes.h` and `UnitInstance.h`.
  - Mirror this in `.vcxproj.filters`, with new filters `Source\Game` and `Headers\Game`.
  - Fix the includes listed under Files.

- [ ] **Step 7: Build and test.**
  - Build Debug and run `--selftest`. Expected: `RESULT: 0 failure(s)`, the same 214 passes as Step 1.
  - Run `grep -rn "AutoBattlerSim" Games/AutoBattler/src Games/AutoBattler/PC_VS2019`. Expected: no output.

- [ ] **Step 8: Commit.**

```bash
git add -A Games/AutoBattler/src Games/AutoBattler/PC_VS2019
git commit -m "Split the AutoBattler sim into Game/ modules

Components, unit types, spatial grid, steering, combat, health and the
sim core each get their own file behind the unchanged sim API. No
behaviour change: self test 214/214.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Move the app, render helpers and self test into `App/`, `App/Render/` and `Tests/`

**Files:**
- Move with `git mv`:
  - `src/AutoBattler.cpp` → `src/App/AutoBattler.cpp`
  - `src/AnimationBake.h/.cpp`, `src/UnitLod.h/.cpp`, `src/UnitCulling.h` → `src/App/Render/`
- Split `src/AutoBattlerSelfTest.cpp` into:
  - `src/Tests/SelfTest.h`: `TestContext`, `writeLine`, `check`, the `CHECK` macro, shared helpers
    (`stepFor`, `runBattle`, `allocRenderData`, `freeRenderData`) and the declaration of each test function;
  - `src/Tests/SelfTest.cpp`: `runAutoBattlerSelfTest` (the runner and its test table) and the
    definitions of `writeLine`, `check` and the shared helpers;
  - `src/Tests/SelfTestSim.cpp`: every test that only uses the sim API or flecs (cell math through nearest
    enemy search, step timings, thread determinism, flecs delete);
  - `src/Tests/SelfTestRender.cpp`: unit instance packing, skin table, baked pose blend, unit culling, LOD
    selection, LOD build.
- Modify: `AutoBattler.vcxproj`, `.vcxproj.filters`, and the include paths in every moved file.

**Interfaces:**
- Produces: `int runAutoBattlerSelfTest(const char* pResultFileName)`, declared in `Game/Sim.h` as today.
  It moves its definition to `Tests/SelfTest.cpp`.
- Test functions keep their names. In `SelfTest.h` they become non-static declarations, for example
  `void testCellMath(TestContext* pCtx);`.

- [ ] **Step 1: Move the files.** Use `git mv` for the five render/app files. Create the four `Tests/`
  files by moving code out of `AutoBattlerSelfTest.cpp` unchanged, except that test functions lose
  `static`. Keep the test table in `runAutoBattlerSelfTest` in exactly today's order, so the report reads
  the same. Then `git rm` the old `AutoBattlerSelfTest.cpp`.

- [ ] **Step 2: Fix includes.**
  - Engine paths: `../../../Common_3` becomes `../../../../Common_3` in `App/` and `Tests/`, and
    `../../../../../Common_3` in `App/Render/`.
  - Game headers: `../Game/Sim.h` from `App/` and `Tests/`, `../../Game/Sim.h` from `App/Render/`.
  - Render headers from `App/AutoBattler.cpp`: `Render/AnimationBake.h`, and likewise for the others.

- [ ] **Step 3: Update the project.** Replace the old paths with the new ones in `AutoBattler.vcxproj` and
  `.filters` (filters `Source\App`, `Source\App\Render`, `Source\Tests` and the matching `Headers\...`).
  The gpu.cfg copy and the shader items stay as they are.

- [ ] **Step 4: Build and run.**
  - Build Debug and Release.
  - Run `--selftest` in both. Expected: 214 passes, 0 failures.
  - Run `--smoke` in Release. Expected: `result=Victory`.
  - Run `git status --short Games/AutoBattler`. Expected: only renames, new files and the project
    changes; no leftover old files.

- [ ] **Step 5: Commit.**

```bash
git add -A Games/AutoBattler/src Games/AutoBattler/PC_VS2019
git commit -m "Move AutoBattler's app, render helpers and self test into App/ and Tests/

No behaviour change: self test 214/214, smoke Victory.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Replace the board and phase rules with a Sandbox GameMode

**Files:**
- Create: `Games/AutoBattler/src/Game/GameMode.h`, `Games/AutoBattler/src/Game/GameMode.cpp`.
- Modify:
  - `Game/Sim.h`, `Game/Sim.cpp`, `Game/SimInternal.h`, `Game/Components.h` and `Game/UnitTypes.h/.cpp`:
    remove the board, phases and rounds, and add the generic world API;
  - `App/AutoBattler.cpp`: run Sandbox; remove the squad UI and picking;
  - `Tests/*`: delete the board and phase tests, rewrite the rest with spawned units, and add the Sandbox
    tests;
  - `AutoBattler.vcxproj` and `.filters`: add the GameMode files.
- Docs: in `Games/AutoBattler/docs/2026-10-13-auto-battler-design.md`, add a line at the top: "Superseded
  by 2026-10-17-conquest-design.md; the board, phases and waves described here were removed in conquest
  plan 1."

**Interfaces:**
- Consumes: the modules from Task 1.
- Produces, in `Game/Sim.h`:

```cpp
static const float MAX_WORLD_SIZE = 1024.0f; // metres; the spatial grid is sized for it

// threadCount: threads the sim may use, the caller's included. unitCapacity: most units alive at once.
BattleSim* simCreate(uint32_t threadCount, uint32_t unitCapacity = MAX_UNITS);
void       simDestroy(BattleSim* pSim);

// Removes every unit and projectile and sets the world to a worldSize square (clamped to MAX_WORLD_SIZE).
void simClear(BattleSim* pSim, float worldSize);
// Adds one unit. Refused (false) at the unit capacity or outside the world.
bool simSpawnUnit(BattleSim* pSim, Team team, UnitType type, float x, float z);
// Advances movement and combat by deltaTime. Ignores NaN and non-positive steps; clamps to MAX_STEP.
void simStep(BattleSim* pSim, float deltaTime);
float simGetTime(const BattleSim* pSim); // seconds simulated since the last simClear
// Units of `team` play the dance clip (a winner celebrating); -1 for none. simClear resets it to -1.
void simSetCelebratingTeam(BattleSim* pSim, int32_t team);
```

  These stay unchanged: `simGetUnitCount`, `simGetProjectileCount`, `simGetUnitCapacity`,
  `simGetWorldSize`, `simFillRenderData`, `simGetTimings`, `simResetTimings`, `simTestNearestEnemySearch`,
  `simTestTotalHp`, `simTestAllPositionsFinite`, `simTestMinUnitDistance`.

- Produces, in `Game/GameMode.h`:

```cpp
#pragma once

#include "Sim.h"

// The game's rules. Sandbox (the only ruleset so far) has no king and no economy: two armies fight until
// one is gone, and the winner celebrates. Campaign rules arrive in conquest plan 3.
struct GameMode
{
    BattleSim* pSim;
    int32_t    mWinner; // Team that won, or -1 while both armies stand (or before a start)
    uint32_t   mUnitsPerTeam;
};

// Clears the world and spawns two facing armies of unitsPerTeam (melee in front, tanks, then ranged) on a
// world sized to hold them. Refused, leaving the sim untouched, if both armies exceed the unit capacity
// or the world would exceed MAX_WORLD_SIZE.
bool gameModeSandboxStart(GameMode* pMode, BattleSim* pSim, uint32_t unitsPerTeam);
// Steps the sim (same dt rules as simStep) and declares the winner once one army is gone.
void    gameModeUpdate(GameMode* pMode, float deltaTime);
int32_t gameModeGetWinner(const GameMode* pMode);
```

- Removed from the public API:
  - functions: `simRestart`, `simStartBattle`, `simStressSetup`, `simCanBuySquad`, `simBuySquad`,
    `simSellSquad`, `simGetCell`, `simGetPhase`, `simGetRound`, `simGetGold`, `simGetBattleTime`,
    `simGetMessage`, `worldToCell`, `cellCenter`, `rayToCell`, `simTestSetCell`, `simTestClearBoard`,
    `simTestSetGold`, `simTestSpawnUnit` (replaced by `simSpawnUnit`), `simTestSetBattleTime`;
  - types and constants: `enum Phase`, `ARENA_CELLS`, `PLAYER_ROWS`, `CELL_SIZE`, `ARENA_SIZE`,
    `START_GOLD`, `BATTLE_TIME_LIMIT`, `RESULT_DURATION`;
  - fields: `UnitStats::mSquadSide`, `UnitStats::mCost`, `Unit::cell`.
- Renamed: the stress layout constants and `stressLayout` move to `GameMode.cpp` as `SANDBOX_SPACING`,
  `SANDBOX_GAP`, `SANDBOX_MARGIN` and `sandboxLayout`.

- [ ] **Step 1: Write the new tests.** Add to `Tests/SelfTestSim.cpp` (and declare them in `SelfTest.h`):

```cpp
void testSpawnRules(TestContext* pCtx)
{
    BattleSim* s = simCreate(1, 4);
    simClear(s, 50.0f);
    CHECK(simGetWorldSize(s) == 50.0f && simGetUnitCount(s, TEAM_PLAYER) == 0u);
    CHECK(simSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, 10.0f, 10.0f));
    CHECK(!simSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, -1.0f, 10.0f)); // outside the world
    CHECK(!simSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, 10.0f, 50.0f));
    CHECK(!simSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, NAN, 10.0f));
    CHECK(simSpawnUnit(s, TEAM_ENEMY, UNIT_TANK, 20.0f, 20.0f));
    CHECK(simSpawnUnit(s, TEAM_ENEMY, UNIT_RANGED, 21.0f, 20.0f));
    CHECK(simSpawnUnit(s, TEAM_ENEMY, UNIT_RANGED, 22.0f, 20.0f));
    CHECK(!simSpawnUnit(s, TEAM_ENEMY, UNIT_RANGED, 23.0f, 20.0f)); // capacity 4 reached
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 1u && simGetUnitCount(s, TEAM_ENEMY) == 3u);
    simClear(s, 2000.0f);
    CHECK(simGetWorldSize(s) == MAX_WORLD_SIZE); // clamped
    CHECK(simGetUnitCount(s, TEAM_PLAYER) + simGetUnitCount(s, TEAM_ENEMY) == 0u);
    simDestroy(s);
}

void testSandboxBattle(TestContext* pCtx)
{
    BattleSim* s = simCreate(1, 1024);
    GameMode   mode = {};
    CHECK(!gameModeSandboxStart(&mode, s, 513)); // 1,026 units: over capacity
    CHECK(simGetUnitCount(s, TEAM_PLAYER) + simGetUnitCount(s, TEAM_ENEMY) == 0u);
    CHECK(gameModeSandboxStart(&mode, s, 200));
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 200u && simGetUnitCount(s, TEAM_ENEMY) == 200u);
    CHECK(gameModeGetWinner(&mode) == -1);

    // Bad steps change nothing and never decide the battle.
    gameModeUpdate(&mode, NAN);
    gameModeUpdate(&mode, -1.0f);
    gameModeUpdate(&mode, 0.0f);
    CHECK(simGetTime(s) == 0.0f && gameModeGetWinner(&mode) == -1);

    for (int32_t i = 0; i < 60 * 120 && gameModeGetWinner(&mode) == -1; ++i)
        gameModeUpdate(&mode, 1.0f / 60.0f);
    const int32_t winner = gameModeGetWinner(&mode);
    CHECK(winner == TEAM_PLAYER || winner == TEAM_ENEMY);
    CHECK(simGetUnitCount(s, (Team)(1 - winner)) == 0u && simGetUnitCount(s, (Team)winner) > 0u);

    // The winners dance.
    RenderData data = allocRenderData(1024);
    simFillRenderData(s, &data);
    bool dancing = data.mUnitCount > 0u;
    for (uint32_t i = 0; i < data.mUnitCount; ++i)
        dancing = dancing && data.pUnitAnims[i].mClip == ANIM_DANCE;
    CHECK(dancing);

    // Restarting mid-battle clears the units, projectiles, winner and celebration.
    CHECK(gameModeSandboxStart(&mode, s, 100));
    gameModeUpdate(&mode, 1.0f / 60.0f);
    CHECK(gameModeSandboxStart(&mode, s, 100));
    CHECK(gameModeGetWinner(&mode) == -1 && simGetProjectileCount(s) == 0u && simGetTime(s) == 0.0f);
    CHECK(simGetUnitCount(s, TEAM_PLAYER) == 100u && simGetUnitCount(s, TEAM_ENEMY) == 100u);
    simFillRenderData(s, &data);
    bool nobodyDances = true;
    for (uint32_t i = 0; i < data.mUnitCount; ++i)
        nobodyDances = nobodyDances && data.pUnitAnims[i].mClip != ANIM_DANCE;
    CHECK(nobodyDances);
    freeRenderData(&data);
    simDestroy(s);
}
```

  Register them in the runner table right after `"nearest enemy search"`:
  `{ "spawn rules", testSpawnRules }, { "sandbox battle", testSandboxBattle },`.

- [ ] **Step 2: Build Debug.** Expected: compile errors (`simClear`, `simSpawnUnit`, `GameMode` undefined).
  This is the failing state.

- [ ] **Step 3: Rewrite the old tests that are worth keeping, and delete the rest.**
  - **Delete:** `testCellMath`, `testBuySell`, `testUnitCap`, `testWaveDeterministic`,
    `testStartBattleGuards`, `testMeleeBattleAndNextRound`, `testDefeatAndRestart`, `testTimeout`,
    `testRestartMidBattle`. Remove them from `SelfTest.h` and the runner. They test rules that no longer
    exist; Sandbox and plan 3 bring their own.
  - **Replace the board-based setups with spawns.** `setupMeleeSkirmish` becomes:

```cpp
// A 5x5 player block facing 5x5 and 5x5 enemy blocks 10 m away, on a 80 m world.
void setupMeleeSkirmish(BattleSim* s)
{
    simClear(s, 80.0f);
    for (int32_t j = 0; j < 5; ++j)
        for (int32_t i = 0; i < 5; ++i)
        {
            simSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, 36.0f + (float)i, 30.0f + (float)j);
            simSpawnUnit(s, TEAM_PLAYER, UNIT_MELEE, 41.0f + (float)i, 30.0f + (float)j);
            simSpawnUnit(s, TEAM_ENEMY, UNIT_MELEE, 38.0f + (float)i, 45.0f + (float)j);
        }
}
```

  - **Rewrite the remaining tests:**
    - `testRangedProjectiles`: a 4×4 ranged block at (40, 36) and a 3×3 enemy tank block at (40, 44). Keep
      its checks, replacing `simStartBattle` with nothing and `simGetPhase(s) == PHASE_BATTLE` with
      `simGetUnitCount(s, TEAM_ENEMY) > 0u`.
    - `testCoincidentUnitsSeparate`: drop the `simTestSetCell` lines and add one enemy far away at
      (75, 75), so the two coincident players have nothing to fight.
    - `testDeltaTimeClamp`: `simGetBattleTime` becomes `simGetTime`, and the `simStartBattle` line goes.
    - `runScriptedBattle`: spawn the same composition as before around the arena centre (two melee blocks
      and a ranged block for the player, a melee block and a tank block for the enemy). Run until one team
      is empty or 60 s. The outcome becomes `{ simGetTime(s), simGetUnitCount(...) x2, simTestTotalHp(...) }`.
    - `testRenderDataPrep` (renamed `testRenderDataLayout`): a 5×5 player melee block and a 3×3 enemy tank
      block 40 m apart. Keep its checks, replacing `ARENA_SIZE` with `simGetWorldSize(s)`.
    - `testRenderDataBattle`: the same spawns as `testRangedProjectiles`, stepped 0.75 s. Keep its checks.
    - `testAnimationState`:
      - idle: a unit with no enemy anywhere plays `ANIM_IDLE`;
      - walk: the skirmish's units walk;
      - attack: the in-range block plays `ANIM_ATTACK`;
      - dance: after `simSetCelebratingTeam(s, TEAM_PLAYER)`, every player unit plays `ANIM_DANCE` and
        enemies do not.
      Replace the victory and timeout-loss sections with this celebration check.
    - `testStressSetup` (renamed `testSandboxSetup`): use `gameModeSandboxStart(&mode, s, 4096)` with
      capacity 8192. Keep the world-size, spacing and movement checks. Replace the "restart returns to the
      arena" section with `simClear(s, 80.0f)`, checking that the world is 80 m and empty.
    - `runStressBattle` and `testStepTimings`: `simStressSetup(s, n)` becomes
      `gameModeSandboxStart(&mode, s, n)`, with the stepping done by `gameModeUpdate`.

- [ ] **Step 4: Implement the sim side.**
  - Remove everything listed as removed under Interfaces from `Sim.h`, `Sim.cpp`, `SimInternal.h`,
    `Components.h` (`Unit::cell`) and `UnitTypes` (`mSquadSide`, `mCost`).
  - `BattleSim` loses `mBoard`, `mPhase`, `mRound`, `mGold`, `mResultTime` and `pMessage`. It renames
    `mBattleTime` to `mTime` and gains `int32_t mCelebratingTeam`.
  - `simCreate` sizes the grid for `MAX_WORLD_SIZE` (`maxDim = ceilf(MAX_WORLD_SIZE / GRID_BUCKET_SIZE)`)
    and ends with `simClear(p, 80.0f)` instead of `simRestart`.
  - New functions in `Sim.cpp`:

```cpp
void simClear(BattleSim* p, float worldSize)
{
    ecs_delete_with(p->pWorld, ecs_id(Unit));
    ecs_delete_with(p->pWorld, ecs_id(Projectile));
    p->mProjectileCount = 0;
    p->mTime = 0.0f;
    p->mCelebratingTeam = -1;
    setWorldSize(p, worldSize > 0.0f && worldSize < MAX_WORLD_SIZE ? worldSize : MAX_WORLD_SIZE);
}

bool simSpawnUnit(BattleSim* p, Team team, UnitType type, float x, float z)
{
    // Positive tests so NaN is refused too.
    if (!(x >= 0.0f && x < p->mWorldSize && z >= 0.0f && z < p->mWorldSize))
        return false;
    if (simGetUnitCount(p, TEAM_PLAYER) + simGetUnitCount(p, TEAM_ENEMY) >= p->mUnitCapacity)
        return false;
    spawnUnit(p, team, type, x, z);
    return true;
}

float simGetTime(const BattleSim* p) { return p->mTime; }
void  simSetCelebratingTeam(BattleSim* p, int32_t team) { p->mCelebratingTeam = team; }
```

  - `spawnUnit` drops its `cell` parameter and the `cell` initialiser.
  - `simStep` becomes the old `PHASE_BATTLE` branch without the result checks: advance `mTime`, then the
    timed `buildGrid`, `steerUnits` and `ecs_progress`, then `++mTimings.mSteps`. `advanceAnimationClocks`
    and `enterResult` are deleted.
  - `simFillRenderData` chooses the clip with
    `winner = (int32_t)unit[i].team == p->mCelebratingTeam` instead of the phase test.

  Spawning goes through `simGetUnitCount` per call, which is an O(tables) flecs count. That's fine for
  setup, but the sandbox start below spawns 2 × unitsPerTeam units, so it calls the internal `spawnUnit`
  directly after checking capacity once.

- [ ] **Step 5: Implement `GameMode.cpp`.**

```cpp
#include "GameMode.h"
#include "SimInternal.h"

#include <math.h>

static const float SANDBOX_SPACING = 1.0f; // one metre between soldiers, like a melee block
static const float SANDBOX_GAP = 10.0f;    // between the two armies' front rows
static const float SANDBOX_MARGIN = 5.0f;  // between the armies and the world edge

// unitsPerTeam soldiers per army in rows of cols; returns the world size holding both armies.
static void sandboxLayout(uint32_t unitsPerTeam, uint32_t* pCols, uint32_t* pRows, float* pWorldSize)
{
    // Body moved unchanged from Sim.cpp's stressLayout, with STRESS_* renamed SANDBOX_*.
}

bool gameModeSandboxStart(GameMode* pMode, BattleSim* pSim, uint32_t unitsPerTeam)
{
    uint32_t cols = 0, rows = 0;
    float    worldSize = 0.0f;
    if (unitsPerTeam == 0 || 2ull * unitsPerTeam > simGetUnitCapacity(pSim))
        return false;
    sandboxLayout(unitsPerTeam, &cols, &rows, &worldSize);
    if (worldSize > MAX_WORLD_SIZE)
        return false;

    simClear(pSim, worldSize);
    // Row 0 is each army's front: melee in front, tanks behind them, ranged at the back.
    const float left = 0.5f * (worldSize - (float)cols * SANDBOX_SPACING);
    const float mid = 0.5f * worldSize;
    for (uint32_t k = 0; k < unitsPerTeam; ++k)
    {
        const uint32_t col = k % cols;
        const uint32_t row = k / cols;
        const UnitType type = row * 10 < rows * 6 ? UNIT_MELEE : (row * 10 < rows * 8 ? UNIT_TANK : UNIT_RANGED);
        const float    x = left + ((float)col + 0.5f) * SANDBOX_SPACING;
        const float    depth = 0.5f * SANDBOX_GAP + ((float)row + 0.5f) * SANDBOX_SPACING;
        spawnUnit(pSim, TEAM_PLAYER, type, x, mid - depth);
        spawnUnit(pSim, TEAM_ENEMY, type, x, mid + depth);
    }
    pMode->pSim = pSim;
    pMode->mWinner = -1;
    pMode->mUnitsPerTeam = unitsPerTeam;
    return true;
}

void gameModeUpdate(GameMode* pMode, float deltaTime)
{
    if (!pMode->pSim || !(deltaTime > 0.0f)) // also rejects NaN
        return;
    simStep(pMode->pSim, deltaTime);
    if (pMode->mWinner >= 0)
        return;
    const uint32_t players = simGetUnitCount(pMode->pSim, TEAM_PLAYER);
    const uint32_t enemies = simGetUnitCount(pMode->pSim, TEAM_ENEMY);
    if (players == 0 || enemies == 0)
    {
        pMode->mWinner = players > 0 ? TEAM_PLAYER : TEAM_ENEMY;
        simSetCelebratingTeam(pMode->pSim, pMode->mWinner);
    }
}

int32_t gameModeGetWinner(const GameMode* pMode) { return pMode->mWinner; }
```

  Declare `void spawnUnit(BattleSim* p, Team team, UnitType type, float x, float z);` in `SimInternal.h`
  and drop its `static`. It is the unchecked fast path for modules that already validated the capacity.

  If both armies die in the same step, the winner is `TEAM_ENEMY` and nobody is left to dance. That is
  acceptable for Sandbox and documented in the comment on `mWinner`.

- [ ] **Step 6: Update the app (`App/AutoBattler.cpp`).**
  - Replace the global `BattleSim* pSim` and its usage with `pSim` plus `GameMode gMode = {}`. In `Init`,
    after `simCreate`, call `gameModeSandboxStart(&gMode, pSim, 1400)` (2,800 units, the old smoke size).
  - In `Update`:
    - replace `simStep(pSim, ...)` with `gameModeUpdate(&gMode, ...)`;
    - delete the buy, sell, picking and start-battle handling (`updatePicking`, `CUSTOM_PLACE`,
      `CUSTOM_SELL` and their input bindings, `gStartRequested`);
    - the restart button calls `gameModeSandboxStart(&gMode, pSim, gMode.mUnitsPerTeam)`.
  - GUI: remove the squad radio buttons and Start Battle. Keep Restart and the LOD-colour checkbox.
  - HUD text: replace the round/gold/phase lines with `"Sandbox  Blue %u vs Red %u   %.0f s"`. Once
    `gameModeGetWinner` ≥ 0, add a second line, `"Blue wins"` or `"Red wins"`.
  - Ground uniforms: set `mHoverCell.y = 0` (no hover) every frame, and
    `mArena = float4(simGetWorldSize(pSim), 0.5f * simGetWorldSize(pSim), 0, 0)`. The ground shader
    rewrite is plan 3.
  - `--smoke`:
    - start with `gameModeSandboxStart(&gMode, pSim, 1400)`;
    - finish 120 frames after `gameModeGetWinner` ≥ 0, or at 150 s;
    - write `result=Blue`, `Red` or `Timeout`, keeping the other fields;
    - delete `PHASE_NAMES`.
  - `--stress`: replace `simStressSetup(pSim, n)` with `gameModeSandboxStart(&gMode, pSim, n)`. Stress keeps
    stepping with `gameModeUpdate(&gMode, STRESS_STEP)`.
  - Remove every remaining use of a deleted API. The build fails until none is left.

- [ ] **Step 7: Build and test.**
  - Build Debug and run `--selftest`. Expected: `RESULT: 0 failure(s)`, with the new `spawn rules` and
    `sandbox battle` checks passing.
  - Build Release and run `--selftest`, `--smoke` (expected `result=Blue` or `result=Red`, never
    `Timeout`) and `--stress`. Every row must show `worst_gpu_minus_cpu_tris` = 0, except the known
    one-frame row at 16k zoom, and the 64k zoom frame ms must be within 10% of 2.35.
  - Run the interactive Release build for 10 s and take a screenshot. Expected: two armies clash, the HUD
    shows the Sandbox line, and there are no squad buttons.

- [ ] **Step 8: Commit.**

```bash
git add -A Games/AutoBattler/src Games/AutoBattler/PC_VS2019 Games/AutoBattler/docs
git commit -m "Replace the auto-battler board and phase rules with a Sandbox GameMode

The board, buy/sell, waves, rounds and phases are gone. The sim gains a
generic world API (simClear, simSpawnUnit, simGetTime,
simSetCelebratingTeam) and always runs combat; GameMode owns the
Sandbox ruleset (two facing armies, winner celebrates) used by the
interactive app, --smoke and --stress. Tests for the removed rules are
deleted; the rest now spawn their units directly.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
