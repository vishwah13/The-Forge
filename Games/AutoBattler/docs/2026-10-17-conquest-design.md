# AutoBattler Conquest — Design

Date: 2026-10-17

## Intent

Turn AutoBattler from a place-squads-then-watch auto battler into a conquest game. The player is a King.
His army is a loose crowd that surrounds and follows him. He walks it into forts, castles and cities held
by enemy garrisons, captures them, and the gold they pay buys more army, so the army grows with every
capture (an incremental loop). The engine work done so far (GPU-driven skinned crowds, chunked
multi-threaded steering, the spatial grid, the flecs fix) carries over unchanged, and the stress test keeps
measuring it.

What the user chose, in order:
- Direct control: WASD moves the king and the army flows after him. A `PlayerController` handles input.
- No Player class and no physics engine (reasons under Architecture).
- Code organised into folders even though this is a prototype.
- The army is a loose crowd, not formations or squads.
- A GameMode owns the rules.
- The conquest game replaces the old auto-battler mode entirely.
- Opposition is garrisons only: no roaming armies, no rival kings yet.
- Engagement is automatic with a leash around the king, plus a held charge key.
- Recruiting works anywhere, by key or button.
- The map is fixed, with places laid out in rings by a fixed seed.

Out of scope for this spec: the Captain, Ranger, Spearman, Knight, Priest and Siege ram units, roaming
enemy armies, rival kings, recruiting at captured places, building meshes (they come with the character
kit), click-to-march, garrison respawn, saving and loading, sound, and non-Windows project files.

## Architecture

### Folders

```
Games/AutoBattler/src/
  Game/                 headless: no graphics, no input
    Sim.h / Sim.cpp           world setup, step order, public API, timings
    Components.h              flecs components (Position, Unit, Health, Attack, Projectile, GridSlot,
                              King, Follower, Garrison, Place)
    UnitTypes.h / .cpp        unit stats table
    SpatialGrid.h / .cpp      grid build, nearest-enemy search, separation
    Steering.cpp              follow king, leash, charge, garrison posts (multi-threaded chunks)
    Combat.cpp                attacks and projectiles: deals damage through Health
    Health.cpp                hit points: applyDamage, hit flash, death -> per-step deaths list
    GameMode.h / .cpp         rules: gold, capture, income, recruiting, defeat and victory; Campaign or Sandbox
    WorldMap.h / .cpp         fixed ring layout of places
  App/
    AutoBattler.cpp           IApp: init, load, frame loop; wiring only
    PlayerController.h / .cpp input -> king intent and recruit requests; camera follow and zoom
    Render/                   UnitRenderer (buffers, culling, LOD draws), AnimationBake, UnitLod,
                              UnitCulling, UnitInstance, Hud
  Tests/
    SelfTest*.cpp             split by area: sim, game mode, rendering maths
```

Dependencies go one way: App → Game. Nothing in `Game/` includes App or engine graphics headers.
`PlayerController` reaches the sim only through calls such as `simSetKingIntent(move, charge)` and
`gameModeRecruit(type)`. The self test and the `--smoke` autopilot use the same calls, so tests play the
game the way the player does.

### Code style: Orthodox C++

The project follows [Orthodox C++](https://bkaradzic.github.io/posts/orthodoxc++/): C-like C++ that
anyone who reads C can follow, using a C++ feature only where it makes the code simpler. The Forge itself is
written this way, and the existing AutoBattler code already is.

- **Modules are plain structs plus free functions.** Examples are `simCreate(...)` / `simStep(pSim, dt)` and
  `playerControllerUpdate(&controller, ...)`. There are no classes with member functions, constructors or
  inheritance. The one exception is `AutoBattler : IApp`, because The Forge requires it.
- **No virtual functions or interfaces.** GameMode is a concrete module (see Responsibilities), and
  Campaign versus Sandbox is a setting.
- **No exceptions, no RTTI.** Failures return `bool` or NULL and are logged with `LOGF`.
- **No STL containers, strings or streams.** Memory comes from `tf_malloc` / `tf_calloc` / `tf_free`, into
  fixed-capacity arrays sized at creation (unit capacity, `MAX_PLACES`). Text uses `snprintf`.
- **C headers:** `<math.h>`, `<string.h>`, `<stdio.h>`; never `<cmath>`, `<cstring>` and the like.
- **Templates where they make the code simpler:** small generic helpers (for example `clamp`/`min`/`max`
  over types, or a fixed-capacity array of `T`), so one helper replaces near-identical copies. No template
  metaprogramming beyond that.
- **No lambdas or `auto`.** Flecs systems and thread-pool tasks are `static` functions. Types are spelled
  out.
- **Allowed where they make code simpler:** references for in/out parameters, `static_assert` on GPU
  layouts, `inline` helpers in headers, default arguments, The Forge's vectormath types with their
  operators, and a small scoped timer (`ScopedUSec`), whose destructor keeps the timing correct across
  early returns.
- **Language level:** C++14, the toolchain default The Forge builds with. No feature newer than that.

### Why there is no Player class or physics engine

- **No Player class.** The king is a flecs entity like every unit, plus a `King` tag. Player state (gold,
  captured places) lives in GameMode. A Player class would sit between `PlayerController` and the sim
  without owning anything.
- **No physics engine.** The ground is flat and units already push apart with separation steering, which is
  cheaper and more controllable at this unit count than physics bodies. Building colliders (simple circles
  and boxes) can be added in the sim later, using the spatial grid. We would revisit physics only for
  ragdolls, arcing projectiles over terrain, or destructible walls.

### Responsibilities

| Unit | Owns | Does not do |
|---|---|---|
| `Sim` | flecs world, step order, thread pool, intent snapshot, render data fill, timings | rules, input |
| `SpatialGrid` | per-step snapshot of all units by bucket, nearest-enemy search, separation | writes to units |
| `Steering` | where each unit wants to go and its movement, per role | damage, rules |
| `Combat` | attack cooldowns and range, projectiles, calling `applyDamage` | deleting units, rules |
| `Health` | HP, hit flash, death (deletes the unit, records it in the step's deaths list) | deciding what a death means |
| `GameMode` | gold, place ownership, capture, income, recruiting, defeat/victory, Campaign/Sandbox | movement, combat, rendering |
| `WorldMap` | the place layout (positions, tiers, radii, garrison composition) | runtime state |
| `PlayerController` | input → intent and recruit calls; camera follow | rules, sim state |

GameMode's rules run as small flecs systems that it registers: capture, income and the defeat/victory
checks. Each step, they read `Health`'s deaths list.

## Game rules (GameMode)

All numbers are starting points for tuning.

- **Start:** the king stands at the map centre with 30 swordsmen and 50 gold.
- **Map:** 1 km square. Places sit in three rings, laid out from a fixed seed:

| Place | Ring radius | Count | Capture radius | Garrison | Income |
|---|---|---|---|---|---|
| Fort | ~150 m | 6 | 15 m | ~40 | 2 gold/s |
| Castle | ~300 m | 6 | 25 m | ~300 | 6 gold/s |
| City | ~450 m | 4 | 40 m | ~1,500 | 15 gold/s |

  Garrisons mix swordsmen, archers and shieldbearers. All 16 places together hold about 8,000 defenders.
- **Capture:** a place becomes the player's once its garrison is dead and the king stands inside its
  capture radius. Garrisons do not respawn.
- **Income:** each owned place adds its income every second.
- **Recruiting:** keys 1, 2, 3 (or HUD buttons) buy 10 swordsmen, archers or shieldbearers for 50, 70 or
  100 gold. They spawn at the back of the army (behind the king's facing, just outside the army radius) and
  follow. A recruit is refused, with a HUD message, when gold is short or the unit cap would be exceeded.
- **Defeat:** the king dies. The HUD shows "Your king has fallen. Places captured: N/16" with Restart.
- **Victory:** all 16 places are captured. The HUD shows "Victory!" with Restart.
- **Sandbox:** no king and no rules. `--stress` uses it to spawn two armies and run combat only.
- **Unit cap:** `MAX_UNITS` rises from 4,096 to 16,384 (stress capacity is unchanged).

### Units in this spec

| Unit | Role | From |
|---|---|---|
| King | 2,000 HP, melee hit 40, 4 m/s, scale 1.4, gold tint; moved only by the player | new |
| Swordsman | all-round melee | today's Melee |
| Archer | ranged, projectiles | today's Ranged |
| Shieldbearer | slow, tough melee | today's Tank |

## King, PlayerController and steering

### King

The king auto-attacks enemies within his melee reach. He moves only by intent: in the intent direction at
his speed, he stops with zero intent, and he stays inside the map. A non-finite intent is ignored. He faces
his movement direction, or his target while standing.

### PlayerController (App)

`PlayerController` is a struct holding the camera follow state and last-frame key state, plus free
functions (`playerControllerInit`, `playerControllerUpdate`). Once per frame:
- WASD or the left stick gives a move vector relative to the screen, normalised to length ≤ 1.
- Space (or the right trigger) held means charge. Keys 1, 2, 3 recruit. The mouse wheel zooms.
- It calls `simSetKingIntent(move, charge)` every frame and `gameModeRecruit(type)` on key press.
- It moves the camera target smoothly towards the king, at the existing fixed pitch with wheel zoom.

### Follower steering (the player's army)

The army radius is R = 2 m + 0.6 m × √(army size); 30 units give about 5 m and 1,000 about 21 m.
- **Follow:** with no target, a unit walks towards the king while it is further than R from him. Inside R
  it only separates.
- **Catch-up:** a unit more than R + 10 m from the king moves at 1.5× its speed.
- **Engage:** it targets the nearest enemy, but only one within the leash L = R + 15 m of the king. It drops
  the target once the target leaves the leash.
- **Charge (held):** L becomes 80 m and the pull towards the king is off.

### Garrison steering

Each defender has a post, a fixed random point inside its place. It targets the nearest enemy within the
place's defence zone (capture radius + 20 m), otherwise walks back to its post. It never leaves the zone.

### Data

Each unit carries a role: `Follower`, or `Garrison { place, postX, postZ }`. Each step the sim copies the
king's position, the intent, the army size and R into a snapshot that all steering threads read. Steering
stays in the chunked thread-pool loop, writing only its own units.

## Rendering and HUD

- **Units:** unchanged pipeline (24-byte instances, shared pose table, GPU culling, LODs). The king uses
  the same placeholder model at 1.4× with the gold tint. Team colours become player blue, enemy red and
  king gold. The vertex shader clamps the colour index to the array, which fixes the open review item.
- **Places:** a coloured ground ring for each capture radius, red while the enemy holds it and blue once
  owned, with a faint fill inside. The ground shader draws the rings from an array of up to 16 places
  (x, z, radius, owner) in the frame uniforms. There are no building meshes in this spec.
- **Ground:** one tiled quad covering the 1 km map. The arena pieces are removed: the enemy-half tint, the
  placement grid and hover cells.
- **Camera:** follows the king, as described under PlayerController.
- **HUD:**
  - Top left: gold with income per second, army size, and places captured out of 16.
  - A window with three recruit buttons (name and cost) and the LOD-colour debug checkbox.
  - A "CHARGE" label while charge is held.
  - The defeat or victory message with a Restart button.
- **Health bars:** damaged units only, as today. The king's bar is always shown and is larger.
- **Removed:** the squad radio buttons, Start Battle, and mouse picking with cell placement.

## Testing

All self tests are headless and drive the game through the public calls.
- **Map:** 16 places at their ring distances (within tolerance), inside the map, non-overlapping, and
  identical across runs.
- **King:** moves at his speed in the intent direction, stops with zero intent, is clamped to the map, and
  ignores a NaN intent.
- **Steering:**
  - followers gather within R of a standing king and follow a moving one;
  - an enemy outside the leash is not targeted and one inside it is;
  - charge extends the leash;
  - garrisons return to their posts and never leave their zone.
- **GameMode:**
  - the start state;
  - recruiting deducts gold, spawns 10 units behind the king, and is refused without gold or room;
  - no capture while a defender lives, a capture once the garrison is dead and the king is inside, and no
    capture while the king is outside;
  - income per owned place;
  - defeat on the king's death and victory when everything is captured;
  - Sandbox ignores the rules.
- **Kept:** the flecs delete test, the rendering maths tests, and the same-result-on-1-or-6-threads test,
  rerun on a conquest scenario.
- **`--smoke`:** an autopilot drives the intent and recruit calls. It walks to the nearest fort, fights,
  captures it and recruits, then writes `AutoBattler_smoke.txt` with places captured and king alive. Pass
  means at least one capture with the king alive.
- **`--stress`:** unchanged, running in Sandbox.

## Build order

Each step leaves the game running and the self test green.
1. Reorganise the files into the folder layout, with no behaviour change. Fix the stale "palette" wording.
2. Remove the old auto-battler rules (prep and battle phases, rounds, waves, the board, buy/sell). Add
   GameMode with Sandbox.
3. WorldMap, places and garrisons.
4. The king, `simSetKingIntent`, `PlayerController` and camera follow.
5. Follower (follow, catch-up, leash, charge) and garrison steering.
6. Capture, income, recruiting, defeat and victory.
7. Ground rings, king tint with the colour-index clamp, and the HUD.
8. The `--smoke` autopilot.
