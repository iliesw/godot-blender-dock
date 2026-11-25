// Loaded into Blender by godot_link.py (ctypes). Mirror of src/win_focus_filter.cpp on the
// Blender side: when focus moves between Blender and the Godot editor that docks it, Blender's
// windows do not see a deactivation/activation (no GHOST window-deactivate / re-activate work),
// so neither program "loses focus" to the other. Switches to any other program are untouched,
// and if Godot hands over to a third program while Blender was kept "active", Blender is told.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <unordered_map>

namespace {

std::unordered_map<HWND, WNDPROC> g_original;
std::unordered_map<HWND, bool> g_window_active;
std::unordered_map<HWND, bool> g_deactivate_posted;
DWORD g_godot_pid = 0;
DWORD g_last_fg_pid = 0;
bool g_app_active = true;

bool belongs_to_godot(HWND h) {
	DWORD pid = 0;
	if (h) {
		GetWindowThreadProcessId(h, &pid);
	}
	return g_godot_pid && pid == g_godot_pid;
}

bool thread_is_godot(DWORD tid) {
	HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
	if (!t) {
		return false;
	}
	DWORD pid = GetProcessIdOfThread(t);
	CloseHandle(t);
	return g_godot_pid && pid == g_godot_pid;
}

LRESULT call_orig(HWND h, UINT m, WPARAM w, LPARAM l) {
	auto it = g_original.find(h);
	return it != g_original.end() ? CallWindowProcW(it->second, h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK filter_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	if (g_godot_pid) {
		if (msg == WM_ACTIVATEAPP) {
			bool activating = wparam != FALSE;
			if (thread_is_godot(static_cast<DWORD>(lparam)) && g_app_active) {
				return 0; // Blender <-> its Godot: not an application switch for Blender
			}
			g_app_active = activating;
		} else if (msg == WM_ACTIVATE) {
			WORD state = LOWORD(wparam);
			bool activating = state == WA_ACTIVE || state == WA_CLICKACTIVE;
			HWND other = reinterpret_cast<HWND>(lparam);
			bool other_is_godot = other ? belongs_to_godot(other)
										: (activating ? g_last_fg_pid == g_godot_pid : belongs_to_godot(GetForegroundWindow()));
			bool believed = g_window_active.count(hwnd) ? g_window_active[hwnd] : false;
			if (other_is_godot && believed) {
				if (activating && !IsIconic(hwnd)) {
					SetFocus(hwnd); // keyboard (raw input follows focus) comes back to Blender
				}
				return 0;
			}
			g_window_active[hwnd] = activating;
			g_deactivate_posted[hwnd] = false;
		}
	}
	if (msg == WM_NCDESTROY) {
		LRESULT r = call_orig(hwnd, msg, wparam, lparam);
		g_original.erase(hwnd);
		g_window_active.erase(hwnd);
		g_deactivate_posted.erase(hwnd);
		return r;
	}
	return call_orig(hwnd, msg, wparam, lparam);
}

BOOL CALLBACK hook_window(HWND h, LPARAM) {
	if (g_original.count(h)) {
		return TRUE;
	}
	wchar_t cls[64] = {};
	GetClassNameW(h, cls, 64);
	if (wcscmp(cls, L"GHOST_WindowClass") != 0) {
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

extern "C" {

// Call from Blender's main thread (a bpy timer) regularly. godot_pid 0 disables filtering.
__declspec(dllexport) int bdh_update(unsigned int godot_pid) {
	g_godot_pid = godot_pid;
	EnumThreadWindows(GetCurrentThreadId(), hook_window, 0);
	HWND fg = GetForegroundWindow();
	DWORD fg_pid = 0;
	if (fg) {
		GetWindowThreadProcessId(fg, &fg_pid);
	}
	if (g_godot_pid && fg && fg_pid != GetCurrentProcessId() && fg_pid != g_godot_pid) {
		for (auto &kv : g_window_active) { // a third program took over: deliver the real deactivation
			if (kv.second && !g_deactivate_posted[kv.first]) {
				g_deactivate_posted[kv.first] = PostMessageW(kv.first, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0),
														 reinterpret_cast<LPARAM>(fg)) != 0;
			}
		}
	}
	g_last_fg_pid = fg_pid;
	return static_cast<int>(g_original.size());
}

// Restore Blender's window procedures (call before quitting / when undocked for good).
__declspec(dllexport) void bdh_shutdown() {
	for (const auto &kv : g_original) {
		if (IsWindow(kv.first) && reinterpret_cast<WNDPROC>(GetWindowLongPtrW(kv.first, GWLP_WNDPROC)) == &filter_proc) {
			SetWindowLongPtrW(kv.first, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(kv.second));
		}
	}
	g_original.clear();
	g_window_active.clear();
	g_deactivate_posted.clear();
	g_godot_pid = 0;
}
}
