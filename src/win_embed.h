#pragma once
// Thin Win32 layer: launch Blender, find its main window, reparent it into a host
// window as a borderless child, keep it positioned, and juggle keyboard focus.
// Deliberately free of Godot headers so <windows.h> never meets godot-cpp macros.

#include <cstdint>
#include <string>
#include <vector>

struct EmbedRect {
	int x = 0, y = 0, w = 0, h = 0; // screen coordinates, physical pixels
	bool operator==(const EmbedRect &o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
	bool operator!=(const EmbedRect &o) const { return !(*this == o); }
};

// True when the foreground window belongs to this (Godot) process.
bool this_process_is_foreground();

// Reads the icon embedded in an .exe at the requested pixel size, as tightly packed RGBA8.
bool extract_exe_icon_rgba(const std::wstring &exe, int size, std::vector<uint8_t> &r_rgba);

class WinEmbed {
public:
	~WinEmbed();

	// Process ---------------------------------------------------------------
	bool launch(const std::wstring &exe, const std::wstring &args, const std::wstring &cwd, std::wstring &r_error);
	bool is_running();
	void terminate();
	uint32_t pid() const { return pid_; }

	// Window ----------------------------------------------------------------
	// Looks for Blender's main GHOST window (once per call). Returns true when found.
	bool find_window();
	bool has_window();
	bool is_embedded() const { return embedded_; }
	intptr_t hwnd() const { return hwnd_; }
	intptr_t parent() const { return parent_; }
	// True while Blender's UI thread is not answering (long operation / freeze).
	bool is_blender_busy() const { return busy_; }

	void embed(intptr_t parent_hwnd);
	void detach();
	void set_rect(const EmbedRect &screen_rect);
	void set_visible(bool visible);

	// Focus -----------------------------------------------------------------
	// Call every editor tick. Moves keyboard focus between Godot and Blender.
	// follow_mouse: hovering Blender gives it focus, hovering Godot for a moment gives it back.
	// godot_text_focused: Godot has a text field focused -> do not steal focus on hover.
	void update_focus(bool follow_mouse, bool godot_text_focused);
	bool blender_has_focus();
	void focus_godot();

private:
	void attach_input(bool attach);
	bool cursor_over_blender(int &r_x, int &r_y);
	void focus_blender();

	void *process_ = nullptr; // HANDLE
	uint32_t pid_ = 0;
	intptr_t hwnd_ = 0; // Blender main window
	intptr_t parent_ = 0; // Godot window
	uint32_t blender_thread_ = 0;
	uint32_t godot_thread_ = 0;
	bool input_attached_ = false;
	bool embedded_ = false;
	bool visible_ = true;
	long orig_style_ = 0;
	long orig_exstyle_ = 0;
	EmbedRect last_rect_;
	uint64_t godot_hover_since_ = 0;
	bool prev_buttons_ = false;
	bool pending_click_ = false;
	bool busy_ = false;
	// Non-blocking hang probe: WM_NULL via SendMessageCallback, answered on Godot's message pump.
	uint64_t probe_sent_ = 0;
	uint64_t probe_replied_ = 0;
	uint64_t next_probe_ = 0;
	uint64_t shown_at_ = 0; // let Blender redraw a freshly revealed window before handing it focus
	void probe_responsiveness();
	static void __stdcall probe_reply(void *hwnd, unsigned msg, uintptr_t data, intptr_t result);
};
