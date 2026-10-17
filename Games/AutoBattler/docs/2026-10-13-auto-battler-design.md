# 40_AutoBattler — Design

Date: 2026-10-13

## Intent

A small, playable top-down auto battler built on The Forge, using only assets already in `Art/`.
Two armies of team-tinted Stormtroopers (up to ~2,000 units total) fight automatically after the
player places squads. The simulation runs on the framework's Flecs ECS so it scales to many entities.

What the user asked for: top-down auto battler, existing art only, simple, built on The Forge,
use the framework's Flecs. Choices made with the user: place-then-watch gameplay, team-tinted
Stormtroopers, three unit types (Melee, Ranged, Tank), ~2,000 units with squad placement.

Out of scope: shop/bench/merging, sound, pathfinding/obstacles, non-Windows project files.

## Where it lives

New unit test sample, modelled on `28_Skinning` (skinned Stormtrooper) and `19_CodeHotReload`
(Flecs usage):

- `Examples_3/Unit_Tests/src/40_AutoBattler/40_AutoBattler.cpp` — `IApp`: renderer resources,
  pipelines, camera, input, UI, drawing.
- `Examples_3/Unit_Tests/src/40_AutoBattler/AutoBattlerSim.h/.cpp` — Flecs world, components,
  systems, spatial grid, game rules. No graphics includes, so it can run headless.
- `Examples_3/Unit_Tests/src/40_AutoBattler/Shaders/FSL/` — unit (skinned, instanced), ground,
  health bar and projectile shaders, `shaders.list`, SRT header.
- `Examples_3/Unit_Tests/src/40_AutoBattler/GPUCfg/gpu.cfg` — copied from 28_Skinning.
- `Examples_3/Unit_Tests/PC_VS2019/40_AutoBattler.vcxproj` (+ `.filters`, `.user`), compiling
  `Common_3/Game/ThirdParty/OpenSource/flecs/flecs.c` like 19_CodeHotReload, added to `Unit_Tests.sln`.

## Assets

| Use | Asset |
|---|---|
| Units | `Meshes/stormtrooper/riggedMesh.bin`, `Animation/stormtrooper/skeleton.ozz`, `Textures/dds/Stormtrooper_D.tex` |
| Victory dance | `Animation/stormtrooper/animations/dance.ozz` |
| Ground | `Textures/dds/PBR/copper_tiles_01/1K/Albedo.tex` |
| Text | existing UI fonts |

## Game rules

**Arena:** 16×16 grid of 5 m cells (80 m square) on the XZ plane. Player owns rows 0–7, enemy rows 8–15.

**Squads and units:**

| Type | Squad | Cost | HP | Damage / cooldown | Range | Speed | Scale |
|---|---|---|---|---|---|---|---|
| Melee | 25 (5×5) | 2 | 100 | 15 / 0.8 s | 1.2 m | 3.0 m/s | 1.0 |
| Ranged | 16 (4×4) | 3 | 60 | 12 / 1.0 s, projectile 15 m/s | 8 m | 2.5 m/s | 0.9 |
| Tank | 9 (3×3) | 4 | 250 | 8 / 1.2 s | 1.4 m | 1.8 m/s | 1.3 |

Unit collision radius = 0.4 m × scale. A squad fills one cell in its formation; a cell holds one squad.

**Phases** (a `GameState` singleton holds phase, round, gold, battle timer, result):

1. **Prep** — player starts with 10 gold. Select a unit type in the UI, left-click an empty cell on
   the player half to buy a squad, right-click a player squad cell to sell it (full refund). The
   enemy wave for this round is already visible. Start Battle is disabled with zero player squads.
2. **Battle** — systems run. Each unit targets the nearest living enemy, moves until in range,
   attacks on cooldown. Melee/Tank hit instantly; Ranged spawn a projectile that homes on its
   target and deals damage on contact (or vanishes if the target died). Units separate from nearby
   units so crowds spread instead of stacking. Units stay inside the arena.
3. **Result** — when one team has no units, survivors dance for 4 s. If 90 s pass first, it's a loss.
   - Victory: round += 1, gold += 5 + round, surviving player squads are restored at full strength
     to their original cells (the army persists between rounds), new enemy wave generated.
   - Defeat: show "Defeat — reached round N" with a Restart button (round 1, 10 gold, empty army).

**Enemy waves:** budget = 6 + 4 × round gold, spent on random squad types in random free enemy cells,
using a seeded RNG (seed = round) so waves are repeatable.

**Cap:** 4,096 live units + 1,024 live projectiles. Buying a squad that would exceed the unit cap
is refused with a UI message; wave generation stops spending at the cap.

## ECS design (Flecs C API)

Setup follows 19_CodeHotReload: `initEntityComponentSystem()`, `ecs_init()`,
`ecs_set_threads(world, max(cores - 1, 1))`, `ECS_COMPONENT_DEFINE`, `ECS_SYSTEM`.

**Components**

- `Position { float x, z; }`
- `Unit { uint8 team; uint8 type; uint16 cell; float radius; float yaw; }`
- `Health { float hp, maxHp, hitFlash; }`
- `Attack { ecs_entity_t target; float timer, retargetTimer; uint32 inRange; }` (damage/range/cooldown come from the unit type table)
- `GridSlot { uint32 slot; }` — the unit's index in this frame's grid snapshot
- `Projectile { ecs_entity_t target; float x, y, z, damage; }`
- Game state (phase, round, gold, timers, board) and the spatial grid live in the `BattleSim` struct passed to systems as context, not as ECS singletons.

**Spatial grid:** 2 m buckets over the arena. Rebuilt each frame into flat arrays (counting sort
by bucket) holding entity id, position, team, radius. Queries read only this snapshot, so systems
can run in parallel without reading other entities' live components. With the snapshot, each team
gets an "enemy rings" field: per bucket, the Chebyshev distance in buckets to the nearest bucket
holding an enemy (two-pass distance transform, one pass over the buckets per team). The nearest-enemy
ring search starts at that ring, since closer rings hold no enemies: the result is identical (same
unit, same tie-breaking), but back-row units no longer scan their own army. On two 4,096-unit armies
this cuts buckets visited per search from ~920 to ~150; at 64k units the sim goes from 11.4 to 3.2 ms
per step (Release). A self test checks every search against brute force.

**Systems** (`ecs_progress` is only called during Battle):

| Phase | System | Threads | Work |
|---|---|---|---|
| (before `ecs_progress`) | `buildGrid()` | 1 | Rebuild grid snapshot, write each unit's `GridSlot` |
| OnUpdate | `SteerSystem` | multi | Retarget (every 0.25 s, or when target died) by searching grid rings outward for nearest enemy; set velocity toward target unless in range; add separation from grid neighbours; clamp to arena; integrate position; update facing |
| OnUpdate | `AttackSystem` | 1 | Tick cooldowns; in range → apply damage to target `Health` (melee) or spawn projectile (ranged). Single-threaded because it writes other entities |
| OnUpdate | `ProjectileSystem` | 1 | Move toward target; on contact apply damage and delete; delete if target gone |
| PostUpdate | `HealthSystem` | 1 | Decay hit flash; `hp <= 0` → `ecs_delete` (deferred, safe) |
| (after `ecs_progress`) | phase check | 1 | Count units per team and switch to Victory/Defeat |

Prep and Result phases still run a render-fill pass so units are drawn when idle.

**Render data:** `FillRenderData(sim, out)` iterates queries and writes plain CPU arrays (unit
instances, health bars for damaged units only, projectile instances). The app copies them to
per-frame GPU buffers. The sim never touches GPU objects.

## Rendering

- **Units:** indexed indirect draws of the unit mesh (a placeholder until the character kit). Per-instance
  structured buffer of 24 bytes (`UnitInstance.h`): x, z, yaw, scale, team + hit flash (packed), and the
  pose (first matrix in the baked table + blend to the next frame, packed). The cull and vertex shaders
  rebuild the world matrix; `unitWorldMatrix()` is the self-tested reference. Team colours are in the
  frame uniforms.
- **GPU culling:** before the draw, `cullclear.comp` resets the indirect draw arguments and
  `cull.comp` (one thread per unit) tests each unit's bounding sphere against the six frustum planes,
  appending visible unit indices to a list and counting them into the draw's instance count. The
  unit vertex shader reads `gUnitInstances[gVisibleUnits[instanceId]]`. `UnitCulling.h` is the CPU
  reference of the same test (self-tested); a pipeline-statistics query on the unit draw reports how
  many units the GPU drew. The cull pass has its own SRT (`Cull.srt.h`) sharing the Persistent and
  PerFrame sets (`SharedSets.h`) with the graphics SRT.
- **LODs:** at load `UnitLod.cpp` builds two simplified index lists over the mesh's own vertices with
  the bundled meshoptimizer (6,518 / 2,432 / 342 triangles), so skinning and the baked animation are
  unchanged. Seams stop `meshopt_simplify` at ~35%, so LOD2 falls back to `meshopt_simplifySloppy`,
  which shades a little darker; acceptable at its distances for this demo. The cull pass picks the
  level from camera distance in bounding radii (`UNIT_LOD_DISTANCES`: 25 and 70) and appends each
  unit to that level's visible list; the app issues one indirect draw per level, binding a
  descriptor set whose `gVisibleUnits` range is that level's list. The stress check compares the
  GPU's drawn triangles with the CPU reference. Visible-list entries carry the level in their top
  two bits, so the UI checkbox "Debug: LOD colours" can tint units green / yellow / red by level.
- **Animation:** `AnimationBake` samples every clip at 30 fps at startup into model-space poses per
  rig joint, stored as 3x4 affine rows (48 B); each clip ends with a copy of its first frame so the
  next frame is always in the same clip. Each mesh uploads a `SkinJoint` table (rig joint + inverse
  bind pose per mesh joint), applied in the vertex shader, so every mesh skinned to the same skeleton
  shares one pose table. LOD0/LOD1 blend the unit's frame with the next; LOD2 reads one frame. The sim
  gives each unit a clip (walk when moving, attack in range, idle in prep, dance for the winning side)
  and its own clock with a per-unit phase offset and speed-scaled walk rate (`UnitAnim`); the app maps
  (clip, time) to a pose offset and blend.
- **Ground:** quad with tiled copper albedo, darker tint on the enemy half, grid lines, highlighted
  hover cell in Prep (green = can place, red = cannot).
- **Health bars:** instanced camera-facing quads above damaged units (green over red).
- **Projectiles:** small yellow camera-facing squares, drawn by the same billboard pipeline as health bars.
- **Camera:** looks down at ~60° over the arena centre. WASD/arrow pan, mouse wheel zoom, clamped
  to the arena.
- **UI:** one window: phase, round, gold, unit counts per team, unit type radio buttons with cost,
  Start Battle / Restart, battle timer, message line. Victory/Defeat banner text.
- **Mouse picking:** ray from cursor through inverse view-projection intersected with ground plane y=0.

## Error handling

- Asset or resource load failure follows existing sample behaviour (framework asserts/logs, `Init` returns false).
- Unit/projectile caps enforced at spawn time; `ASSERT` on instance buffer overflow as a guard.
- Targets are validated with `ecs_is_alive` before use; dead targets trigger retarget.
- Frame `deltaTime` clamped to 1/30 s so a hitch can't tunnel units or skip combat.

## Verification

- `40_AutoBattler.exe --selftest` runs the simulation tests headless (no renderer), writes
  `40_AutoBattler_selftest.txt` and exits with the failure count. Includes a scripted battle
  (2 player Melee squads vs 1 enemy Melee squad, fixed dt 1/60) that must end in victory.
- `40_AutoBattler.exe --smoke` plays a ~2,800-unit battle through the real renderer, writes
  `40_AutoBattler_smoke.txt` (frames, average frame time, result) and quits.
- `AutoBattler.exe --stress` measures the entity ceiling. It runs battles of 4k, 8k, 16k, 32k and
  64k units, two facing armies at the board's 1 m spacing on a square world sized to fit them (the
  80 m arena holds only ~8-10k). Each tier runs twice: with the whole field on screen, and with a
  gameplay camera 60 m from the field centre. The sim steps a fixed 1/60 s per frame, so every run
  measures battle seconds 1-6: after 60 warm-up frames it averages 300 frames and writes
  `AutoBattler_stress.txt` (world size, frame, sim and GPU ms, fps, units the GPU drew, the CPU
  reference count of visible units, the worst per-frame difference between them, units alive). The
  CPU reference count runs only in stress mode and adds a pass over every unit to the frame. The sim's unit storage is a `simCreate` capacity separate from the gameplay
  `MAX_UNITS` (4096), and `simRestart` returns to the 80 m arena.
- Build Debug and Release for PC_VS2019.
- Run manually: place squads, win a round (army persists, gold increases), lose a round (Defeat →
  Restart), fill toward ~2,000 units and confirm it stays interactive, check the log for
  validation errors.
