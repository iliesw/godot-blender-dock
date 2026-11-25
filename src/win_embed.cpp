#include "win_embed.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <vector>

namespace {

struct FindCtx {
	DWORD pid;
	HWND best;
	LONG best_area;
};

BOOL CALLBACK enum_proc(HWND h, LPARAM lp) {
	FindCtx *ctx = reinterpret_cast<FindCtx *>(lp);
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid != ctx->pid || GetWindow(h, GW_OWNER) != nullptr) {
		return TRUE; // hidden windows count: startup keeps Blender hidden until it is ready
	}
	wchar_t cls[64] = {};
	GetClassNameW(h, cls, 64);
	if (wcscmp(cls, L"GHOST_WindowClass") != 0) {
		return TRUE;
	}
	RECT r;
	GetWindowRect(h, &r);
	LONG area = (r.right - r.left) * (r.bottom - r.top);
	if (area > ctx->best_area) {
		ctx->best = h;
		ctx->best_area = area;
	}
	return TRUE;
}

inline HWND H(intptr_t v) { return reinterpret_cast<HWND>(v); }

bool any_button_down() {
	return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
			(GetAsyncKeyState(VK_MBUTTON) & 0x8000);
}

} // namespace

bool this_process_is_foreground() {
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (fg) {
		GetWindowThreadProcessId(fg, &pid);
	}
	return pid == GetCurrentProcessId();
}

bool extract_exe_icon_rgba(const std::wstring &exe, int size, std::vector<uint8_t> &r_rgba) {
	HICON icon = nullptr;
	if (SHDefExtractIconW(exe.c_str(), 0, 0, &icon, nullptr, MAKELONG(size, size)) != S_OK || !icon) {
		return false;
	}
	bool ok = false;
	ICONINFO info = {};
	if (GetIconInfo(icon, &info)) {
		HDC dc = GetDC(nullptr);
		BITMAPINFO bi = {};
		bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bi.bmiHeader.biWidth = size;
		bi.bmiHeader.biHeight = -size; // top-down
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		std::vector<uint8_t> bgra(size_t(size) * size * 4);
		if (info.hbmColor && GetDIBits(dc, info.hbmColor, 0, size, bgra.data(), &bi, DIB_RGB_COLORS) == size) {
			bool has_alpha = false;
			for (size_t i = 3; i < bgra.size(); i += 4) {
				if (bgra[i]) {
					has_alpha = true;
					break;
				}
			}
			std::vector<uint8_t> mask;
			if (!has_alpha && info.hbmMask) { // legacy icon: derive alpha from the AND mask
				mask.resize(bgra.size());
				GetDIBits(dc, info.hbmMask, 0, size, mask.data(), &bi, DIB_RGB_COLORS);
			}
			r_rgba.resize(bgra.size());
			for (size_t i = 0; i < bgra.size(); i += 4) {
				r_rgba[i + 0] = bgra[i + 2];
				r_rgba[i + 1] = bgra[i + 1];
				r_rgba[i + 2] = bgra[i + 0];
				r_rgba[i + 3] = has_alpha ? bgra[i + 3] : ((!mask.empty() && mask[i]) ? 0 : 255);
			}
			ok = true;
		}
		ReleaseDC(nullptr, dc);
		if (info.hbmColor) {
			DeleteObject(info.hbmColor);
		}
		if (info.hbmMask) {
			DeleteObject(info.hbmMask);
		}
	}
	DestroyIcon(icon);
	return ok;
}

namespace {
struct HideCtx {
	DWORD pid;
};

BOOL CALLBACK hide_proc(HWND h, LPARAM lp) {
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid == reinterpret_cast<HideCtx *>(lp)->pid && IsWindowVisible(h)) {
		wchar_t cls[64] = {};
		GetClassNameW(h, cls, 64);
		if (wcscmp(cls, L"GHOST_WindowClass") == 0) {
			ShowWindowAsync(h, SW_HIDE);
		}
	}
	return TRUE;
}
} // namespace

void WinEmbed::start_hold_thread() {
	stop_hold_thread();
	hold_thread_run_ = true;
	DWORD pid = pid_;
	HANDLE proc = process_;
	hold_thread_ = std::thread([this, pid, proc]() {
		HideCtx ctx = { pid };
		while (hold_thread_run_ && hold_hidden_) {
			if (WaitForSingleObject(proc, 0) != WAIT_TIMEOUT) {
				break; // Blender exited
			}
			EnumWindows(hide_proc, reinterpret_cast<LPARAM>(&ctx));
			Sleep(4);
		}
	});
}

void WinEmbed::stop_hold_thread() {
	hold_thread_run_ = false;
	if (hold_thread_.joinable()) {
		hold_thread_.join();
	}
}

WinEmbed::~WinEmbed() {
	stop_hold_thread();
	// Never leave Blender parented to a window that is about to die: that would destroy
	// Blender's main window and lose unsaved work. Hand it back as a normal window.
	detach();
	if (process_) {
		CloseHandle(process_);
		process_ = nullptr;
	}
}

bool WinEmbed::launch(const std::wstring &exe, const std::wstring &args, const std::wstring &cwd, std::wstring &r_error) {
	if (is_running()) {
		return true;
	}
	if (process_) {
		CloseHandle(process_);
		process_ = nullptr;
	}
	hwnd_ = 0;
	embedded_ = false;

	std::wstring cmd = L"\"" + exe + L"\" " + args;
	std::vector<wchar_t> buf(cmd.begin(), cmd.end());
	buf.push_back(0);

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};
	BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
			cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
	if (!ok) {
		DWORD err = GetLastError();
		wchar_t *msg = nullptr;
		FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
				err, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
		r_error = msg ? msg : L"CreateProcess failed";
		if (msg) {
			LocalFree(msg);
		}
		return false;
	}
	CloseHandle(pi.hThread);
	process_ = pi.hProcess;
	pid_ = pi.dwProcessId;
	if (hold_hidden_) {
		start_hold_thread();
	}
	return true;
}

bool WinEmbed::is_running() {
	return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

void WinEmbed::terminate() {
	if (is_running()) {
		TerminateProcess(process_, 0);
	}
	hwnd_ = 0;
	embedded_ = false;
}

bool WinEmbed::has_window() {
	if (hwnd_ && !IsWindow(H(hwnd_))) {
		hwnd_ = 0; // Blender recreated or closed its main window
		embedded_ = false;
	}
	return hwnd_ != 0;
}

bool WinEmbed::find_window() {
	if (has_window()) {
		return true;
	}
	if (!is_running()) {
		return false;
	}
	FindCtx ctx = { pid_, nullptr, 0 };
	EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
	if (!ctx.best) {
		return false;
	}
	hwnd_ = reinterpret_cast<intptr_t>(ctx.best);
	blender_thread_ = GetWindowThreadProcessId(ctx.best, nullptr);
	orig_style_ = GetWindowLongW(ctx.best, GWL_STYLE);
	orig_exstyle_ = GetWindowLongW(ctx.best, GWL_EXSTYLE);
	if (hold_hidden_) {
		ShowWindowAsync(ctx.best, SW_HIDE); // first frame it exists: hide before it is noticed
		visible_ = false;
	}
	return true;
}

// Owned windows only stack above their owner once their z-order is touched; put Blender
// directly above the Godot window (not above every other application).
static void raise_above_owner(HWND h, HWND owner) {
	HWND root = GetAncestor(owner, GA_ROOT);
	HWND above = GetWindow(root, GW_HWNDPREV);
	while (above == h) {
		above = GetWindow(above, GW_HWNDPREV);
	}
	SetWindowPos(h, above ? above : HWND_TOP, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
}

void WinEmbed::set_hold_hidden(bool p_hold) {
	hold_hidden_ = p_hold;
	if (!p_hold) {
		stop_hold_thread();
	} else if (is_running() && !hold_thread_.joinable()) {
		start_hold_thread();
	}
	if (p_hold && hwnd_ && IsWindow(H(hwnd_))) {
		ShowWindowAsync(H(hwnd_), SW_HIDE);
		visible_ = false;
	}
}

void WinEmbed::enforce_hold() {
	if (hold_hidden_ && hwnd_ && IsWindow(H(hwnd_)) && IsWindowVisible(H(hwnd_))) {
		ShowWindowAsync(H(hwnd_), SW_HIDE);
		visible_ = false;
	}
}

void WinEmbed::release_for_quit() {
	if (!has_window()) {
		return;
	}
	HWND h = H(hwnd_);
	ShowWindow(h, SW_HIDE);
	SetWindowLongPtrW(h, GWLP_HWNDPARENT, 0);
	embedded_ = false;
	parent_ = 0;
	hwnd_ = 0;
}

// Blender is NOT parented into Godot: a cross-process child window shares input with its
// parent, and a busy Blender then freezes Godot's input. Instead it is a borderless window
// *owned* by the Godot window: always above it, minimized with it, hidden from the taskbar and
// Alt+Tab, kept exactly over the host panel. Each program keeps its own input queue.
void WinEmbed::embed(intptr_t parent_hwnd) {
	if (!has_window() || !parent_hwnd) {
		return;
	}
	if (embedded_ && parent_ == parent_hwnd) {
		return;
	}
	HWND h = H(hwnd_);
	if (!embedded_) {
		if (IsZoomed(h) || IsIconic(h)) {
			ShowWindow(h, SW_RESTORE);
		}
		LONG style = orig_style_;
		style &= ~(WS_CHILD | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_MAXIMIZE |
				WS_MINIMIZE);
		style |= WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
		LONG ex = orig_exstyle_ & ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
											 WS_EX_STATICEDGE | WS_EX_TOPMOST);
		ex |= WS_EX_TOOLWINDOW; // no taskbar button, no Alt+Tab entry
		SetWindowLongW(h, GWL_STYLE, style);
		SetWindowLongW(h, GWL_EXSTYLE, ex);
	}
	SetWindowLongPtrW(h, GWLP_HWNDPARENT, static_cast<LONG_PTR>(parent_hwnd)); // owner, not parent
	parent_ = parent_hwnd;
	godot_thread_ = GetWindowThreadProcessId(H(parent_hwnd), nullptr);
	embedded_ = true;
	last_rect_ = EmbedRect();
	SetWindowPos(h, nullptr, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
	raise_above_owner(h, H(parent_hwnd));
	if (visible_ && !hold_hidden_) {
		ShowWindowAsync(h, SW_SHOWNOACTIVATE);
		shown_at_ = GetTickCount64();
	}
}

void WinEmbed::detach() {
	if (!embedded_ || !has_window()) {
		embedded_ = false;
		return;
	}
	HWND h = H(hwnd_);
	RECT r;
	GetWindowRect(h, &r);
	SetWindowLongPtrW(h, GWLP_HWNDPARENT, 0);
	SetWindowLongW(h, GWL_STYLE, (orig_style_ & ~(WS_MAXIMIZE | WS_MINIMIZE | WS_CHILD)) | WS_VISIBLE);
	SetWindowLongW(h, GWL_EXSTYLE, orig_exstyle_);
	int w = std::max<int>(r.right - r.left, 640);
	int hh = std::max<int>(r.bottom - r.top, 480);
	SetWindowPos(h, HWND_TOP, r.left, r.top, w, hh, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
	SetForegroundWindow(h);
	embedded_ = false;
	parent_ = 0;
	visible_ = true;
}

void WinEmbed::set_rect(const EmbedRect &screen_rect) {
	if (!embedded_ || !has_window() || screen_rect == last_rect_) {
		return;
	}
	// Async: never block the Godot editor when Blender is busy (baking, big booleans...).
	SetWindowPos(H(hwnd_), nullptr, screen_rect.x, screen_rect.y, std::max(1, screen_rect.w), std::max(1, screen_rect.h),
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
	last_rect_ = screen_rect;
}

void WinEmbed::set_visible(bool visible) {
	if (visible && hold_hidden_) {
		return;
	}
	if (!embedded_ || !has_window()) {
		visible_ = visible;
		return;
	}
	if (visible == visible_) {
		return;
	}
	if (!visible && blender_has_focus()) {
		focus_godot();
	}
	visible_ = visible;
	ShowWindowAsync(H(hwnd_), visible ? SW_SHOWNOACTIVATE : SW_HIDE);
	if (visible) {
		raise_above_owner(H(hwnd_), H(parent_));
		shown_at_ = GetTickCount64();
	}
}

void WinEmbed::attach_input(bool) {
	// Intentionally unused: input queues are never shared (see embed()).
}

static DWORD foreground_pid() {
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (fg) {
		GetWindowThreadProcessId(fg, &pid);
	}
	return pid;
}

bool WinEmbed::blender_has_focus() {
	return embedded_ && pid_ && foreground_pid() == pid_;
}

// Global, not a member: a reply may arrive after the plugin (and its WinEmbed) is gone.
static uint64_t g_probe_replied = 0;

void __stdcall WinEmbed::probe_reply(void *, unsigned, uintptr_t, intptr_t) {
	g_probe_replied = GetTickCount64();
}

void WinEmbed::probe_responsiveness() {
	uint64_t now = GetTickCount64();
	probe_replied_ = g_probe_replied;
	bool waiting = probe_sent_ > probe_replied_;
	busy_ = waiting && now - probe_sent_ > 300; // status only ("Blender: busy")
	if (!waiting && now >= next_probe_) {
		next_probe_ = now + 250;
		probe_sent_ = now;
		if (!SendMessageCallbackW(H(hwnd_), WM_NULL, 0, 0, reinterpret_cast<SENDASYNCPROC>(&WinEmbed::probe_reply), 0)) {
			g_probe_replied = now;
		}
	}
}

bool WinEmbed::is_responsive() {
	if (!has_window()) {
		return false;
	}
	probe_responsiveness();
	return !busy_ && probe_sent_ <= probe_replied_ && probe_replied_ != 0;
}

void WinEmbed::focus_blender() {
	if (!embedded_ || !visible_ || !has_window() || busy_) {
		return;
	}
	// Godot is in front, so it may hand the foreground to Blender. Normal activation:
	// Blender gets the keyboard through its own input queue, Godot is never blocked.
	SetForegroundWindow(H(hwnd_));
}

void WinEmbed::focus_godot() {
	if (!parent_) {
		return;
	}
	// Allowed because godot_link.py calls AllowSetForegroundWindow(godot_pid) while Blender is in front.
	SetForegroundWindow(GetAncestor(H(parent_), GA_ROOT));
}

void WinEmbed::update_priority(bool p_godot_active) {
	if (!process_) {
		return;
	}
	int want = p_godot_active ? 0 : 1;
	if (want == priority_state_) {
		return;
	}
	priority_state_ = want;
	SetPriorityClass(process_, p_godot_active ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
}

const char *WinEmbed::mouse_location() {
	int x, y;
	bool down = any_button_down();
	if (cursor_over_blender(x, y)) {
		return down ? "over Blender, button down" : "over Blender";
	}
	POINT pt = { x, y };
	HWND w = WindowFromPoint(pt);
	DWORD pid = 0;
	if (w) {
		GetWindowThreadProcessId(w, &pid);
	}
	if (pid == GetCurrentProcessId()) {
		return down ? "over Godot, button down" : "over Godot";
	}
	return down ? "elsewhere, button down" : "elsewhere";
}

bool WinEmbed::cursor_over_blender(int &r_x, int &r_y) {
	POINT pt;
	GetCursorPos(&pt);
	r_x = pt.x;
	r_y = pt.y;
	HWND w = WindowFromPoint(pt);
	DWORD wpid = 0;
	if (w) {
		GetWindowThreadProcessId(w, &wpid);
	}
	return wpid && wpid == pid_;
}

void WinEmbed::update_focus(bool follow_mouse, bool godot_text_focused) {
	if (!embedded_ || !visible_ || !has_window() || !parent_) {
		return;
	}
	probe_responsiveness();
	DWORD fg = foreground_pid();
	bool godot_front = fg == GetCurrentProcessId();
	bool blender_front = fg == pid_;
	if (!godot_front && !blender_front) {
		godot_hover_since_ = 0;
		return; // another application is in front: leave focus alone
	}
	bool buttons = any_button_down();
	prev_buttons_ = buttons;
	if (!follow_mouse || buttons) {
		godot_hover_since_ = 0;
		return; // clicks already move focus the normal Windows way
	}
	int x, y;
	if (cursor_over_blender(x, y)) {
		godot_hover_since_ = 0;
		if (godot_front && !godot_text_focused && GetTickCount64() - shown_at_ > 150) {
			focus_blender();
		}
		return;
	}
	if (!blender_front) {
		return;
	}
	POINT pt = { x, y };
	HWND w = WindowFromPoint(pt);
	DWORD wpid = 0;
	if (w) {
		GetWindowThreadProcessId(w, &wpid);
	}
	if (wpid != GetCurrentProcessId()) {
		godot_hover_since_ = 0;
		return;
	}
	uint64_t now = GetTickCount64();
	if (!godot_hover_since_) {
		godot_hover_since_ = now;
	} else if (now - godot_hover_since_ > 250) {
		focus_godot();
		godot_hover_since_ = 0;
	}
}
