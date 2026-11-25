"""Godot live link — runs inside Blender (started by the Godot "Blender" dock).

Streams the evaluated scene (meshes with modifiers, materials, lights, cameras,
empties) to the Godot editor over TCP, converted to Godot space:
    Blender (x, y, z) Z-up  ->  Godot (x, z, -y) Y-up, clockwise front faces.

Usable standalone too:
    blender my.blend --python godot_link.py -- --godot-port 6789
"""

import json
import os
import socket
import struct
import sys
import time

import bpy
import numpy as np
from bpy.app.handlers import persistent
from mathutils import Matrix

# --------------------------------------------------------------------------- config

def _arg(name, default):
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if name in argv:
        i = argv.index(name)
        if i + 1 < len(argv):
            return argv[i + 1]
    return default


PORT = int(_arg("--godot-port", os.environ.get("GODOT_LINK_PORT", "6789")))
TICK = 1.0 / 30.0           # max send rate
HEAVY_LOOPS = 250_000       # meshes above this are throttled while editing
HEAVY_INTERVAL = 0.25

# Blender -> Godot axis change, and the extra local fix for lights/cameras
# (Blender points them down local -Z with +Y up, same as Godot, but the axis
# conjugation turns their local frame, so rotate -90deg about X back).
C = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))
C_INV = C.inverted()
LOCAL_FIX = Matrix.Rotation(-1.5707963267948966, 4, "X")

GEOMETRY_TYPES = {"MESH", "CURVE", "SURFACE", "META", "FONT", "CURVES", "POINTCLOUD"}


# --------------------------------------------------------------------------- state

class Link:
    sock = None
    next_connect = 0.0
    rx = b""
    dirty_geo = set()
    dirty_xform = set()
    dirty_mat = set()
    full_sync = True
    last_heavy = {}
    last_scene_names = None
    sent_meshes = {}  # object name -> mesh key


L = Link()


def _frame(header, blob=b""):
    h = json.dumps(header, separators=(",", ":")).encode("utf-8")
    return struct.pack("<II", 4 + len(h) + len(blob), len(h)) + h + blob


def _send(header, blob=b""):
    if not L.sock:
        return False
    try:
        L.sock.sendall(_frame(header, blob))
        return True
    except OSError:
        _disconnect()
        return False


def _disconnect():
    if L.sock:
        try:
            L.sock.close()
        except OSError:
            pass
    L.sock = None
    L.rx = b""


def _try_connect():
    now = time.monotonic()
    if L.sock or now < L.next_connect:
        return
    L.next_connect = now + 1.0
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=0.2)
    except OSError:
        return
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(5.0)
    L.sock = s
    L.full_sync = True
    L.last_scene_names = None
    L.sent_meshes.clear()
    _send({"t": "hello", "file": bpy.data.filepath, "blender": bpy.app.version_string})


def _read_commands():
    if not L.sock:
        return
    try:
        L.sock.setblocking(False)
        while True:
            chunk = L.sock.recv(65536)
            if not chunk:
                _disconnect()
                return
            L.rx += chunk
    except (BlockingIOError, InterruptedError):
        pass
    except OSError:
        _disconnect()
        return
    finally:
        if L.sock:
            L.sock.settimeout(5.0)

    while len(L.rx) >= 8:
        flen, hlen = struct.unpack_from("<II", L.rx, 0)
        if len(L.rx) < 4 + flen:
            break
        header = json.loads(L.rx[8:8 + hlen].decode("utf-8") or "{}")
        L.rx = L.rx[4 + flen:]
        _handle_command(header)


def _handle_command(h):
    t = h.get("t")
    if t == "sync":
        L.full_sync = True
    elif t == "open" and h.get("path"):
        bpy.app.timers.register(lambda p=h["path"]: (bpy.ops.wm.open_mainfile(filepath=p), None)[1], first_interval=0.01)
    elif t == "save":
        if bpy.data.filepath:
            bpy.ops.wm.save_mainfile()
    elif t == "quit":
        bpy.ops.wm.quit_blender()
    elif t == "select":
        ob = bpy.data.objects.get(h.get("name", ""))
        if ob and bpy.context.view_layer.objects.get(ob.name):
            for o in bpy.context.view_layer.objects.selected:
                o.select_set(False)
            ob.select_set(True)
            bpy.context.view_layer.objects.active = ob


# --------------------------------------------------------------------------- conversion

def _xform(mat, fix_local=False):
    m = C @ mat @ C_INV
    if fix_local:
        m = m @ LOCAL_FIX
    # Godot Transform3D: basis columns then origin.
    return [m[0][0], m[1][0], m[2][0],
            m[0][1], m[1][1], m[2][1],
            m[0][2], m[1][2], m[2][2],
            m[0][3], m[1][3], m[2][3]]


def _to_godot_vec(a):
    """(N*3) Blender float array -> (N,3) Godot space float32."""
    a = a.reshape(-1, 3)
    out = np.empty_like(a)
    out[:, 0] = a[:, 0]
    out[:, 1] = a[:, 2]
    out[:, 2] = -a[:, 1]
    return out


def _mesh_payload(ob, depsgraph):
    """Return (header, blob) for the evaluated geometry of ob, or None."""
    ob_eval = ob.evaluated_get(depsgraph)
    try:
        me = ob_eval.to_mesh()
    except RuntimeError:
        return None
    if me is None:
        return None
    try:
        me.calc_loop_triangles()
        nt = len(me.loop_triangles)
        nl = len(me.loops)
        if nt == 0 or nl == 0:
            return {"surfaces": []}, b""

        vidx = np.empty(nl, np.int32)
        me.loops.foreach_get("vertex_index", vidx)
        co = np.empty(len(me.vertices) * 3, np.float32)
        me.vertices.foreach_get("co", co)
        pos = _to_godot_vec(co)[vidx]

        nrm = np.empty(nl * 3, np.float32)
        me.corner_normals.foreach_get("vector", nrm)
        nrm = _to_godot_vec(nrm)

        uv = None
        uvl = me.uv_layers.active
        if uvl is not None:
            uv = np.empty(nl * 2, np.float32)
            uvl.uv.foreach_get("vector", uv)
            uv = uv.reshape(-1, 2)
            uv[:, 1] = 1.0 - uv[:, 1]

        tris = np.empty(nt * 3, np.int32)
        me.loop_triangles.foreach_get("loops", tris)
        tris = tris.reshape(-1, 3)[:, [0, 2, 1]]  # CCW -> CW
        mat_idx = np.empty(nt, np.int32)
        me.loop_triangles.foreach_get("material_index", mat_idx)

        slots = [s.material.name if s.material else "" for s in ob_eval.material_slots]
        surfaces, chunks, off = [], [], 0
        for m in np.unique(mat_idx):
            sel = tris[mat_idx == m]
            used, local = np.unique(sel.ravel(), return_inverse=True)
            parts = [pos[used].tobytes(), nrm[used].tobytes()]
            if uv is not None:
                parts.append(uv[used].tobytes())
            parts.append(local.astype(np.int32).tobytes())
            data = b"".join(parts)
            surfaces.append({
                "mat": slots[m] if 0 <= m < len(slots) else "",
                "nv": int(len(used)), "ni": int(local.size),
                "uv": uv is not None, "off": off,
            })
            chunks.append(data)
            off += len(data)
        return {"surfaces": surfaces, "loops": nl}, b"".join(chunks)
    finally:
        ob_eval.to_mesh_clear()


def _principled(mat):
    if not mat.use_nodes or not mat.node_tree:
        return None
    out = next((n for n in mat.node_tree.nodes if n.type == "OUTPUT_MATERIAL" and n.is_active_output), None)
    if out and out.inputs["Surface"].is_linked:
        n = out.inputs["Surface"].links[0].from_node
        if n.type == "BSDF_PRINCIPLED":
            return n
    return next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)


def _input_value(node, name, default):
    sock = node.inputs.get(name)
    if sock is None:
        return default
    return sock.default_value


def _linked_image(node, name):
    sock = node.inputs.get(name)
    if sock is None or not sock.is_linked:
        return ""
    src = sock.links[0].from_node
    if src.type == "TEX_IMAGE" and src.image and src.image.source == "FILE":
        path = bpy.path.abspath(src.image.filepath, library=src.image.library)
        return os.path.normpath(path) if os.path.exists(path) else ""
    return ""


def _send_material(mat):
    h = {"t": "mat", "name": mat.name, "color": list(mat.diffuse_color), "metallic": mat.metallic,
         "roughness": mat.roughness, "specular": 0.5, "emission": [0, 0, 0], "emission_strength": 0.0,
         "alpha_blend": False, "double_sided": not mat.use_backface_culling, "texture": ""}
    p = _principled(mat)
    if p is not None:
        tex = _linked_image(p, "Base Color")
        col = list(_input_value(p, "Base Color", (0.8, 0.8, 0.8, 1.0)))
        alpha = float(_input_value(p, "Alpha", 1.0))
        h["color"] = [1.0, 1.0, 1.0, alpha] if tex else [col[0], col[1], col[2], alpha]
        h["texture"] = tex
        h["metallic"] = float(_input_value(p, "Metallic", 0.0))
        h["roughness"] = float(_input_value(p, "Roughness", 0.5))
        h["specular"] = float(_input_value(p, "Specular IOR Level", 0.5))
        h["emission"] = list(_input_value(p, "Emission Color", (0, 0, 0, 1)))[:3]
        h["emission_strength"] = float(_input_value(p, "Emission Strength", 0.0))
        h["alpha_blend"] = alpha < 0.999 and getattr(mat, "surface_render_method", "DITHERED") == "BLENDED"
    _send(h)


def _object_header(ob, depsgraph):
    h = {"t": "obj", "name": ob.name, "visible": ob.visible_get(), "kind": "empty"}
    fix = False
    if ob.type in GEOMETRY_TYPES:
        h["kind"] = "mesh"
        h["mesh"] = L.sent_meshes.get(ob.name, ob.name)
        h["shadow"] = getattr(ob.evaluated_get(depsgraph), "visible_shadow", True)
    elif ob.type == "LIGHT":
        li = ob.data
        fix = True
        h["color"] = list(li.color)
        h["shadow"] = li.use_shadow
        if li.type == "SUN":
            h["kind"] = "sun"
            h["energy"] = li.energy  # W/m^2 ~ Godot directional energy
        else:
            # Rough non-physical mapping: 1000 W point light ~ energy 10, range ~ 32 m.
            h["energy"] = li.energy / 100.0
            h["range"] = li.cutoff_distance if getattr(li, "use_custom_distance", False) else max(5.0, li.energy ** 0.5)
            if li.type == "SPOT":
                h["kind"] = "spot"
                h["spot_angle"] = min(89.9, li.spot_size * 57.29577951308232 * 0.5)
                h["spot_attenuation"] = max(0.05, 1.0 - li.spot_blend)
            else:
                h["kind"] = "omni"  # POINT and AREA (approximation)
    elif ob.type == "CAMERA":
        cam = ob.data
        fix = True
        h["kind"] = "camera"
        r = bpy.context.scene.render
        w, hgt = r.resolution_x * r.pixel_aspect_x, r.resolution_y * r.pixel_aspect_y
        horizontal = cam.sensor_fit == "HORIZONTAL" or (cam.sensor_fit == "AUTO" and w >= hgt)
        h["fit"] = "width" if horizontal else "height"
        if cam.type == "ORTHO":
            h["projection"] = "ortho"
            h["size"] = cam.ortho_scale
        else:
            h["projection"] = "persp"
            h["fov"] = (cam.angle_x if horizontal else cam.angle_y) * 57.29577951308232
        h["near"] = cam.clip_start
        h["far"] = cam.clip_end
    h["xform"] = _xform(ob.matrix_world, fix)
    return h


# --------------------------------------------------------------------------- sync

def _scene_objects():
    vl = bpy.context.view_layer
    return [ob for ob in vl.objects if ob.type in GEOMETRY_TYPES | {"LIGHT", "CAMERA", "EMPTY"}]


def _send_geometry(ob, depsgraph, now, force=False):
    if not force:
        last = L.last_heavy.get(ob.name)
        if last is not None and now - last[0] < HEAVY_INTERVAL and last[1] > HEAVY_LOOPS:
            return False  # retry next tick
    res = _mesh_payload(ob, depsgraph)
    if res is None:
        return True
    header, blob = res
    header["t"] = "mesh"
    header["key"] = ob.name
    L.sent_meshes[ob.name] = ob.name
    L.last_heavy[ob.name] = (now, header.pop("loops", 0))
    _send(header, blob)
    return True


def _tick():
    try:
        _try_connect()
        if not L.sock:
            return 0.5
        _read_commands()
        if not L.sock:
            return 0.5

        depsgraph = bpy.context.evaluated_depsgraph_get()
        now = time.monotonic()
        objs = _scene_objects()
        by_name = {ob.name: ob for ob in objs}

        if L.full_sync:
            L.full_sync = False
            for mat in bpy.data.materials:
                _send_material(mat)
            for ob in objs:
                if ob.type in GEOMETRY_TYPES:
                    _send_geometry(ob, depsgraph, now, force=True)
                _send(_object_header(ob, depsgraph))
            L.dirty_geo.clear()
            L.dirty_xform.clear()
            L.dirty_mat.clear()
        else:
            for name in list(L.dirty_mat):
                mat = bpy.data.materials.get(name)
                if mat:
                    _send_material(mat)
            L.dirty_mat.clear()

            for name in list(L.dirty_geo):
                ob = by_name.get(name)
                if ob is None or ob.type not in GEOMETRY_TYPES:
                    L.dirty_geo.discard(name)
                    continue
                if _send_geometry(ob, depsgraph, now):
                    L.dirty_geo.discard(name)
                    L.dirty_xform.add(name)

            for name in list(L.dirty_xform):
                ob = by_name.get(name)
                if ob is not None:
                    _send(_object_header(ob, depsgraph))
            L.dirty_xform.clear()

        names = sorted(by_name)
        if names != L.last_scene_names:
            # New objects (duplicate, add) need a full send; removed ones get dropped by Godot.
            new = set(names) - set(L.last_scene_names or [])
            if L.last_scene_names is not None:
                for n in new:
                    ob = by_name[n]
                    if ob.type in GEOMETRY_TYPES:
                        _send_geometry(ob, depsgraph, now, force=True)
                    _send(_object_header(ob, depsgraph))
            L.last_scene_names = names
            _send({"t": "scene", "objects": names})
    except Exception as e:  # never let the timer die
        print("[godot_link]", type(e).__name__, e)
    return TICK


@persistent
def _on_depsgraph(scene, depsgraph):
    if not L.sock:
        return
    for u in depsgraph.updates:
        idb = u.id
        if isinstance(idb, bpy.types.Object):
            name = idb.original.name if idb.original else idb.name
            if u.is_updated_geometry:
                L.dirty_geo.add(name)
            if u.is_updated_transform or u.is_updated_geometry:
                L.dirty_xform.add(name)
            # Visibility / data changes (light power, camera lens) arrive as plain updates.
            L.dirty_xform.add(name)
        elif isinstance(idb, bpy.types.Material):
            L.dirty_mat.add(idb.original.name if idb.original else idb.name)
        elif isinstance(idb, (bpy.types.Light, bpy.types.Camera)):
            for ob in bpy.context.view_layer.objects:
                if ob.data == idb.original:
                    L.dirty_xform.add(ob.name)


@persistent
def _on_load(_):
    L.full_sync = True
    L.last_scene_names = None
    L.sent_meshes.clear()
    if L.sock:
        _send({"t": "hello", "file": bpy.data.filepath, "blender": bpy.app.version_string})


@persistent
def _on_save(_):
    if L.sock:
        _send({"t": "hello", "file": bpy.data.filepath, "blender": bpy.app.version_string})


def _suppress_splash():
    """Skip the welcome splash for this embedded session only.

    Blender shows the splash right after --python scripts run, so turning the
    preference off here prevents it. It is restored a moment later and the
    preferences are marked clean, so the user's normal Blender keeps its splash.
    """
    if "--godot-keep-splash" in sys.argv:
        return
    prefs = bpy.context.preferences
    if not prefs.view.show_splash:
        return
    was_dirty = prefs.is_dirty
    prefs.view.show_splash = False

    def restore():
        prefs.view.show_splash = True
        prefs.is_dirty = was_dirty
        return None

    bpy.app.timers.register(restore, first_interval=2.0)


def register():
    for lst, fn in ((bpy.app.handlers.depsgraph_update_post, _on_depsgraph),
                    (bpy.app.handlers.load_post, _on_load),
                    (bpy.app.handlers.save_post, _on_save)):
        if fn not in lst:
            lst.append(fn)
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=0.1, persistent=True)
    print(f"[godot_link] streaming to Godot on 127.0.0.1:{PORT}")


_suppress_splash()
register()
