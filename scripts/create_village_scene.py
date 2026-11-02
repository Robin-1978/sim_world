import bpy
import math
from mathutils import Vector
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "blender"
OUT.mkdir(parents=True, exist_ok=True)


def material(name, color, metallic=0.0, roughness=0.8):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (*color, 1.0)
    mat.metallic = metallic
    mat.roughness = roughness
    return mat


def cube(name, location, scale, mat, bevel=0.0):
    bpy.ops.mesh.primitive_cube_add(location=location)
    obj = bpy.context.object
    obj.name = name
    obj.scale = scale
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if bevel:
        modifier = obj.modifiers.new("soft corners", "BEVEL")
        modifier.width = bevel
        modifier.segments = 2
    obj.data.materials.append(mat)
    return obj


def cylinder(name, location, radius, depth, mat, vertices=12):
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius, depth=depth, location=location)
    obj = bpy.context.object
    obj.name = name
    obj.data.materials.append(mat)
    return obj


def cone(name, location, radius, depth, mat):
    bpy.ops.mesh.primitive_cone_add(vertices=6, radius1=radius, radius2=0.0, depth=depth, location=location)
    obj = bpy.context.object
    obj.name = name
    obj.data.materials.append(mat)
    return obj


def look_at(obj, target):
    obj.rotation_euler = (Vector(target) - obj.location).to_track_quat("-Z", "Y").to_euler()


for obj in list(bpy.data.objects):
    bpy.data.objects.remove(obj, do_unlink=True)

ground = material("salt grass", (0.22, 0.38, 0.24))
water = material("tide water", (0.05, 0.20, 0.34), metallic=0.1, roughness=0.35)
wood = material("weathered wood", (0.34, 0.18, 0.09))
plaster = material("lime plaster", (0.72, 0.63, 0.46))
roof = material("blue tile", (0.10, 0.18, 0.28))
stone = material("dock stone", (0.30, 0.32, 0.31))
gold = material("lantern glow", (0.95, 0.55, 0.12), metallic=0.1, roughness=0.3)
mira_mat = material("Mira marker", (0.78, 0.25, 0.20))
lorn_mat = material("Lorn marker", (0.20, 0.43, 0.78))
eve_mat = material("Eve marker", (0.48, 0.23, 0.65))

cube("village_ground", (0, 0, -0.35), (14, 11, 0.35), ground)
cube("tidal_channel", (9, 0, -0.05), (3.0, 11, 0.18), water)
cube("main_path", (0, 0, 0.03), (8.5, 1.0, 0.08), stone)
cube("dock", (6.2, 0, 0.12), (2.2, 1.2, 0.12), wood)

def house(name, x, y, width, depth):
    cube(name + "_body", (x, y, 1.0), (width, depth, 1.0), plaster, 0.12)
    cone(name + "_roof", (x, y, 2.6), max(width, depth) * 1.35, 1.7, roof)


house("mira_home", -6, -5, 1.8, 1.5)
house("lorn_house", -1, -5, 1.8, 1.5)
house("archive_room", 4, -5, 1.8, 1.5)
house("granary", -5, 3, 2.2, 1.8)
house("archive", 3, 3, 2.2, 1.8)

for x in (-6, -1, 4):
    cylinder("lantern_post", (x, -2.8, 1.3), 0.10, 2.4, wood, 8)
    cylinder("lantern", (x, -2.8, 2.55), 0.24, 0.35, gold, 8)

# Low-poly ferry and three readable NPC markers for the prototype camera.
cube("ferry_hull", (7.8, 0, 0.65), (1.3, 0.55, 0.25), wood, 0.15)
cube("ferry_deck", (7.8, 0, 1.0), (0.9, 0.45, 0.08), plaster)
cylinder("ferry_pole", (7.8, 0, 2.0), 0.07, 2.0, wood, 8)

for name, location, mat in [
    ("npc_mira", (-4.2, 3.0, 0.7), mira_mat),
    ("npc_lorn", (6.0, 0.0, 0.7), lorn_mat),
    ("npc_eve", (3.0, 2.2, 0.7), eve_mat),
]:
    cylinder(name, location, 0.38, 1.4, mat, 10)
    cone(name + "_head", (location[0], location[1], 1.65), 0.38, 0.65, mat)

bpy.ops.object.light_add(type="AREA", location=(0, -2, 14))
key = bpy.context.object
key.name = "sun-softbox"
key.data.energy = 1400
key.data.shape = "DISK"
key.data.size = 12
look_at(key, (0, 0, 0))

bpy.ops.object.camera_add(location=(16, -20, 17))
camera = bpy.context.object
camera.name = "slanted_top_down_camera"
camera.data.lens = 44
look_at(camera, (0, 0, 0))
bpy.context.scene.camera = camera

scene = bpy.context.scene
scene.render.engine = "BLENDER_EEVEE"
scene.render.resolution_x = 1280
scene.render.resolution_y = 720
scene.render.resolution_percentage = 50
scene.world.color = (0.025, 0.045, 0.08)
scene.render.filepath = str(OUT / "village_preview.png")

scene["content_version"] = "village-p0.1"
scene["camera_mode"] = "slanted_top_down"
scene["npcs"] = "mira,lorn,eve"

bpy.ops.wm.save_as_mainfile(filepath=str(OUT / "village.blend"))
bpy.ops.export_scene.gltf(filepath=str(OUT / "village.glb"), export_format="GLB", use_selection=False)
print("Generated", OUT / "village.blend")
print("Generated", OUT / "village.glb")
