import bpy, os, math

OUT_GLB = r"E:/Gudyi/WORK5/3ds-ai/app/banner/banner_model.glb"
OUT_PNG = r"E:/Gudyi/WORK5/3ds-ai/app/banner/banner_preview.png"
os.makedirs(os.path.dirname(OUT_GLB), exist_ok=True)

bpy.ops.wm.read_factory_settings(use_empty=True)


def new_mat(name, rgb, rough=0.9, metal=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    ptr = None
    for n in m.node_tree.nodes:
        if n.type == "BSDF_PRINCIPLED":
            ptr = n
            break
    if ptr is None:
        ptr = m.node_tree.nodes.new("ShaderNodeBsdfPrincipled")
    ptr.inputs["Base Color"].default_value = (rgb[0], rgb[1], rgb[2], 1.0)
    ptr.inputs["Roughness"].default_value = rough
    ptr.inputs["Metallic"].default_value = metal
    return m


teal = new_mat("teal", (0.11, 0.62, 0.46))   # AIConsole teal
light = new_mat("light", (0.85, 0.98, 0.94))  # bright front panel / AI text


def active(o):
    bpy.context.view_layer.objects.active = o


# --- bubble body: rounded box (speech bubble) ---
bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0, 0, 0))
body = bpy.context.object
body.name = "bubble"
body.scale = (1.6, 0.45, 1.15)
active(body)
bpy.ops.object.modifier_add(type="BEVEL")
mod = body.modifiers[0]
mod.width = 0.14
mod.segments = 4
mod.limit_method = "ANGLE"
bpy.ops.object.modifier_apply(modifier=mod.name)
body.data.materials.append(teal)

# --- tail: speech-bubble tail pointing down-left ---
bpy.ops.mesh.primitive_cone_add(vertices=4, radius1=0.22, radius2=0.0,
                                depth=0.5, location=(-0.45, -0.12, -0.68))
tail = bpy.context.object
tail.name = "tail"
tail.rotation_euler = (math.radians(180), 0.0, math.radians(15))
active(tail)
bpy.ops.object.modifier_add(type="BEVEL")
mod = tail.modifiers[0]
mod.width = 0.04
bpy.ops.object.modifier_apply(modifier=mod.name)
tail.data.materials.append(teal)

# --- AI text on the front (+Z) face ---
curve = bpy.data.curves.new("ai_curve", "FONT")
curve.body = "AI"
curve.extrude = 0.04
curve.bevel_depth = 0.01
txt = bpy.data.objects.new("ai_text", curve)
bpy.context.collection.objects.link(txt)
txt.scale = (0.55, 0.55, 1.0)
txt.location = (0.0, -0.24, 0.02)
txt.rotation_euler = (math.radians(90), 0.0, 0.0)
txt.data.materials.append(light)
active(txt)
bpy.ops.object.convert(target="MESH")

# bake transforms so exported meshes are clean (still rigid)
for nm in ("bubble", "tail", "ai_text"):
    o = bpy.data.objects[nm]
    active(o)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

# --- hierarchy: COMMON -> world -> meshes (pycgfx auto-names the CMDL COMMON) ---
bpy.ops.object.empty_add(type="PLAIN_AXES", location=(0, 0, 0))
common = bpy.context.object
common.name = "COMMON"
bpy.ops.object.empty_add(type="PLAIN_AXES", location=(0, 0, 0))
world = bpy.context.object
world.name = "world"
world.parent = common
for nm in ("bubble", "tail", "ai_text"):
    bpy.data.objects[nm].parent = world

# --- preview render (camera + sun) so we can SEE the model ---
bpy.ops.object.camera_add(location=(2.0, -3.4, 1.3))
cam = bpy.context.object
cam.name = "preview_cam"
cam.data.lens = 40
track = cam.constraints.new("TRACK_TO")
track.target = world
track.track_axis = "TRACK_NEGATIVE_Z"
track.up_axis = "UP_Y"
bpy.ops.object.light_add(type="SUN", location=(3, 3, 5))
bpy.context.object.name = "preview_sun"
scene = bpy.context.scene
scene.camera = cam
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = 48
scene.render.resolution_x = 600
scene.render.resolution_y = 600
scene.render.resolution_percentage = 100
scene.render.filepath = OUT_PNG
scene.render.image_settings.file_format = "PNG"
bpy.ops.render.render(write_still=True)
print("RENDERED", OUT_PNG, os.path.getsize(OUT_PNG) if os.path.exists(OUT_PNG) else "MISSING")

# --- export glTF / GLB ---
bpy.ops.export_scene.gltf(
    filepath=OUT_GLB,
    export_format="GLB",
    export_yup=True,
    use_selection=False,
)
print("EXPORTED", OUT_GLB, os.path.getsize(OUT_GLB) if os.path.exists(OUT_GLB) else "MISSING")
