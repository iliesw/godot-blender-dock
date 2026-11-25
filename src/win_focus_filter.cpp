// Focus filter: make Godot treat the docked Blender as part of itself.
//
// Every switch between Godot and Blender is an application switch for Windows, so Godot gets
// WM_ACTIVATEAPP and runs its application focus-out / focus-in work (throttle to the unfocused
// frame rate; on return: filesystem change scan, external-edit checks, extension reload check,
// full redraw). That cost ~100 ms on each click back into Godot.
//
// The plugin lives in Godot's process, so it subclasses Godot's top-level windows and drops
// WM_ACTIVATEAPP only when the "other application" is the docked Blender. Switches to any
// other program still reach Godot. If the user goes from Blender to a third program, Godot is
// told then (focus-out is posted), so it never misses a real focus change.

#include "win_focus_filter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <unordered_map>

namespace {

std::unordered_map<HWND, WNDPROC> g_original; // subclassed Godot windows -> their real WndProc
DWORD g_blender_tid = 0; // Blender's UI thread (0 = filter inactive)
DWORD g_blender_pid = 0;
bool g_godot_app_focused = true; // what Godot currently believes about application focus
DWORD g_main_tid = 0;
bool g_focus_out_posted = false;
// Per Godot window: does Godot believe it is the active window? (DisplayServerWindows tracks this
// only through WM_ACTIVATE -> _process_activate_event; WM_SETFOCUS/KILLFOCUS are not handled.)
std::unordered_map<HWND, bool> g_window_active;
std::unordered_map<HWND, bool> g_deactivate_posted;
DWORD g_last_fg_pid = 0; // foreground owner last frame (fallback when WM_ACTIVATE has no lParam)


bool belongs_to_blender(HWND h) {
	DWORD pid = 0;
	if (h) {
		GetWindowThreadProcessId(h, &pid);
	}
	return g_blender_pid && pid == g_blender_pid;
}

bool cursor_over_blender() {
	POINT pt;
	GetCursorPos(&pt);
	return belongs_to_blender(WindowFromPoint(pt));
}

LRESULT CALLBACK filter_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	auto it = g_original.find(hwnd);
	WNDPROC orig = it != g_original.end() ? it->second : nullptr;
	if (msg == WM_ACTIVATEAPP && g_blender_tid) {
		bool activating = wparam != FALSE;
		DWORD other_tid = static_cast<DWORD>(lparam);
		if (other_tid == g_blender_tid) {
			// Godot <-> docked Blender: keep Godot "focused" (no focus-out/in work, no throttle).
			// Only swallow while consistent with what Godot believes.
			if (!activating && g_godot_app_focused) {
				return 0;
			}
			if (activating && g_godot_app_focused) {
				return 0;
			}
		}
		g_godot_app_focused = activating;
		g_focus_out_posted = false;
	}
	if (msg == WM_NCACTIVATE && g_blender_tid && wparam == FALSE &&
			(belongs_to_blender(GetForegroundWindow()) || cursor_over_blender())) {
		// Title bar keeps its active look while the docked Blender is in front.
		return orig ? CallWindowProcW(orig, hwnd, msg, TRUE, lparam) : DefWindowProcW(hwnd, msg, TRUE, lparam);
	}
	if (msg == WM_ACTIVATE && g_blender_tid) {
		WORD state = LOWORD(wparam);
		HWND other = reinterpret_cast<HWND>(lparam);
		bool activating = state == WA_ACTIVE || state == WA_CLICKACTIVE;
		bool other_is_blender;
		if (other) {
			other_is_blender = belongs_to_blender(other);
		} else if (activating) {
			other_is_blender = g_last_fg_pid == g_blender_pid; // coming back from Blender
		} else {
			other_is_blender = belongs_to_blender(GetForegroundWindow()) || cursor_over_blender();
		}
		bool believed_active = g_window_active.count(hwnd) ? g_window_active[hwnd] : false;
		if (other_is_blender && believed_active) {
			if (activating) {
				// Godot's own activation does SetFocus; do just that so the keyboard comes back,
				// without the focus-in event (no editor-wide refresh).
				if (!IsIconic(hwnd)) {
					SetFocus(hwnd);
				}
			}
			return 0; // Godot <-> docked Blender: Godot stays "active"
		}
		g_window_active[hwnd] = activating;
		g_deactivate_posted[hwnd] = false;
	}
	if (msg == WM_NCDESTROY) {
		LRESULT r = orig ? CallWindowProcW(orig, hwnd, msg, wparam, lparam) : DefWindowProcW(hwnd, msg, wparam, lparam);
		g_original.erase(hwnd);
		g_window_active.erase(hwnd);
		g_deactivate_posted.erase(hwnd);
		return r;
	}
	return orig ? CallWindowProcW(orig, hwnd, msg, wparam, lparam) : DefWindowProcW(hwnd, msg, wparam, lparam);
}

BOOL CALLBACK hook_window(HWND h, LPARAM) {
	if (g_original.count(h)) {
		return TRUE;
	}
	wchar_t cls[64] = {};
	GetClassNameW(h, cls, 64);
	if (wcscmp(cls, L"Engine") != 0) { // Godot's window class
		return TRUE;
	}
	WNDPROC prev = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&filter_proc)));
	if (prev) {
		g_original[h] = prev;
		g_window_active[h] = GetForegroundWindow() == h;
	}
	return TRUE;
}

} // namespace

void focus_filter_update(uint32_t p_blender_pid, uint32_t p_blender_tid) {
	g_blender_pid = p_blender_pid;
	g_blender_tid = p_blender_tid;
	if (!g_main_tid) {
		g_main_tid = GetCurrentThreadId();
	}
	if (!g_blender_tid) {
		return;
	}
	EnumThreadWindows(g_main_tid, hook_window, 0); // new Godot windows (floating docks, dialogs)

	// Blender -> a third program: Godot did not hear about losing focus, tell it now.
	HWND fg = GetForegroundWindow();
	DWORD fg_pid = 0;
	DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, &fg_pid) : 0;
	if (fg && fg_pid != GetCurrentProcessId() && fg_pid != g_blender_pid) {
		for (auto &kv : g_window_active) {
			if (kv.second && !g_deactivate_posted[kv.first]) {
				// Real window deactivation, delivered through the normal path (passes the filter).
				g_deactivate_posted[kv.first] = PostMessageW(kv.first, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0),
														 reinterpret_cast<LPARAM>(fg)) != 0;
			}
		}
	}
	g_last_fg_pid = fg_pid;
	if (fg && g_godot_app_focused && !g_focus_out_posted && fg_pid != GetCurrentProcessId() && fg_pid != g_blender_pid &&
			!g_original.empty()) {
		// Posted (not called) so Godot handles it in its normal message loop; it passes the filter
		// because the other thread is not Blender's.
		g_focus_out_posted = PostMessageW(g_original.begin()->first, WM_ACTIVATEAPP, FALSE, static_cast<LPARAM>(fg_tid)) != 0;
	}
}

void focus_filter_shutdown() {
	// If Godot was kept "focused" while Blender was active, let it know the truth first.
	HWND fg = GetForegroundWindow();
	DWORD fg_pid = 0;
	DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, &fg_pid) : 0;
	if (g_godot_app_focused && fg_pid != GetCurrentProcessId() && !g_original.empty()) {
		HWND any = g_original.begin()->first;
		CallWindowProcW(g_original[any], any, WM_ACTIVATEAPP, FALSE, static_cast<LPARAM>(fg_tid));
		g_godot_app_focused = false;
	}
	for (const auto &kv : g_original) {
		if (IsWindow(kv.first) &&
				reinterpret_cast<WNDPROC>(GetWindowLongPtrW(kv.first, GWLP_WNDPROC)) == &filter_proc) {
			SetWindowLongPtrW(kv.first, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(kv.second));
		}
	}
	g_original.clear();
	g_window_active.clear();
	g_deactivate_posted.clear();
	g_blender_tid = 0;
	g_blender_pid = 0;
}
