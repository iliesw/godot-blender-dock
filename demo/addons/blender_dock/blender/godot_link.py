"""Godot live link: runs inside Blender (started by the Godot "Blender" dock).

Streams the evaluated scene to the Godot editor over TCP, converted to Godot space
(Blender x, y, z Z-up  ->  Godot x, z, -y Y-up, clockwise front faces):
  * hierarchy: object parenting and collections become nested nodes
  * meshes with modifiers / armature deformation / shape keys, UV, UV2, vertex colors, tangents
  * materials: Principled BSDF incl. base color, normal, roughness, metallic and emission textures
  * lights, cameras, empties; geometry-node and collection instances as MultiMesh
  * collision hints (name suffix -col / -colonly / -convcol, or the Godot panel)
And from Godot: selection, transforms, editor-view follow, save, glTF bake, chrome.

Standalone use:  blender my.blend --python godot_link.py -- --godot-port 6789
"""

import atexit
import ctypes
import hashlib
import json
import math
import os
import socket
import struct
import sys
import tempfile
import threading
import time

import bpy
import numpy as np
from bpy.app.handlers import persistent
from mathutils import Matrix, Quaternion, Vector

# --------------------------------------------------------------------------- config

ARGV = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def _arg(name, default=None):
    if name in ARGV:
        i = ARGV.index(name)
        if i + 1 < len(ARGV):
            return ARGV[i + 1]
    return default


PORT = int(_arg("--godot-port", os.environ.get("GODOT_LINK_PORT", "6789")))
GODOT_PID = int(_arg("--godot-pid", "0"))
PROJECT_DIR = _arg("--godot-project", "")
THEME_BASE = _arg("--godot-base", "")
THEME_ACCENT = _arg("--godot-accent", "")
RECOVERY_PATH = _arg("--godot-recovery", "")
RECOVERY_INTERVAL = float(_arg("--godot-recovery-interval", "30"))
HIDE_STATUSBAR = _arg("--godot-hide-statusbar", "1") == "1"

TICK = 1.0 / 30.0
SLOW_MESH_SECONDS = 0.012   # extraction slower than this -> mesh gets its own adaptive interval

C = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))
C_INV = C.inverted()
LOCAL_FIX = Matrix.Rotation(-math.pi / 2, 4, "X")   # lights / cameras
LOCAL_FIX_INV = LOCAL_FIX.inverted()

GEOMETRY_TYPES = {"MESH", "CURVE", "SURFACE", "META", "FONT", "CURVES", "POINTCLOUD"}
SYNC_TYPES = GEOMETRY_TYPES | {"LIGHT", "CAMERA", "EMPTY", "ARMATURE"}
COL_SUFFIXES = (("-convcolonly", "convex", True), ("-convcol", "convex", False),
                ("-colonly", "trimesh", True), ("-col", "trimesh", False))


# --------------------------------------------------------------------------- state

class Link:
    sock = None
    next_connect = 0.0
    rx = b""
    full_sync = True
    dirty_geo = set()
    dirty_obj = set()
    dirty_mat = set()
    dirty_inst = True
    mesh_topo = {}      # mesh key -> {sig, surfaces[used corner ids]} for position-only updates
    mesh_settle = {}    # mesh key -> time for a full refresh after position-only updates
    mesh_hash = {}      # object name -> hash of last payload (skip identical sends)
    mesh_next = {}      # object name -> earliest next send (adaptive throttling for heavy meshes)
    last_names = None
    last_sel = None
    last_view = None
    remote_xform_until = {}   # object name -> time: ignore echo while Godot drags it
    docked = True
    status = "connecting"
    last_recovery = 0.0
    hwnd = 0
    parent_hwnd = 0
    godot_died = False


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
    L.status = "disconnected"


def _hello():
    _send({"t": "hello", "file": bpy.data.filepath, "blender": bpy.app.version_string})


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
    L.status = "live"
    _reset_sync()
    _hello()


def _reset_sync():
    L.full_sync = True
    L.last_names = None
    L.last_sel = None
    L.mesh_topo.clear()
    L.mesh_settle.clear()
    L.mesh_hash.clear()
    L.mesh_next.clear()


def _ctx_override():
    wm = bpy.context.window_manager
    win = bpy.context.window or (wm.windows[0] if wm.windows else None)
    return bpy.context.temp_override(window=win) if win else None


def _run_op(fn):
    ov = _ctx_override()
    if ov is None:
        return fn()
    with ov:
        return fn()


# --------------------------------------------------------------------------- commands from Godot

def _read_commands():
    if not L.sock:
        return
    try:
        L.sock.setblocking(False)
        while True:
            chunk = L.sock.recv(1 << 16)
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
        try:
            header = json.loads(L.rx[8:8 + hlen].decode("utf-8") or "{}")
        except ValueError:
            header = {}
        L.rx = L.rx[4 + flen:]
        try:
            _handle_command(header)
        except Exception as e:
            print("[godot_link] command failed:", header.get("t"), e)


def _handle_command(h):
    t = h.get("t")
    if t == "sync":
        _reset_sync()
    elif t == "open" and h.get("path"):
        path = h["path"]
        bpy.app.timers.register(lambda: (_run_op(lambda: bpy.ops.wm.open_mainfile(filepath=path)), None)[1],
                                first_interval=0.01)
    elif t == "save":
        _save(h.get("path", ""))
    elif t == "quit":
        _run_op(lambda: bpy.ops.wm.quit_blender())
    elif t == "sel":
        _apply_selection(h.get("names", []), h.get("active", ""))
    elif t == "xform":
        _apply_xform(h.get("name", ""), h.get("xform", []))
    elif t == "view":
        _apply_view(h)
    elif t == "chrome":
        L.docked = bool(h.get("docked", True))
        _apply_chrome()
    elif t == "embedded":
        L.hwnd = int(h.get("hwnd", 0))
        L.parent_hwnd = int(h.get("parent", 0))
    elif t == "bake":
        _bake(h.get("dir", ""))


def _save(path):
    def do():
        if bpy.data.filepath:
            bpy.ops.wm.save_mainfile()
        elif path:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            bpy.ops.wm.save_as_mainfile(filepath=path)
    _run_op(do)
    _hello()


def _apply_selection(names, active):
    vl = bpy.context.view_layer
    want = set(names)
    for ob in vl.objects:
        sel = ob.name in want
        if ob.select_get() != sel:
            ob.select_set(sel)
    if active and active in vl.objects:
        vl.objects.active = vl.objects[active]
    L.last_sel = (sorted(want), active if active in want else "")


def _godot_to_blender_matrix(x, fix):
    m = Matrix(((x[0], x[3], x[6], x[9]),
                (x[1], x[4], x[7], x[10]),
                (x[2], x[5], x[8], x[11]),
                (0, 0, 0, 1)))
    if fix:
        m = m @ LOCAL_FIX_INV
    return C_INV @ m @ C


def _apply_xform(name, x):
    ob = bpy.data.objects.get(name)
    if not ob or len(x) != 12:
        return
    ob.matrix_world = _godot_to_blender_matrix(x, ob.type in {"LIGHT", "CAMERA"})
    L.remote_xform_until[name] = time.monotonic() + 0.3


def _largest_view3d():
    best = None
    for win in bpy.context.window_manager.windows:
        for area in win.screen.areas:
            if area.type == "VIEW_3D" and (best is None or area.width * area.height > best.width * best.height):
                best = area
    return best


def _apply_view(h):
    area = _largest_view3d()
    if not area:
        return
    space = area.spaces.active
    r3d = space.region_3d
    cam = _godot_to_blender_matrix(h["xform"], True)  # Godot camera looks down -Z like a Blender camera
    loc, rot, _ = cam.decompose()
    dist = max(0.1, float(h.get("distance", r3d.view_distance)))
    r3d.view_perspective = "ORTHO" if h.get("ortho") else "PERSP"
    r3d.view_rotation = rot
    r3d.view_distance = dist
    r3d.view_location = loc + rot @ Vector((0, 0, -dist))
    if h.get("ortho"):
        r3d.view_distance = float(h.get("size", 10.0))
    else:
        # Godot: vertical fov. Blender viewport lens is relative to a 72 mm-wide sensor on the long side.
        aspect = max(area.width, 1) / max(area.height, 1)
        vfov = math.radians(float(h.get("fov", 75.0)))
        long_fov = 2 * math.atan(math.tan(vfov / 2) * max(aspect, 1.0))
        space.lens = 36.0 / math.tan(long_fov / 2)
    area.tag_redraw()


def _bake(directory):
    """Export the selection (with animation) as glTF into the Godot project."""
    sel = [ob for ob in bpy.context.view_layer.objects if ob.select_get()]
    if not sel or not directory:
        _send({"t": "baked", "ok": False, "msg": "Select objects in Blender first."})
        return
    roots = {ob for ob in sel}
    for ob in sel:  # include whole rigs: children of selected armatures
        roots.update(ob.children_recursive)
    active = bpy.context.view_layer.objects.active or sel[0]
    name = bpy.path.clean_name(active.name)
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, name + ".glb")

    def do():
        prev = [ob for ob in bpy.context.view_layer.objects if ob.select_get()]
        for ob in bpy.context.view_layer.objects:
            ob.select_set(ob in roots)
        try:
            bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_yup=True,
                                      export_apply=True, export_animations=True, export_skins=True,
                                      export_morph=True, export_cameras=False, export_lights=False)
        finally:
            for ob in bpy.context.view_layer.objects:
                ob.select_set(ob in prev)
    try:
        _run_op(do)
        _send({"t": "baked", "ok": True, "path": path})
    except Exception as e:
        _send({"t": "baked", "ok": False, "msg": str(e)})


# --------------------------------------------------------------------------- conversion

def _to_list12(m):
    return [m[0][0], m[1][0], m[2][0],
            m[0][1], m[1][1], m[2][1],
            m[0][2], m[1][2], m[2][2],
            m[0][3], m[1][3], m[2][3]]


def _godot_world(ob):
    m = C @ ob.matrix_world @ C_INV
    if ob.type in {"LIGHT", "CAMERA"}:
        m = m @ LOCAL_FIX
    return m


def _vec3(a):
    a = a.reshape(-1, 3)
    out = np.empty_like(a)
    out[:, 0] = a[:, 0]
    out[:, 1] = a[:, 2]
    out[:, 2] = -a[:, 1]
    return out


def _collision(ob):
    kind = ob.get("godot_collision", "")
    only = bool(ob.get("godot_collision_only", False))
    if kind in ("trimesh", "convex"):
        return kind, only
    low = ob.name.lower()
    for suffix, k, o in COL_SUFFIXES:
        if low.endswith(suffix):
            return k, o
    return "", False


def _corner_verts(me, nl):
    out = np.empty(nl, np.int32)
    attr = me.attributes.get(".corner_vert")
    if attr is not None:
        attr.data.foreach_get("value", out)  # raw attribute: much faster than MeshLoop.vertex_index
    else:
        me.loops.foreach_get("vertex_index", out)
    return out


def _positions(me, cv):
    co = np.empty(len(me.vertices) * 3, np.float32)
    attr = me.attributes.get("position")
    if attr is not None:
        attr.data.foreach_get("vector", co)
    else:
        me.vertices.foreach_get("co", co)
    return _vec3(co)[cv]


def _normals(me, nl):
    nrm = np.empty(nl * 3, np.float32)
    me.corner_normals.foreach_get("vector", nrm)
    return _vec3(nrm)


def _extract_full(me, cv):
    """Full payload: topology and all attributes. Arrays are per corner (loop)."""
    me.calc_loop_triangles()
    nt, nl = len(me.loop_triangles), len(me.loops)
    if nt == 0 or nl == 0:
        return None
    tris = np.empty(nt * 3, np.int32)
    me.loop_triangles.foreach_get("loops", tris)
    tris = tris.reshape(-1, 3)[:, [0, 2, 1]]  # CCW -> CW
    mat_idx = np.empty(nt, np.int32)
    me.loop_triangles.foreach_get("material_index", mat_idx)

    uv = uv2 = tan = col = None
    layers = list(me.uv_layers)
    if layers:
        act = me.uv_layers.active or layers[0]
        uv = np.empty(nl * 2, np.float32)
        act.uv.foreach_get("vector", uv)
        uv = uv.reshape(-1, 2)
        uv[:, 1] = 1.0 - uv[:, 1]
        others = [l for l in layers if l != act]
        if others:
            uv2 = np.empty(nl * 2, np.float32)
            others[0].uv.foreach_get("vector", uv2)
            uv2 = uv2.reshape(-1, 2)
            uv2[:, 1] = 1.0 - uv2[:, 1]
        try:
            me.calc_tangents(uvmap=act.name)
            t3 = np.empty(nl * 3, np.float32)
            me.loops.foreach_get("tangent", t3)
            sign = np.empty(nl, np.float32)
            me.loops.foreach_get("bitangent_sign", sign)
            tan = np.empty((nl, 4), np.float32)
            tan[:, :3] = _vec3(t3)
            tan[:, 3] = -sign  # V is flipped above
        except RuntimeError:
            tan = None  # n-gons: no tangents (only matters for normal maps)
    ca = me.color_attributes.active_color if hasattr(me, "color_attributes") else None
    if ca is not None:
        raw = np.empty(len(ca.data) * 4, np.float32)
        ca.data.foreach_get("color", raw)
        raw = raw.reshape(-1, 4)
        col = raw if ca.domain == "CORNER" else (raw[cv] if ca.domain == "POINT" else None)
    return {"tris": tris, "mat_idx": mat_idx, "tan": tan, "uv": uv, "uv2": uv2, "col": col}


def _send_mesh_from(key, me, slots, now):
    """Send geometry. When only positions changed (sculpt, edit, armature) send a fast
    position+normal update on the cached topology, then one full refresh once edits settle."""
    t0 = time.perf_counter()
    nl = len(me.loops)
    if nl == 0:
        _send({"t": "mesh", "key": key, "surfaces": []})
        L.mesh_topo.pop(key, None)
        return
    cv = _corner_verts(me, nl)
    sig = (nl, len(me.polygons), len(me.vertices), hash(cv[::7].tobytes()), tuple(slots))
    topo = L.mesh_topo.get(key)
    pos = _positions(me, cv)
    nrm = _normals(me, nl)

    if topo is not None and topo["sig"] == sig:
        surfaces, chunks, off = [], [], 0
        for s in topo["surfaces"]:
            used = s["used"]
            data = pos[used].tobytes() + nrm[used].tobytes()
            surfaces.append({"nv": s["nv"], "ni": s["ni"], "off": off, "len": len(data), "tan": False})
            chunks.append(data)
            off += len(data)
        _send({"t": "mesh", "key": key, "surfaces": surfaces, "pos_only": True}, b"".join(chunks))
        L.mesh_settle[key] = now + 0.6  # full refresh (tangents) when editing pauses
    else:
        d = _extract_full(me, cv)
        if d is None:
            _send({"t": "mesh", "key": key, "surfaces": []})
            L.mesh_topo.pop(key, None)
            return
        surfaces, chunks, off, cache = [], [], 0, []
        for m in np.unique(d["mat_idx"]):
            sel = d["tris"][d["mat_idx"] == m]
            used, local = np.unique(sel.ravel(), return_inverse=True)
            parts = [pos[used].tobytes(), nrm[used].tobytes()]
            if d["tan"] is not None:
                parts.append(d["tan"][used].tobytes())
            for k in ("uv", "uv2", "col"):
                if d[k] is not None:
                    parts.append(d[k][used].tobytes())
            parts.append(local.astype(np.int32).tobytes())
            data = b"".join(parts)
            surfaces.append({"nv": int(len(used)), "ni": int(local.size), "off": off, "len": len(data),
                             "tan": d["tan"] is not None, "mat": slots[m] if 0 <= m < len(slots) else "",
                             "uv": d["uv"] is not None, "uv2": d["uv2"] is not None, "col": d["col"] is not None})
            cache.append({"used": used, "nv": int(len(used)), "ni": int(local.size)})
            chunks.append(data)
            off += len(data)
        blob = b"".join(chunks)
        digest = hashlib.blake2b(blob, digest_size=16).digest()
        L.mesh_topo[key] = {"sig": sig, "surfaces": cache}
        L.mesh_settle.pop(key, None)
        if L.mesh_hash.get(key) != digest:
            L.mesh_hash[key] = digest
            _send({"t": "mesh", "key": key, "surfaces": surfaces, "pos_only": False}, blob)
    cost = time.perf_counter() - t0
    # Heavy meshes: back off so editing stays fluid (about 2x the cost, max 0.5 s).
    L.mesh_next[key] = now + (min(0.5, cost * 2.0) if cost > SLOW_MESH_SECONDS else 0.0)


def _settle_meshes(now):
    for key, when in list(L.mesh_settle.items()):
        if now >= when and now >= L.mesh_next.get(key, 0.0):
            del L.mesh_settle[key]
            L.mesh_topo.pop(key, None)  # forces the full path next time
            if key.startswith("inst:"):
                L.dirty_inst = True
            else:
                L.dirty_geo.add(key)


def _send_object_mesh(ob, depsgraph, now, force=False):
    if not force and now < L.mesh_next.get(ob.name, 0.0):
        return False
    ob_eval = ob.evaluated_get(depsgraph)
    try:
        me = ob_eval.to_mesh()
    except RuntimeError:
        me = None
    try:
        slots = [s.material.name if s.material else "" for s in ob_eval.material_slots]
        if me is None:
            _send({"t": "mesh", "key": ob.name, "surfaces": []})
        else:
            _send_mesh_from(ob.name, me, slots, now)
    finally:
        if me is not None:
            ob_eval.to_mesh_clear()
    return True


# --------------------------------------------------------------------------- materials

def _principled(mat):
    if not mat.use_nodes or not mat.node_tree:
        return None
    nodes = mat.node_tree.nodes
    out = next((n for n in nodes if n.type == "OUTPUT_MATERIAL" and n.is_active_output), None)
    if out and out.inputs["Surface"].is_linked:
        n = out.inputs["Surface"].links[0].from_node
        if n.type == "BSDF_PRINCIPLED":
            return n
    return next((n for n in nodes if n.type == "BSDF_PRINCIPLED"), None)


def _val(node, name, default):
    s = node.inputs.get(name)
    return default if s is None else s.default_value


def _image_path(img):
    if img is None:
        return ""
    if img.source == "FILE" and not img.packed_file:
        p = bpy.path.abspath(img.filepath, library=img.library)
        if os.path.exists(p):
            return os.path.normpath(p)
    # Packed / generated image: write a PNG copy to the temp folder.
    try:
        out = os.path.join(tempfile.gettempdir(), "blender_dock_tex", bpy.path.clean_name(img.name) + ".png")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        cp = img.copy()
        cp.filepath_raw = out
        cp.file_format = "PNG"
        cp.save()
        bpy.data.images.remove(cp)
        return out
    except Exception:
        return ""


def _trace_image(sock):
    """Follow a socket back to an Image Texture (through Normal Map / Separate Color / reroutes)."""
    channel = ""
    for _ in range(6):
        if sock is None or not sock.is_linked:
            return "", channel
        link = sock.links[0]
        n = link.from_node
        if n.type == "TEX_IMAGE":
            if link.from_socket.name == "Alpha":
                channel = channel or "a"
            return _image_path(n.image), channel
        if n.type in ("SEPARATE_COLOR", "SEPRGB", "SEPARATE_RGB"):
            channel = {"Red": "r", "Green": "g", "Blue": "b", "R": "r", "G": "g", "B": "b"}.get(link.from_socket.name, "")
            sock = n.inputs[0]
        elif n.type in ("NORMAL_MAP", "REROUTE", "BUMP"):
            sock = n.inputs.get("Color") or n.inputs[0]
        else:
            return "", channel
    return "", channel


def _send_material(mat):
    h = {"t": "mat", "name": mat.name, "color": list(mat.diffuse_color), "metallic": mat.metallic,
         "roughness": mat.roughness, "specular": 0.5, "emission": [0, 0, 0], "emission_strength": 0.0,
         "alpha_blend": False, "double_sided": not mat.use_backface_culling}
    p = _principled(mat)
    if p is not None:
        base_sock = p.inputs.get("Base Color")
        tex, _ = _trace_image(base_sock)
        vcol = bool(base_sock and base_sock.is_linked and
                    base_sock.links[0].from_node.type in ("VERTEX_COLOR", "ATTRIBUTE"))
        col = list(_val(p, "Base Color", (0.8, 0.8, 0.8, 1.0)))
        alpha = float(_val(p, "Alpha", 1.0))
        h["color"] = [1.0, 1.0, 1.0, alpha] if (tex or vcol) else [col[0], col[1], col[2], alpha]
        h["texture"] = tex
        h["vertex_color"] = vcol
        h["metallic"] = float(_val(p, "Metallic", 0.0))
        h["roughness"] = float(_val(p, "Roughness", 0.5))
        h["specular"] = float(_val(p, "Specular IOR Level", 0.5))
        h["emission"] = list(_val(p, "Emission Color", (0, 0, 0, 1)))[:3]
        h["emission_strength"] = float(_val(p, "Emission Strength", 0.0))
        h["alpha_blend"] = alpha < 0.999 and getattr(mat, "surface_render_method", "DITHERED") == "BLENDED"
        h["rough_tex"], h["rough_ch"] = _trace_image(p.inputs.get("Roughness"))
        h["metal_tex"], h["metal_ch"] = _trace_image(p.inputs.get("Metallic"))
        h["emission_tex"], _ = _trace_image(p.inputs.get("Emission Color"))
        nsock = p.inputs.get("Normal")
        h["normal_tex"], _ = _trace_image(nsock)
        h["normal_strength"] = 1.0
        if nsock and nsock.is_linked and nsock.links[0].from_node.type == "NORMAL_MAP":
            h["normal_strength"] = float(_val(nsock.links[0].from_node, "Strength", 1.0))
        if h["rough_tex"]:
            h["roughness"] = 1.0
        if h["metal_tex"]:
            h["metallic"] = 1.0
        if h["emission_tex"] and h["emission_strength"] > 0:
            h["emission"] = [1, 1, 1]
    _send(h)


# --------------------------------------------------------------------------- objects & hierarchy

def _parent_key(ob, synced):
    if ob.parent and ob.parent.name in synced:
        return "o:" + ob.parent.name
    for c in ob.users_collection:
        if c != bpy.context.scene.collection:
            return "c:" + c.name
    return ""


def _collection_tree():
    """[(name, parent_key, visible)] for collections in the view layer, parents first."""
    out = []

    def walk(lc, parent_key):
        for child in lc.children:
            if child.exclude:
                continue
            out.append((child.collection.name, parent_key, child.is_visible))
            walk(child, "c:" + child.collection.name)
    walk(bpy.context.view_layer.layer_collection, "")
    return out


def _send_collections():
    for name, parent, visible in _collection_tree():
        _send({"t": "coll", "name": name, "parent": parent, "visible": visible})


def _object_header(ob, synced):
    h = {"t": "obj", "name": ob.name, "visible": ob.visible_get(), "kind": "empty"}
    parent = _parent_key(ob, synced)
    h["parent"] = parent
    world = _godot_world(ob)
    if parent.startswith("o:"):
        world = _godot_world(bpy.data.objects[parent[2:]]).inverted_safe() @ world
    h["xform"] = _to_list12(world)
    if ob.type in GEOMETRY_TYPES:
        h["kind"] = "mesh"
        h["mesh"] = ob.name
        h["shadow"] = getattr(ob, "visible_shadow", True)
        h["col"], h["colonly"] = _collision(ob)
    elif ob.type == "LIGHT":
        li = ob.data
        h["color"] = list(li.color)
        h["shadow"] = li.use_shadow
        if li.type == "SUN":
            h["kind"] = "sun"
            h["energy"] = li.energy
        else:
            h["energy"] = li.energy / 100.0
            h["range"] = li.cutoff_distance if getattr(li, "use_custom_distance", False) else max(5.0, li.energy ** 0.5)
            if li.type == "SPOT":
                h["kind"] = "spot"
                h["spot_angle"] = min(89.9, math.degrees(li.spot_size) * 0.5)
                h["spot_attenuation"] = max(0.05, 1.0 - li.spot_blend)
            else:
                h["kind"] = "omni"
    elif ob.type == "CAMERA":
        cam = ob.data
        h["kind"] = "camera"
        r = bpy.context.scene.render
        w, hgt = r.resolution_x * r.pixel_aspect_x, r.resolution_y * r.pixel_aspect_y
        horizontal = cam.sensor_fit == "HORIZONTAL" or (cam.sensor_fit == "AUTO" and w >= hgt)
        h["fit"] = "width" if horizontal else "height"
        if cam.type == "ORTHO":
            h["projection"], h["size"] = "ortho", cam.ortho_scale
        else:
            h["projection"] = "persp"
            h["fov"] = math.degrees(cam.angle_x if horizontal else cam.angle_y)
        h["near"], h["far"] = cam.clip_start, cam.clip_end
    return h


# --------------------------------------------------------------------------- instances

def _send_instances(depsgraph, now):
    """Geometry-node / collection / particle instances -> one MultiMesh per (instancer, source)."""
    groups = {}
    sources = {}
    for inst in depsgraph.object_instances:
        if not inst.is_instance or inst.parent is None:
            continue
        src = inst.object
        if src.type not in GEOMETRY_TYPES:
            continue
        data_name = (src.data.name + "." + str(len(src.data.vertices))) if src.type == "MESH" and src.data else src.name
        mesh_key = "inst:" + (src.original.name if src.original else src.name) + ":" + data_name
        key = (inst.parent.original.name if inst.parent.original else inst.parent.name) + "/" + mesh_key[5:]
        m = C @ inst.matrix_world @ C_INV
        groups.setdefault(key, (mesh_key, inst.parent.original.name if inst.parent.original else inst.parent.name, []))[2].append(
            (m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3], m[2][0], m[2][1], m[2][2], m[2][3]))
        if mesh_key not in sources:
            sources[mesh_key] = src
    for mesh_key, src in sources.items():
        if now < L.mesh_next.get(mesh_key, 0.0):
            continue
        try:
            me = src.to_mesh()
        except RuntimeError:
            continue
        if me is None:
            continue
        try:
            slots = [s.material.name if s.material else "" for s in src.material_slots]
            _send_mesh_from(mesh_key, me, slots, now)
        finally:
            src.to_mesh_clear()
    for key, (mesh_key, parent, xforms) in groups.items():
        arr = np.asarray(xforms, np.float32)
        _send({"t": "inst", "key": key, "mesh": mesh_key, "count": len(xforms)}, arr.tobytes())
    return sorted(groups)


# --------------------------------------------------------------------------- tick

def _selection_state():
    vl = bpy.context.view_layer
    names = sorted(ob.name for ob in vl.objects if ob.select_get())
    act = vl.objects.active
    return names, (act.name if act and act.name in names else "")


def _tick():
    try:
        _try_connect()
        _apply_chrome()
        _recovery_tick()
        if not L.sock:
            return 0.5
        _read_commands()
        if not L.sock:
            return 0.5

        depsgraph = bpy.context.evaluated_depsgraph_get()
        now = time.monotonic()
        objs = [ob for ob in bpy.context.view_layer.objects if ob.type in SYNC_TYPES]
        by_name = {ob.name: ob for ob in objs}
        synced = set(by_name)

        names = sorted(by_name)
        structure_changed = names != L.last_names

        if L.full_sync:
            L.full_sync = False
            _send_collections()
            for mat in bpy.data.materials:
                _send_material(mat)
            for ob in objs:
                if ob.type in GEOMETRY_TYPES:
                    _send_object_mesh(ob, depsgraph, now, force=True)
            for ob in objs:
                _send(_object_header(ob, synced))
            L.dirty_geo.clear()
            L.dirty_obj.clear()
            L.dirty_mat.clear()
            L.dirty_inst = True
        else:
            if structure_changed:
                _send_collections()
                new = set(names) - set(L.last_names or [])
                for n in new:
                    if by_name[n].type in GEOMETRY_TYPES:
                        _send_object_mesh(by_name[n], depsgraph, now, force=True)
                L.dirty_obj.update(new)
                # Children of re-parented / renamed objects need fresh local transforms.
                L.dirty_obj.update(names)
            for name in list(L.dirty_mat):
                mat = bpy.data.materials.get(name)
                if mat:
                    _send_material(mat)
            L.dirty_mat.clear()
            for name in list(L.dirty_geo):
                ob = by_name.get(name)
                if ob is None or ob.type not in GEOMETRY_TYPES:
                    L.dirty_geo.discard(name)
                elif _send_object_mesh(ob, depsgraph, now):
                    L.dirty_geo.discard(name)
            for name in list(L.dirty_obj):
                ob = by_name.get(name)
                if ob is not None and now >= L.remote_xform_until.get(name, 0.0):
                    _send(_object_header(ob, synced))
                    for child in ob.children:
                        if child.name in synced:
                            _send(_object_header(child, synced))
            L.dirty_obj.clear()

        _settle_meshes(now)
        inst_keys = None
        if L.dirty_inst:
            L.dirty_inst = False
            inst_keys = _send_instances(depsgraph, now)

        if structure_changed or inst_keys is not None:
            L.last_names = names
            msg = {"t": "scene", "objects": names, "collections": [c[0] for c in _collection_tree()]}
            if inst_keys is not None:
                msg["instances"] = inst_keys
            _send(msg)

        sel = _selection_state()
        if sel != L.last_sel:
            L.last_sel = sel
            _send({"t": "sel", "names": sel[0], "active": sel[1]})
    except Exception as e:  # never let the timer die
        import traceback
        traceback.print_exc()
        print("[godot_link]", type(e).__name__, e)
    return TICK


@persistent
def _on_depsgraph(scene, depsgraph):
    if not L.sock:
        return
    for u in depsgraph.updates:
        idb = u.id
        orig = getattr(idb, "original", None) or idb
        if isinstance(idb, bpy.types.Object):
            if u.is_updated_geometry:
                L.dirty_geo.add(orig.name)
                L.dirty_inst = True
            L.dirty_obj.add(orig.name)
            if u.is_updated_transform and (orig.instance_type != "NONE" or orig.is_instancer):
                L.dirty_inst = True
        elif isinstance(idb, bpy.types.Material):
            L.dirty_mat.add(orig.name)
        elif isinstance(idb, (bpy.types.Light, bpy.types.Camera)):
            for ob in bpy.context.view_layer.objects:
                if ob.data == orig:
                    L.dirty_obj.add(ob.name)
        elif isinstance(idb, bpy.types.Collection):
            L.dirty_inst = True


@persistent
def _on_load(_):
    _reset_sync()
    if L.sock:
        _hello()


@persistent
def _on_save(_):
    if L.sock:
        _hello()


# --------------------------------------------------------------------------- crash safety

def _recovery_tick():
    """Keep a recent copy of unsaved work so a Godot crash never loses it."""
    if not RECOVERY_PATH:
        return
    if L.godot_died:
        L.godot_died = False
        _write_recovery()
        print("[godot_link] Godot exited unexpectedly; recovery copy written to", RECOVERY_PATH)
        return
    now = time.monotonic()
    if now - L.last_recovery < RECOVERY_INTERVAL:
        return
    L.last_recovery = now
    if bpy.data.is_dirty:
        _write_recovery()


def _write_recovery():
    if not RECOVERY_PATH:
        return
    try:
        _run_op(lambda: bpy.ops.wm.save_as_mainfile(filepath=RECOVERY_PATH, copy=True, compress=True))
    except Exception as e:
        print("[godot_link] recovery save failed:", e)


def _clear_recovery():
    for p in (RECOVERY_PATH, RECOVERY_PATH + "1") if RECOVERY_PATH else ():
        if os.path.exists(p):
            try:
                os.remove(p)
            except OSError:
                pass


atexit.register(_clear_recovery)  # clean exit -> nothing to recover


def _watchdog():
    """Background thread: if Godot dies, rescue Blender's window before it is taken down."""
    if os.name != "nt" or not GODOT_PID:
        return
    k32 = ctypes.windll.kernel32
    u32 = ctypes.windll.user32
    SYNCHRONIZE = 0x00100000
    handle = k32.OpenProcess(SYNCHRONIZE, False, GODOT_PID)
    if not handle:
        return
    while True:
        if k32.WaitForSingleObject(handle, 200) == 0:  # Godot exited
            break
        if L.parent_hwnd and L.hwnd and not u32.IsWindow(ctypes.c_void_p(L.parent_hwnd)):
            break
    if L.hwnd and u32.IsWindow(ctypes.c_void_p(L.hwnd)):
        GWL_STYLE = -16
        WS_OVERLAPPEDWINDOW, WS_VISIBLE, WS_CHILD = 0x00CF0000, 0x10000000, 0x40000000
        h = ctypes.c_void_p(L.hwnd)
        u32.SetParent(h, None)
        style = (u32.GetWindowLongW(h, GWL_STYLE) & ~WS_CHILD) | WS_OVERLAPPEDWINDOW | WS_VISIBLE
        u32.SetWindowLongW(h, GWL_STYLE, style)
        u32.SetWindowPos(h, None, 100, 100, 1280, 800, 0x0020 | 0x0040)  # FRAMECHANGED | SHOWWINDOW
    L.docked = False
    L.godot_died = True  # main-thread tick writes the recovery copy (bpy is not thread-safe)


# --------------------------------------------------------------------------- look & feel

def _apply_chrome():
    if not HIDE_STATUSBAR:
        return
    want = not L.docked
    for win in bpy.context.window_manager.windows:
        scr = win.screen
        if scr and scr.show_statusbar != want:
            scr.show_statusbar = want


def _hex(s):
    s = s.strip().lstrip("#")
    if len(s) < 6:
        return None
    return tuple(int(s[i:i + 2], 16) / 255.0 for i in (0, 2, 4))


def _setcol(obj, prop, rgb):
    try:
        cur = getattr(obj, prop)
    except AttributeError:
        return
    n = len(cur)
    setattr(obj, prop, tuple(rgb[:3]) + ((cur[3],) if n == 4 else ()))


def _mix(a, b, t):
    return tuple(a[i] * (1 - t) + b[i] * t for i in range(3))


def _apply_theme():
    """Match Godot's base/accent colors for this session only (preferences are not saved)."""
    base, accent = _hex(THEME_BASE), _hex(THEME_ACCENT)
    if not base:
        return
    prefs = bpy.context.preferences
    prefs.use_preferences_save = False  # session-only: nothing below reaches userpref.blend
    th = prefs.themes[0]
    dark = _mix(base, (0, 0, 0), 0.25)
    darker = _mix(base, (0, 0, 0), 0.45)
    light = _mix(base, (1, 1, 1), 0.08)
    for prop in th.bl_rna.properties:
        if prop.type != "POINTER":
            continue
        sp = getattr(th, prop.identifier, None)
        space = getattr(sp, "space", None)
        if space is None:
            continue
        _setcol(space, "back", base)
        _setcol(space, "header", darker)
        _setcol(space, "navigation_bar", dark)
        _setcol(space, "execution_buts", dark)
        _setcol(space, "tab_back", darker)
        grad = getattr(space, "gradients", None)
        if grad is not None:
            _setcol(grad, "high_gradient", base)
            _setcol(grad, "gradient", dark)
        panel = getattr(sp, "space_list", None)
        if panel is not None:
            _setcol(panel, "list", dark)
    ui = th.user_interface
    for w in ("wcol_regular", "wcol_tool", "wcol_toolbar_item", "wcol_text", "wcol_option", "wcol_toggle",
              "wcol_num", "wcol_numslider", "wcol_menu", "wcol_pulldown", "wcol_menu_back", "wcol_tab",
              "wcol_radio", "wcol_box", "wcol_list_item", "wcol_pie_menu", "wcol_tooltip", "wcol_menu_item"):
        wc = getattr(ui, w, None)
        if wc is None:
            continue
        _setcol(wc, "inner", light if w not in ("wcol_box", "wcol_menu_back", "wcol_tooltip") else dark)
        if accent:
            _setcol(wc, "inner_sel", accent)
    if accent:
        _setcol(th.view_3d, "object_active", accent)
        _setcol(ui, "widget_text_cursor", accent)


def _suppress_splash():
    if "--godot-keep-splash" in ARGV:
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


# --------------------------------------------------------------------------- Godot sidebar panel

COL_ITEMS = [("", "Auto (name suffix)", "Use -col / -colonly / -convcol name suffixes"),
             ("none", "None", ""), ("trimesh", "Trimesh", "Exact concave collision (static)"),
             ("convex", "Convex", "Convex hull collision")]


def _col_get(self):
    v = self.get("godot_collision", "")
    return [i[0] for i in COL_ITEMS].index(v) if v in [i[0] for i in COL_ITEMS] else 0


def _col_set(self, value):
    self["godot_collision"] = COL_ITEMS[value][0]


class GODOT_OT_bake(bpy.types.Operator):
    bl_idname = "godot.bake_selected"
    bl_label = "Bake Selected to Godot"
    bl_description = "Export the selection (with rigs and animations) as .glb into the Godot project"

    def execute(self, context):
        if not PROJECT_DIR:
            self.report({"ERROR"}, "Godot project folder unknown")
            return {"CANCELLED"}
        _bake(os.path.join(PROJECT_DIR, "blender_assets"))
        return {"FINISHED"}


class GODOT_OT_resync(bpy.types.Operator):
    bl_idname = "godot.resync"
    bl_label = "Resync"
    bl_description = "Send the whole scene to Godot again"

    def execute(self, context):
        _reset_sync()
        return {"FINISHED"}


class GODOT_PT_panel(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Godot"
    bl_label = "Godot Link"

    def draw(self, context):
        lay = self.layout
        lay.label(text=f"Link: {L.status} (:{PORT})", icon="LINKED" if L.sock else "UNLINKED")
        row = lay.row(align=True)
        row.operator("godot.resync", icon="FILE_REFRESH")
        row.operator("godot.bake_selected", text="Bake to Godot", icon="EXPORT")
        ob = context.object
        if ob and ob.type in GEOMETRY_TYPES:
            box = lay.box()
            box.label(text="Collision", icon="MOD_PHYSICS")
            box.prop(ob, "godot_collision_mode", text="")
            box.prop(ob, "godot_collision_only_prop", text="Collision only (hide mesh)")


def register():
    bpy.types.Object.godot_collision_mode = bpy.props.EnumProperty(
        name="Godot Collision", items=COL_ITEMS, get=_col_get, set=_col_set)
    bpy.types.Object.godot_collision_only_prop = bpy.props.BoolProperty(
        name="Collision Only", get=lambda s: bool(s.get("godot_collision_only", False)),
        set=lambda s, v: s.__setitem__("godot_collision_only", v))
    for cls in (GODOT_OT_bake, GODOT_OT_resync, GODOT_PT_panel):
        try:
            bpy.utils.register_class(cls)
        except ValueError:
            pass
    for lst, fn in ((bpy.app.handlers.depsgraph_update_post, _on_depsgraph),
                    (bpy.app.handlers.load_post, _on_load),
                    (bpy.app.handlers.save_post, _on_save)):
        if fn not in lst:
            lst.append(fn)
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=0.1, persistent=True)
    if GODOT_PID:
        threading.Thread(target=_watchdog, name="godot_watchdog", daemon=True).start()
    print(f"[godot_link] streaming to Godot on 127.0.0.1:{PORT}")


if True:  # executed by --python at startup
    _suppress_splash()
    try:
        _apply_theme()
    except Exception as e:
        print("[godot_link] theme:", e)
    register()
