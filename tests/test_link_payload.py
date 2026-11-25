# Headless check of godot_link.py: blender -b --factory-startup --python tests/test_link_payload.py
import socket, struct, threading, json, sys, os, time
import bpy

srv = socket.socket(); srv.bind(("127.0.0.1", 0)); srv.listen(1)
port = srv.getsockname()[1]
frames = []
def serve():
    c, _ = srv.accept(); buf = b""
    c.settimeout(3)
    try:
        while True:
            d = c.recv(1 << 20)
            if not d: break
            buf += d
            while len(buf) >= 8:
                fl, hl = struct.unpack_from("<II", buf)
                if len(buf) < 4 + fl: break
                frames.append((json.loads(buf[8:8+hl]), buf[8+hl:4+fl])); buf = buf[4+fl:]
    except socket.timeout:
        pass
threading.Thread(target=serve, daemon=True).start()

sys.argv += ["--", "--godot-port", str(port)]
bpy.ops.mesh.primitive_monkey_add(location=(1, 2, 3))
bpy.context.active_object.modifiers.new("sub", "SUBSURF")
ns = {"__name__": "godot_link"}
exec(open(os.path.join(os.path.dirname(__file__), "..", "demo", "addons", "blender_dock", "blender", "godot_link.py")).read(), ns)
ns["_tick"]()
time.sleep(1.0)

import numpy as np
kinds = {}
for h, blob in frames:
    kinds[h["t"]] = kinds.get(h["t"], 0) + 1
    if h["t"] == "mesh":
        for s in h["surfaces"]:
            nv, ni, off = s["nv"], s["ni"], s["off"]
            pos = np.frombuffer(blob, np.float32, nv * 3, off).reshape(-1, 3)
            idx = np.frombuffer(blob, np.int32, ni, off + nv * (24 + (8 if s["uv"] else 0)))
            assert idx.max() < nv and idx.min() >= 0, "bad indices"
            print("MESH", h["key"], "nv", nv, "tris", ni // 3, "uv", s["uv"], "mat", repr(s["mat"]), "y-range", pos[:, 1].min().round(2), pos[:, 1].max().round(2))
    if h["t"] == "obj":
        print("OBJ", h["name"], h["kind"], "origin", [round(v, 2) for v in h["xform"][9:]])
print("FRAMES", kinds)
