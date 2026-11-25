# Blender Dock for Godot

A GDExtension that runs the real Blender inside the Godot 4.7 editor (Windows), plus a live link that streams the Blender scene into your Godot scene as you edit.

- **Blender main-screen tab**: a "Blender" tab next to 2D / 3D / Script / Game.
- **Blender dock**: in the bottom panel by default. You can move it to a side slot or float it, so Blender sits beside Godot's 3D viewport.
- **Live sync**: meshes (with modifiers applied), materials (Principled BSDF: color, metallic, roughness, emission, base-color texture), lights, cameras and empties appear under a `BlenderLink` node and update about 30 times per second.

## Use
1. Copy `demo/addons/blender_dock/` into your project's `addons/` folder and restart the editor. A GDExtension needs no plugin toggle.
2. Press **Launch Blender**. It finds the newest `C:\Program Files\Blender Foundation\Blender X.Y\blender.exe`, or you can set
   *Editor Settings > Blender Dock > Blender Path*.
3. Open a 3D scene and press **Link to Scene**. Edit in Blender and watch the Godot viewport update.
4. Use **Open .blend...** to set the project's file (*Project Settings > Blender Dock > Blend File*). Use **Pop Out** / **Dock** to move Blender between its own window and the editor.

Keyboard focus follows the mouse: hovering over Blender gives it the keyboard, and hovering over Godot for about 0.25 s gives it back. Turn this off with *Editor Settings > Blender Dock > Focus Follows Mouse* to switch focus by clicking instead.

## Build
```
git clone --depth 1 --branch 10.0.0-stable https://github.com/godotengine/godot-cpp
pip install scons
scons            # -> demo/addons/blender_dock/bin/blender_dock.windows.editor.x86_64.dll
```
Requires MSVC 2022. `tests/test_link_payload.py` checks the Blender add-on without a UI:
`blender -b --factory-startup --python tests/test_link_payload.py`.

## How it works
- `src/win_embed.cpp`: launches `blender.exe`, finds its `GHOST_WindowClass` window, removes the frame, and reparents it with `SetParent` into whichever Godot window holds the visible host. It keeps the window positioned with async `SetWindowPos`. It shares the input queue (`AttachThreadInput`) only while Blender has keyboard focus.
- `src/blender_link.cpp`: a TCP server on `127.0.0.1:6789` that receives length-prefixed messages (JSON header plus a raw binary mesh payload) and builds `ArrayMesh` / `StandardMaterial3D` / light / camera nodes.
- `demo/addons/blender_dock/blender/godot_link.py`: the Blender side. Blender runs it at startup with `--python`. It converts coordinates (Z-up to Y-up), flips triangle winding, and sends only what changed.

## Limits
- Windows only (Linux X11 would be possible; macOS and Wayland would not).
- Godot's single-window mode must stay **off**. Otherwise Godot popups open underneath Blender.
- Sync is one-way, Blender to Godot. Rigs/animation, geometry-node instances, vertex colors and complex node materials are not streamed.
- Light power is a rough conversion from Blender watts to Godot energy.
- The `BlenderLink` nodes, with their meshes embedded, are saved into the `.tscn`. For final assets, export glTF or use Godot's native `.blend` import.
- If the Godot editor crashes while Blender is docked, Blender's window goes down with it. Save in Blender often (closing the editor normally hands Blender back as a regular window).
