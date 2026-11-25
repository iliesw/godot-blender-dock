# Blender Dock for Godot

A GDExtension that runs the real Blender inside the Godot 4.7 editor (Windows) and links the two both ways.

## What you get
- **Blender main-screen tab** next to 2D / 3D / Script / Game, with Blender's own icon.
- **Blender dock**: bottom panel by default. It can also go in a side slot or float, so Blender sits beside Godot's 3D viewport.
- **Live sync, Blender → Godot**, under a `BlenderLink` node:
  - hierarchy: object parenting and collections become nested `Node3D`s
  - meshes with modifiers, armature deformation and shape keys; UV, UV2, vertex colors, tangents
  - materials (Principled BSDF): color, metallic, roughness, emission, and base-color / normal / roughness / metallic / emission textures. Packed textures are extracted automatically.
  - lights, cameras, empties
  - geometry-node, collection and particle instances as `MultiMeshInstance3D`
  - collisions: a name suffix `-col`, `-colonly`, `-convcol` or `-convcolonly`, or the **Godot** tab in Blender's sidebar, adds a `StaticBody3D` with a trimesh or convex shape
- **Godot → Blender**:
  - selection follows in both directions
  - moving or rotating a synced node in Godot moves the Blender object
  - **Follow View**: Blender's viewport follows Godot's 3D editor camera
  - **Ctrl+S** saves the scene *and* the `.blend` (to the project's blend file, or `res://blender/<scene>.blend`)
- **Bake glTF**: exports Blender's selection with rigs, skinning, shape keys and animations to `res://blender_assets/*.glb`. Godot imports it as a normal animated scene with an `AnimationPlayer`.
- **Fast big meshes**: when only vertex positions change (sculpting, editing, posing), just positions and normals are sent. One full update, including tangents, follows once you pause. A 250k-triangle mesh takes about 45 ms instead of 330 ms per update.
- **Crash safety**:
  - Blender keeps a recovery copy of unsaved work.
  - If the Godot editor dies, Blender pulls its window out and stays open.
  - On the next launch, the dock offers to recover that work into `res://blender/recovered_*.blend`.
- **Hang guard**: if Blender stops responding (long bake, heavy operation), Godot takes the keyboard back, so its own input never freezes.
- **Look**:
  - Blender takes Godot's base and accent colors, for that session only (your Blender preferences are never saved by it).
  - No splash screen; your normal Blender still shows one.
  - The status bar is hidden while docked.
- **Settings dialog** (gear button): Blender path, port, auto-launch, focus mode, theme matching, status bar, splash, recovery interval.

## Use
1. Copy `demo/addons/blender_dock/` into your project's `addons/` folder and restart the editor.
2. Press **Launch**. It auto-detects the newest `C:\Program Files\Blender Foundation\Blender X.Y\blender.exe`, or use the gear button to choose one.
3. Open a 3D scene and press **Link to Scene**.

Keyboard focus follows the mouse by default: hovering over Blender gives it the keyboard. You can switch to click-to-focus in settings.

## Build
```
git clone --depth 1 --branch 10.0.0-stable https://github.com/godotengine/godot-cpp
pip install scons
scons            # -> demo/addons/blender_dock/bin/blender_dock.windows.editor.x86_64.dll
```
Requires MSVC 2022.

## Tests
- `blender -b --factory-startup --python tests/test_link_payload.py`: checks the Blender side without a UI (hierarchy, instances, materials, collisions, position-only updates, selection and transforms coming from Godot).
- `blender -b --factory-startup --python tests/perf_bigmesh.py`: big-mesh update timings.
- In-editor self-test: start Godot with `BLENDER_DOCK_AUTOLAUNCH=1 BLENDER_DOCK_SELFTEST=1` (plus `BLENDER_DOCK_PORT=6790` to run beside another editor). It walks through the dock, selection, transform, follow view, save and bake, and prints `SELFTEST` lines to the log.

## How it works
- `src/win_embed.cpp` launches `blender.exe`, removes its frame and reparents it (`SetParent`) into whichever Godot window holds the visible host. It shares the input queue only while Blender has focus and never shares it with a frozen Blender.
- `src/blender_link.cpp` is a TCP server on `127.0.0.1:6789`. Messages are a JSON header plus a raw binary payload; the protocol is documented in `blender_link.h`.
- `demo/addons/blender_dock/blender/godot_link.py` is the Blender side. It converts Z-up to Y-up and flips triangle winding, sends only what changed, runs the crash watchdog, and provides the Godot sidebar panel.

## Limits
- Windows only.
- Godot's single-window mode must stay off.
- Animation is not streamed as tracks. Live posing shows as deformed meshes; use **Bake glTF** to get real skeletons and `AnimationPlayer`s.
- Light power is a rough conversion from watts to Godot energy.
- Linked nodes and their meshes are saved inside the `.tscn`.
- Follow View goes from Godot to Blender only; Godot has no API for moving its editor camera.
