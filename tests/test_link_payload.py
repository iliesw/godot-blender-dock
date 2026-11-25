# Headless check of godot_link.py:
#   blender -b --factory-startup --python tests/test_link_payload.py
import json
import os
import socket
import struct
import sys
import threading
import time

import bpy
import numpy as np

srv = socket.socket()
srv.bind(("127.0.0.1", 0))
srv.listen(1)
port = srv.getsockname()[1]
frames = []
conn = []


def serve():
    c, _ = srv.accept()
    conn.append(c)
    buf = b""
    c.settimeout(0.5)
    while True:
        try:
            d = c.recv(1 << 20)
        except socket.timeout:
            continue
        except OSError:
            return
        if not d:
            return
        buf += d
        while len(buf) >= 8:
            fl, hl = struct.unpack_from("<II", buf)
            if len(buf) < 4 + fl:
                break
            frames.append((json.loads(buf[8:8 + hl]), buf[8 + hl:4 + fl]))
            buf = buf[4 + fl:]


threading.Thread(target=serve, daemon=True).start()


def send(h):
    b = json.dumps(h).encode()
    conn[0].sendall(struct.pack("<II", 4 + len(b), len(b)) + b)


# --- scene: collection + parenting + modifiers + vertex colors + normal-mapped material + collision + GN instances
coll = bpy.data.collections.new("Props")
bpy.context.scene.collection.children.link(coll)
bpy.ops.mesh.primitive_monkey_add(location=(1, 2, 3))
monkey = bpy.context.active_object
monkey.modifiers.new("sub", "SUBSURF")
monkey.data.color_attributes.new("Col", "FLOAT_COLOR", "CORNER")
bpy.ops.mesh.primitive_cube_add(location=(0, 0, 5))
child = bpy.context.active_object
child.name = "Crate-col"
child.parent = monkey
child.matrix_parent_inverse = monkey.matrix_world.inverted()
for o in (monkey, child):
    for c in o.users_collection:
        c.objects.unlink(o)
    coll.objects.link(o)

mat = bpy.data.materials.new("Rock")
mat.use_nodes = True
nt = mat.node_tree
img = bpy.data.images.new("rock_n", 8, 8)
tex = nt.nodes.new("ShaderNodeTexImage")
tex.image = img
nmap = nt.nodes.new("ShaderNodeNormalMap")
nt.links.new(tex.outputs["Color"], nmap.inputs["Color"])
nt.links.new(nmap.outputs["Normal"], nt.nodes["Principled BSDF"].inputs["Normal"])
monkey.data.materials.append(mat)

bpy.ops.mesh.primitive_plane_add(size=4, location=(-4, 0, 0))
plane = bpy.context.active_object
plane.name = "Scatter"
gn = plane.modifiers.new("scatter", "NODES")
tree = bpy.data.node_groups.new("Scatter", "GeometryNodeTree")
tree.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
tree.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
gi, go = tree.nodes.new("NodeGroupInput"), tree.nodes.new("NodeGroupOutput")
pts = tree.nodes.new("GeometryNodeMeshToPoints")
inst = tree.nodes.new("GeometryNodeInstanceOnPoints")
ico = tree.nodes.new("GeometryNodeMeshIcoSphere")
ico.inputs["Radius"].default_value = 0.2
tree.links.new(gi.outputs[0], pts.inputs["Mesh"])
tree.links.new(pts.outputs[0], inst.inputs["Points"])
tree.links.new(ico.outputs["Mesh"], inst.inputs["Instance"])
tree.links.new(inst.outputs[0], go.inputs[0])
gn.node_group = tree

sys.argv += ["--", "--godot-port", str(port), "--godot-token", "t0ken"]
ns = {"__name__": "godot_link"}
src = os.path.join(os.path.dirname(__file__), "..", "demo", "addons", "blender_dock", "blender", "godot_link.py")
exec(open(src).read(), ns)
ns["_tick"]()
time.sleep(0.3)
ns["_tick"]()
time.sleep(0.5)

errors = []


def check(cond, msg):
    if not cond:
        errors.append(msg)


kinds = {}
for h, blob in frames:
    kinds[h["t"]] = kinds.get(h["t"], 0) + 1
    if h["t"] == "mesh":
        for s in h["surfaces"]:
            check(s["off"] + s["len"] <= len(blob), "surface out of blob " + h["key"])
            nv = s["nv"]
            off = s["off"] + nv * 24 + (nv * 16 if s["tan"] else 0)
            for k, w in (("uv", 8), ("uv2", 8), ("col", 16)):
                if s.get(k):
                    off += nv * w
            if not h.get("pos_only"):
                idx = np.frombuffer(blob, np.int32, s["ni"], off)
                check(idx.max() < nv, "bad indices " + h["key"])
        print("MESH", h["key"], [(s["nv"], s["ni"] // 3, s.get("mat"), s["tan"], s.get("col")) for s in h["surfaces"]])
    elif h["t"] == "obj":
        print("OBJ", h["name"], h["kind"], "parent=", h["parent"], "col=", h.get("col"), h.get("colonly"))
    elif h["t"] == "inst":
        print("INST", h["key"], h["count"], len(blob) // 48)
        check(len(blob) == h["count"] * 48, "instance blob size")
    elif h["t"] == "mat":
        print("MAT", h["name"], "normal_tex=", bool(h.get("normal_tex")))
    elif h["t"] in ("coll", "scene", "sel"):
        print(h["t"].upper(), {k: v for k, v in h.items() if k != "t"})

objs = {h["name"]: h for h, _ in frames if h["t"] == "obj"}
check(objs["Suzanne"]["parent"] == "c:Props", "monkey should be in Props collection")
check(objs["Crate-col"]["parent"] == "o:Suzanne", "crate should be parented to monkey")
check(objs["Crate-col"]["col"] == "trimesh", "crate collision from suffix")
check(any(h["t"] == "inst" and h["count"] == 4 for h, _ in frames), "4 GN instances on plane")
check(any(h["t"] == "mat" and h["name"] == "Rock" and h.get("normal_tex") for h, _ in frames), "normal map traced")

frames0 = list(frames)
# --- move a vertex: should come back as a position-only update
frames.clear()
monkey.data.vertices[0].co.x += 0.5
monkey.data.update()
bpy.context.view_layer.update()
ns["_on_depsgraph"](bpy.context.scene, bpy.context.evaluated_depsgraph_get())
ns["L"].dirty_geo.add("Suzanne")
ns["_tick"]()
time.sleep(0.3)
upd = [h for h, _ in frames if h["t"] == "mesh" and h["key"] == "Suzanne"]
check(upd and upd[0]["pos_only"], "vertex move should be pos_only")

# --- Godot -> Blender commands (first frame must prove the token)
check(any(h["t"] == "hello" and h.get("token") == "t0ken" for h, _ in frames0), "hello carries the token")
send({"t": "sync", "token": "t0ken"})
send({"t": "sel", "names": ["Crate-col"], "active": "Crate-col"})
send({"t": "xform", "name": "Crate-col", "xform": [1, 0, 0, 0, 1, 0, 0, 0, 1, 7, 8, 9]})
time.sleep(0.2)
ns["_tick"]()
check(child.select_get() and not monkey.select_get(), "selection applied")
check(abs(child.matrix_world.translation.x - 7) < 1e-4 and abs(child.matrix_world.translation.z - 8) < 1e-4
      and abs(child.matrix_world.translation.y + 9) < 1e-4, "xform applied in Blender space")

print("FRAMES", kinds)
print("RESULT", "OK" if not errors else "FAIL: " + "; ".join(errors))
