import bpy, time, numpy as np
bpy.ops.mesh.primitive_monkey_add(); ob = bpy.context.active_object; ob.modifiers.new("s","SUBSURF").levels = 4
me = ob.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh()
def T(label, f):
    t = time.perf_counter(); r = f(); print(f"PROF {label}: {1000*(time.perf_counter()-t):.0f} ms"); return r
T("calc_loop_triangles", me.calc_loop_triangles)
nl, nt = len(me.loops), len(me.loop_triangles)
tris = np.empty(nt*3, np.int32); T("tris foreach_get", lambda: me.loop_triangles.foreach_get("loops", tris))
T("hash tris", lambda: hash(tris.tobytes()))
vidx = np.empty(nl, np.int32); T("vertex_index", lambda: me.loops.foreach_get("vertex_index", vidx))
co = np.empty(len(me.vertices)*3, np.float32); T("co", lambda: me.vertices.foreach_get("co", co))
n = np.empty(nl*3, np.float32); T("corner_normals", lambda: me.corner_normals.foreach_get("vector", n))
uv = np.empty(nl*2, np.float32); T("uv", lambda: me.uv_layers.active.uv.foreach_get("vector", uv))
T("calc_tangents", lambda: me.calc_tangents())
t3 = np.empty(nl*3, np.float32); T("tangent get", lambda: me.loops.foreach_get("tangent", t3))
T("unique", lambda: np.unique(tris.ravel(), return_inverse=True))
