# Big-mesh update cost: blender -b --factory-startup --python tests/perf_bigmesh.py
import os
import sys
import time

import bpy

sys.argv += ["--", "--godot-port", "1"]
ns = {"__name__": "godot_link"}
src = os.path.join(os.path.dirname(__file__), "..", "demo", "addons", "blender_dock", "blender", "godot_link.py")
exec(open(src).read(), ns)
sent = []
ns["_send"] = lambda h, blob=b"": sent.append((h, len(blob)))

bpy.ops.mesh.primitive_monkey_add()
ob = bpy.context.active_object
ob.modifiers.new("s", "SUBSURF").levels = 4
for label in ("full", "pos_only", "pos_only again"):
    if label != "full":
        ob.data.vertices[0].co.x += 0.01
        ob.data.update()
    dg = bpy.context.evaluated_depsgraph_get()
    ob_eval = ob.evaluated_get(dg)
    me = ob_eval.to_mesh()
    loops = len(me.loops)
    t = time.perf_counter()
    ns["_send_mesh_from"]("Suzanne", me, ["Mat"], 0.0)
    ms = 1000 * (time.perf_counter() - t)
    ob_eval.to_mesh_clear()
    h, n = sent[-1]
    print(f"PERF {label}: {ms:.0f} ms, pos_only={h['pos_only']}, {n / 1e6:.1f} MB, corners={loops}")
