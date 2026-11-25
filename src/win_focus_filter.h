#pragma once
// Keeps Godot from running its application focus-out/in work when focus moves between the
// editor and the docked Blender (see win_focus_filter.cpp). Windows only.

#include <cstdint>

// Call every editor frame while Blender is docked (pid/tid 0 = disable filtering).
void focus_filter_update(uint32_t p_blender_pid, uint32_t p_blender_tid);
// Call before the plugin unloads: restores Godot's window procedures.
void focus_filter_shutdown();
