# Blender side of the Mixamo import: turns one Mixamo FBX into a glTF that The Forge's AssetPipeline
# converts cleanly. Run by import_mixamo.py; by hand:
#   blender -b --factory-startup -P blender_fbx_to_gltf.py -- character <in.fbx> <out.gltf> <diffuse_out.png>
#   blender -b --factory-startup -P blender_fbx_to_gltf.py -- animation <in.fbx> <out.gltf> <clip_name>
#
# Both modes apply the armature's object transform (Mixamo armatures import at 0.01 scale, centimetres),
# so the skeleton's root node is identity and joints are in metres. That matters because AssetPipeline
# gives any node without an animation track an identity transform, not its rest pose.

import sys

import bpy


def import_fbx(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=path)
    armatures = [ob for ob in bpy.data.objects if ob.type == "ARMATURE"]
    if len(armatures) != 1:
        raise RuntimeError(f"{path}: expected one armature, found {len(armatures)}")
    return armatures[0]


def select_only(objects, active):
    bpy.ops.object.mode_set(mode="OBJECT") if bpy.context.object and bpy.context.object.mode != "OBJECT" else None
    for ob in bpy.data.objects:
        ob.select_set(ob in objects)
    bpy.context.view_layer.objects.active = active


def apply_armature_transform(armature):
    """Bakes the armature's object rotation and scale into its bones, children and location keys."""
    scale = armature.scale[0]
    if abs(armature.scale[1] - scale) > 1e-6 or abs(armature.scale[2] - scale) > 1e-6:
        raise RuntimeError("non-uniform armature scale is not supported")
    objects = [armature] + [ob for ob in bpy.data.objects if ob.parent == armature]
    select_only(objects, armature)
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
    # Pose locations are in the bones' own space, which just shrank by `scale`; shrink the keys with it.
    action = armature.animation_data.action if armature.animation_data else None
    if action:
        for fcurve in action.fcurves:
            if fcurve.data_path.endswith(".location"):
                for key in fcurve.keyframe_points:
                    key.co[1] *= scale
                    key.handle_left[1] *= scale
                    key.handle_right[1] *= scale


def export_gltf(path, animations):
    bpy.ops.export_scene.gltf(
        filepath=path,
        export_format="GLTF_SEPARATE",
        use_selection=False,
        export_yup=True,
        export_apply=False,
        export_skins=True,
        export_def_bones=False,
        export_morph=False,
        export_materials="EXPORT" if not animations else "NONE",
        export_animations=animations,
        export_animation_mode="ACTIONS",
        export_force_sampling=True,
        export_optimize_animation_size=False,  # keep constant channels: every joint must have a track
        export_anim_slide_to_zero=True,
        export_reset_pose_bones=True,
    )


def character(fbx_path, gltf_path, diffuse_path):
    armature = import_fbx(fbx_path)
    if armature.animation_data:
        armature.animation_data.action = None  # Mixamo's two-frame bind pose take
    meshes = [ob for ob in bpy.data.objects if ob.type == "MESH" and ob.parent == armature]
    if not meshes:
        raise RuntimeError(f"{fbx_path}: no skinned mesh under the armature")

    # One skinned mesh: the game draws a single geometry with one joint palette.
    if len(meshes) > 1:
        select_only(meshes, meshes[0])
        bpy.ops.object.join()
    mesh = meshes[0]
    apply_armature_transform(armature)

    # Diffuse texture: the image feeding Base Color of the first material.
    diffuse = None
    for slot in mesh.material_slots:
        if slot.material and slot.material.use_nodes:
            for node in slot.material.node_tree.nodes:
                if node.type == "BSDF_PRINCIPLED" and node.inputs["Base Color"].is_linked:
                    source = node.inputs["Base Color"].links[0].from_node
                    if source.type == "TEX_IMAGE" and source.image:
                        diffuse = source.image
                        break
        if diffuse:
            break
    if diffuse is None:
        raise RuntimeError(f"{fbx_path}: no diffuse texture found")
    diffuse.filepath_raw = diffuse_path
    diffuse.file_format = "PNG"
    diffuse.save()

    tris = sum(len(p.vertices) - 2 for p in mesh.data.polygons)
    print(f"MIXAMO_IMPORT character mesh={mesh.name} vertices={len(mesh.data.vertices)} triangles={tris} "
          f"bones={len(armature.data.bones)} diffuse={diffuse.size[0]}x{diffuse.size[1]}")
    export_gltf(gltf_path, animations=False)


def animation(fbx_path, gltf_path, clip_name):
    armature = import_fbx(fbx_path)
    for ob in [ob for ob in bpy.data.objects if ob.type == "MESH"]:
        bpy.data.objects.remove(ob)  # "With Skin" downloads: keep only the skeleton
    action = armature.animation_data.action
    start, end = (int(round(f)) for f in action.frame_range)

    # Key every bone on every frame, so no joint is left without a track.
    select_only([armature], armature)
    bpy.ops.object.mode_set(mode="POSE")
    bpy.ops.pose.select_all(action="SELECT")
    bpy.ops.nla.bake(frame_start=start, frame_end=end, only_selected=False, visual_keying=True,
                     clear_constraints=False, use_current_action=True, bake_types={"POSE"})
    bpy.ops.object.mode_set(mode="OBJECT")

    apply_armature_transform(armature)
    action = armature.animation_data.action
    action.name = clip_name
    fps = bpy.context.scene.render.fps
    print(f"MIXAMO_IMPORT animation clip={clip_name} frames={end - start + 1} fps={fps} "
          f"seconds={(end - start) / fps:.2f} bones={len(armature.data.bones)}")
    export_gltf(gltf_path, animations=True)


args = sys.argv[sys.argv.index("--") + 1:]
if args[0] == "character":
    character(args[1], args[2], args[3])
elif args[0] == "animation":
    animation(args[1], args[2], args[3])
else:
    raise SystemExit(f"unknown mode {args[0]}")
