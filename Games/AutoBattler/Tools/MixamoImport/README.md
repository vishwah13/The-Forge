# Mixamo import

Turns a Mixamo character and its animations into The Forge runtime assets with one command:
skinned mesh (`.bin`), skeleton and clips (`.ozz`), and diffuse texture (`.tex`). Nothing is
retargeted at runtime: Mixamo fits every animation to the character you upload, so the clips play
directly on its skeleton.

## One-time setup

1. **Blender 4.x** (tested with 4.1). Found automatically in `C:\Program Files\Blender Foundation`
   or on `PATH`, otherwise pass `--blender "path\to\blender.exe"`.
2. **Python 3** on `PATH` (tested with 3.12). Standard library only.
3. **AssetPipeline**: build `Common_3/Tools/AssetPipeline/Win64/AssetPipeline.sln`, configuration
   `Release|x64`. The tool expects
   `Common_3/Tools/AssetPipeline/Win64/x64/Release/AssetPipelineCmd.exe`.

## Getting assets from Mixamo

1. Pick or upload a character on [mixamo.com](https://www.mixamo.com).
2. Download the **character**: Format `FBX Binary (.fbx)`, Skin `With Skin`, pose `T-pose`.
3. With the **same character still selected**, download each **animation**: Format `FBX Binary (.fbx)`,
   Skin `Without Skin`, Frames per Second `30`. For walk/run cycles tick **In Place**: the game
   moves units itself.
4. Name the animation files after the clip you want (`Walk.fbx` becomes clip `walk`), and keep the
   animations of one character together in one folder.

Animations downloaded for a *different* character share the joint names but not the bone lengths.
The tool warns when the hips rest height differs.

## Running it

From the repository root:

```
python Games/AutoBattler/Tools/MixamoImport/import_mixamo.py ^
    --name paladin ^
    --character "Art/Meshes/MixamoChar/Paladin J Nordstrom.fbx" ^
    --animations Art/Animation/mixamo
```

- `--name`: asset name for the output folders and files (letters, digits, underscores).
- `--character`: the With Skin FBX.
- `--animations`: one or more FBX files and/or folders containing them.
- `--art`: Art folder to write into (default: the repository's `Art`).
- `--keep-work`: keep the intermediate glTF files and logs, for debugging.

It takes about 10 s for a character and three clips. Running it again overwrites the outputs, so
adding a clip means dropping the FBX into the folder and re-running.

## Output

```
Art/Meshes/<name>/riggedMesh.bin                 one skinned geometry, joint remaps to skeleton.ozz
Art/Animation/<name>/skeleton.ozz
Art/Animation/<name>/animations/<clip>.ozz       one per animation FBX ("Neutral Idle.fbx" -> neutral_idle.ozz)
Art/Textures/dds/<name>_D.tex                    diffuse, BC7 with mipmaps
```

Load them like the Stormtrooper's: `Rig::Initialize(RD_ANIMATIONS, "<name>/skeleton.ozz")`, the mesh
as `"<name>/riggedMesh.bin"` with `GEOMETRY_LOAD_FLAG_SHADOWED`, clips as
`"<name>/animations/<clip>.ozz"`, and the texture as `"<name>_D.tex"`. Units are metres (a Mixamo
character is about 1.8 m tall), so the per-model values in the game (`UNIT_MODEL_SCALE`, the culling
bounds in `UnitCulling.h`, the health bar height) need to match the new model.

## What it does

1. **Blender** (`blender_fbx_to_gltf.py`, run headless):
   - Character: joins all meshes under the armature into one skinned mesh (the game draws one
     geometry), applies the armature's 0.01 import scale so joints are in metres, saves the Base
     Color texture as PNG, and exports glTF.
   - Each animation: keys every bone on every frame, applies the same scale (rescaling location
     keys to match), and exports glTF with constant channels kept.
2. **Checks** every clip has translation, rotation and scale tracks for every skin joint.
   AssetPipeline gives an untracked joint an identity transform rather than its rest pose, which
   would collapse it.
3. **AssetPipeline `-pa`**: builds `skeleton.ozz` and the clips, and writes the skin's joint remaps
   into a copy of the character glTF.
4. **AssetPipeline `-pgltf`**: converts that copy to the mesh `.bin`.
5. **AssetPipeline `-pt --bc7 --genmips`**: converts the texture.
6. Copies the results into `Art`.

## Troubleshooting

Each step's log is kept in the intermediate folder named in the error message.

- `Blender not found`: pass `--blender`.
- `AssetPipelineCmd.exe not built`: build the AssetPipeline solution (Release|x64).
- `joints without full tracks`: the animation FBX lacks bones the character has (a different rig,
  or not downloaded from Mixamo). Download it again for this character.
- `expected one armature`: the FBX has no skeleton or several. Use Mixamo's FBX export.
- `no diffuse texture found`: the material has no image on Base Color. Re-download With Skin.
