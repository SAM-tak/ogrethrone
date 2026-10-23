# Exports a model imported with mmd_tools in Blender to an FBX file the engine reads:
#
#     blender -b model.blend --python tools/mmd-to-fbx.py -- out.fbx
#
# Blender's FBX exporter only writes image textures linked directly to a Principled BSDF, and
# mmd_tools materials route theirs through its own shader group, so each material's base
# texture (mmd_base_tex) is linked to a new Principled BSDF first. Only the armatures and the
# meshes outside the rigid body and joint groups are exported: mmd_tools keeps physics proxies
# as ordinary meshes. Textures are named by file name alone; keep them in the FBX's folder or
# its Textures folder. The .blend file is not saved.
import sys

import bpy

out = sys.argv[sys.argv.index('--') + 1]

for material in bpy.data.materials:
    if not material.use_nodes:
        continue
    nodes = material.node_tree.nodes
    base = nodes.get('mmd_base_tex')
    output = next((node for node in nodes if node.type == 'OUTPUT_MATERIAL'), None)
    if not base or not base.image or not output:
        continue
    bsdf = nodes.new('ShaderNodeBsdfPrincipled')
    material.node_tree.links.new(base.outputs['Color'], bsdf.inputs['Base Color'])
    material.node_tree.links.new(bsdf.outputs['BSDF'], output.inputs['Surface'])


def physics(obj):
    """Whether obj lies under mmd_tools' rigid body or joint group."""
    while obj:
        if obj.name in ('rigidbodies', 'joints') or obj.name.startswith(('rigidbodies.', 'joints.')):
            return True
        obj = obj.parent
    return False


bpy.ops.object.select_all(action='DESELECT')
for obj in bpy.context.view_layer.objects:
    if obj.type in ('ARMATURE', 'MESH') and not physics(obj):
        obj.hide_set(False)
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
bpy.ops.export_scene.fbx(filepath=out, use_selection=True, object_types={'ARMATURE', 'MESH'},
                         add_leaf_bones=False, bake_anim=True, path_mode='STRIP')
print('Exported', out)
