# Changelog

## 0.9.0 (first public release)
- Blender docked in the Godot 4.7 editor (main-screen tab and dock). Starts invisibly with a progress bar and closes with the editor (it saves the .blend first).
- Live Blender → Godot sync: hierarchy, meshes, materials and textures, lights, cameras, instances, collisions.
- Preview mode (RenderingServer, direct GPU vertex updates) and Scene Nodes mode.
- Two-way selection, transforms and Follow View; F frames the selection; Ctrl+S also saves the .blend.
- Bake glTF with rigs and animations.
- Switching between Godot and Blender doesn't count as losing focus on either side; Godot keeps full speed while Blender runs.
- Recovery copies, crash rescue, hang guard.
- Link secured with a per-session token; falls back to the next free port automatically.
- Optional stall logs for bug reports (off by default).
