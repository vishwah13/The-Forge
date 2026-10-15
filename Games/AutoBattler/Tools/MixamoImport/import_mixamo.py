"""Imports a Mixamo character and its animations into The Forge's runtime formats.

    python import_mixamo.py --name paladin --character "Art/Meshes/MixamoChar/Paladin J Nordstrom.fbx" --animations Art/Animation/mixamo

Writes, under the repository's Art folder:
    Meshes/<name>/riggedMesh.bin              skinned mesh (one geometry, joint remaps to the skeleton)
    Animation/<name>/skeleton.ozz             skeleton
    Animation/<name>/animations/<clip>.ozz    one clip per animation FBX, named after the file
    Textures/dds/<name>_D.tex                 diffuse texture, BC7 with mips

Pipeline: Blender (FBX -> glTF, metres, every joint keyed) -> AssetPipeline -pa (skeleton + clips, writes the
skin's joint remaps into the glTF) -> AssetPipeline -pgltf (mesh) -> AssetPipeline -pt (texture). See README.md.
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
ASSET_PIPELINE = os.path.join(REPO, "Common_3", "Tools", "AssetPipeline", "Win64", "x64", "Release", "AssetPipelineCmd.exe")
BLENDER_SCRIPT = os.path.join(HERE, "blender_fbx_to_gltf.py")


def fail(message):
    sys.exit(f"import_mixamo: error: {message}")


def find_blender(explicit):
    if explicit:
        return explicit
    found = shutil.which("blender")
    if found:
        return found
    installs = sorted(glob.glob(r"C:\Program Files\Blender Foundation\Blender *\blender.exe"))
    if installs:
        return installs[-1]
    fail("Blender not found; pass --blender <path to blender.exe>")


def clip_name(path):
    """'Neutral Idle.fbx' -> 'neutral_idle'."""
    stem = os.path.splitext(os.path.basename(path))[0]
    return re.sub(r"[^a-z0-9]+", "_", stem.lower()).strip("_")


def run(command, log_path, step):
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    with open(log_path, encoding="utf-8", errors="replace") as log:
        text = log.read()
    # AssetPipeline logs errors as "ERR|" and Blender prints Python tracebacks; neither always sets the exit code.
    if result.returncode != 0 or "ERR|" in text or "Traceback" in text:
        fail(f"{step} failed, see {log_path}")
    return text


def blender(blender_exe, log_path, step, *args):
    return run([blender_exe, "-b", "--factory-startup", "-P", BLENDER_SCRIPT, "--", *args], log_path, step)


def asset_pipeline(command, input_dir, output_dir, log_path, *flags):
    os.makedirs(output_dir, exist_ok=True)
    # AssetPipeline appends file names straight onto these paths, so both need the trailing separator.
    run([ASSET_PIPELINE, command, "--input", input_dir + os.sep, "--output", output_dir + os.sep, "--force", *flags], log_path,
        f"AssetPipeline {command}")


def check_clip(character_gltf, clip_gltf):
    """Every joint of the character must have translation, rotation and scale tracks with matching rest proportions."""
    with open(character_gltf) as f:
        character = json.load(f)
    with open(clip_gltf) as f:
        clip = json.load(f)
    joints = {character["nodes"][j]["name"]: character["nodes"][j] for j in character["skins"][0]["joints"]}
    paths = {}
    for channel in clip["animations"][0]["channels"]:
        name = clip["nodes"][channel["target"]["node"]]["name"]
        paths.setdefault(name, set()).add(channel["target"]["path"])
    missing = [name for name in joints if paths.get(name) != {"translation", "rotation", "scale"}]
    if missing:
        fail(f"{clip_gltf}: joints without full tracks: {', '.join(missing[:8])}")
    # A clip downloaded for another Mixamo character has the same joint names but other bone lengths.
    clip_hips = next(n for n in clip["nodes"] if n["name"].endswith("Hips"))
    char_hips = next(n for name, n in joints.items() if name.endswith("Hips"))
    clip_y = clip_hips.get("translation", [0, 0, 0])[1]
    char_y = char_hips.get("translation", [0, 0, 0])[1]
    if abs(clip_y - char_y) > 0.01 * abs(char_y):
        print(f"import_mixamo: warning: {os.path.basename(clip_gltf)} hips rest height {clip_y:.3f} m differs from the character's "
              f"{char_y:.3f} m; it was probably downloaded for a different character", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", required=True, help="asset name used for output folders and files, e.g. paladin")
    parser.add_argument("--character", required=True, help="Mixamo character FBX (With Skin)")
    parser.add_argument("--animations", required=True, nargs="+", help="animation FBX files and/or folders of them")
    parser.add_argument("--art", default=os.path.join(REPO, "Art"), help="Art folder to write into (default: the repository's)")
    parser.add_argument("--blender", help="path to blender.exe (default: PATH, then Program Files)")
    parser.add_argument("--keep-work", action="store_true", help="keep the intermediate glTF/log folder and print its path")
    args = parser.parse_args()

    if not re.fullmatch(r"[A-Za-z0-9_]+", args.name):
        fail("--name may only contain letters, digits and underscores")
    if not os.path.isfile(args.character):
        fail(f"character FBX not found: {args.character}")
    clips = []
    for entry in args.animations:
        clips += sorted(glob.glob(os.path.join(entry, "*.fbx"))) if os.path.isdir(entry) else [entry]
    for clip in clips:
        if not os.path.isfile(clip):
            fail(f"animation FBX not found: {clip}")
    if not clips:
        fail("no animation FBX files found")
    names = [clip_name(c) for c in clips]
    if len(set(names)) != len(names):
        fail(f"two animations map to the same clip name: {names}")
    if not os.path.isfile(ASSET_PIPELINE):
        fail(f"{ASSET_PIPELINE} not built; build Common_3/Tools/AssetPipeline/Win64/AssetPipeline.sln (Release|x64)")
    blender_exe = find_blender(args.blender)

    work = tempfile.mkdtemp(prefix=f"mixamo_{args.name}_")
    gltf_dir = os.path.join(work, "gltf")  # character glTF + animations/ subfolder, the layout -pa discovers
    os.makedirs(os.path.join(gltf_dir, "animations"))
    os.makedirs(os.path.join(work, "texture"))
    character_gltf = os.path.join(gltf_dir, f"{args.name}.gltf")
    diffuse_png = os.path.join(work, "texture", f"{args.name}_D.png")
    try:
        print(f"[1/5] Blender: character {os.path.basename(args.character)}")
        log = blender(blender_exe, os.path.join(work, "blender_character.log"), "Blender character export", "character", args.character,
                      character_gltf, diffuse_png)
        print("      " + re.search(r"MIXAMO_IMPORT character (.*)", log).group(1))

        print(f"[2/5] Blender: {len(clips)} animation(s)")
        for clip, name in zip(clips, names):
            clip_gltf = os.path.join(gltf_dir, "animations", f"{name}.gltf")
            log = blender(blender_exe, os.path.join(work, f"blender_{name}.log"), f"Blender export of {clip}", "animation", clip, clip_gltf,
                          name)
            print("      " + re.search(r"MIXAMO_IMPORT animation (.*)", log).group(1))
            check_clip(character_gltf, clip_gltf)

        print("[3/5] AssetPipeline -pa: skeleton and clips")
        ozz_dir = os.path.join(work, "ozz")
        asset_pipeline("-pa", gltf_dir, ozz_dir, os.path.join(work, "pa.log"))
        # -pa writes the skin's joint remaps into a copy of the glTF in its output; -pgltf must read that copy.
        shutil.copyfile(os.path.join(ozz_dir, f"{args.name}.gltf"), character_gltf)

        print("[4/5] AssetPipeline -pgltf: mesh")
        mesh_dir = os.path.join(work, "mesh")
        asset_pipeline("-pgltf", gltf_dir, mesh_dir, os.path.join(work, "pgltf.log"))

        print("[5/5] AssetPipeline -pt: texture")
        texture_dir = os.path.join(work, "tex")
        asset_pipeline("-pt", os.path.join(work, "texture"), texture_dir, os.path.join(work, "pt.log"), "--bc7", "--genmips")

        outputs = {
            os.path.join(mesh_dir, f"{args.name}.bin"): os.path.join(args.art, "Meshes", args.name, "riggedMesh.bin"),
            os.path.join(ozz_dir, "skeleton.ozz"): os.path.join(args.art, "Animation", args.name, "skeleton.ozz"),
            os.path.join(texture_dir, f"{args.name}_D.tex"): os.path.join(args.art, "Textures", "dds", f"{args.name}_D.tex"),
        }
        for name in names:
            outputs[os.path.join(ozz_dir, "animations", f"{name}.ozz")] = os.path.join(args.art, "Animation", args.name, "animations",
                                                                                      f"{name}.ozz")
        for source in outputs:
            if not os.path.isfile(source):
                fail(f"expected output missing: {source} (logs in {work})")
        print("Wrote:")
        for source, destination in outputs.items():
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            shutil.copyfile(source, destination)
            print(f"  {os.path.relpath(destination, args.art)}")
    except SystemExit:
        print(f"import_mixamo: intermediate files kept in {work}", file=sys.stderr)
        raise
    if args.keep_work:
        print(f"Intermediate files: {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
