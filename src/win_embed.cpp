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
	if (pid != ctx->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER) != nullptr) {
		return TRUE;
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

WinEmbed::~WinEmbed() {
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
	return true;
}

bool WinEmbed::is_running() {
	return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

void WinEmbed::terminate() {
	if (is_running()) {
		TerminateProcess(process_, 0);
	}
	attach_input(false);
	hwnd_ = 0;
	embedded_ = false;
}

bool WinEmbed::has_window() {
	if (hwnd_ && !IsWindow(H(hwnd_))) {
		// Blender recreated or closed its main window.
		attach_input(false);
		hwnd_ = 0;
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
	return true;
}

void WinEmbed::embed(intptr_t parent_hwnd) {
	if (!has_window() || !parent_hwnd) {
		return;
	}
	if (embedded_ && parent_ == parent_hwnd) {
		return;
	}
	HWND h = H(hwnd_);
	HWND p = H(parent_hwnd);

	bool had_focus = blender_has_focus();
	if (had_focus) {
		focus_godot();
	}
	attach_input(false);

	if (!embedded_) {
		if (IsZoomed(h) || IsIconic(h)) {
			ShowWindow(h, SW_RESTORE);
		}
		LONG style = orig_style_;
		style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_MAXIMIZE |
				WS_MINIMIZE);
		style |= WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
		LONG ex = orig_exstyle_ & ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
											 WS_EX_STATICEDGE | WS_EX_TOPMOST);
		// Style must be WS_CHILD *before* SetParent (see SetParent docs).
		SetWindowLongW(h, GWL_STYLE, style);
		SetWindowLongW(h, GWL_EXSTYLE, ex);
	}

	// The Godot window must not paint over its new child.
	LONG pstyle = GetWindowLongW(p, GWL_STYLE);
	if (!(pstyle & WS_CLIPCHILDREN)) {
		SetWindowLongW(p, GWL_STYLE, pstyle | WS_CLIPCHILDREN);
	}

	SetParent(h, p);
	parent_ = parent_hwnd;
	godot_thread_ = GetWindowThreadProcessId(p, nullptr);
	embedded_ = true;
	last_rect_ = EmbedRect();
	SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);
	if (visible_) {
		ShowWindowAsync(h, SW_SHOWNA);
	}
}

void WinEmbed::detach() {
	if (!embedded_ || !has_window()) {
		embedded_ = false;
		return;
	}
	HWND h = H(hwnd_);
	if (blender_has_focus()) {
		focus_godot();
	}
	attach_input(false);

	RECT r;
	GetWindowRect(h, &r);
	SetParent(h, nullptr);
	SetWindowLongW(h, GWL_STYLE, (orig_style_ & ~(WS_MAXIMIZE | WS_MINIMIZE)) | WS_VISIBLE);
	SetWindowLongW(h, GWL_EXSTYLE, orig_exstyle_);
	int w = std::max<int>(r.right - r.left, 640);
	int hh = std::max<int>(r.bottom - r.top, 480);
	SetWindowPos(h, HWND_TOP, r.left, r.top, w, hh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	SetForegroundWindow(h);
	embedded_ = false;
	parent_ = 0;
	visible_ = true;
}

void WinEmbed::set_rect(const EmbedRect &screen_rect) {
	if (!embedded_ || !has_window()) {
		return;
	}
	if (screen_rect == last_rect_) {
		return;
	}
	POINT pt = { screen_rect.x, screen_rect.y };
	ScreenToClient(H(parent_), &pt);
	// Async: never block the Godot editor when Blender is busy (baking, big booleans...).
	SetWindowPos(H(hwnd_), nullptr, pt.x, pt.y, std::max(1, screen_rect.w), std::max(1, screen_rect.h),
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
	last_rect_ = screen_rect;
}

void WinEmbed::set_visible(bool visible) {
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
	ShowWindowAsync(H(hwnd_), visible ? SW_SHOWNA : SW_HIDE);
	if (visible) {
		shown_at_ = GetTickCount64();
	}
}

void WinEmbed::attach_input(bool attach) {
	if (attach == input_attached_) {
		return;
	}
	if (!blender_thread_ || !godot_thread_ || blender_thread_ == godot_thread_) {
		input_attached_ = false;
		return;
	}
	if (AttachThreadInput(godot_thread_, blender_thread_, attach ? TRUE : FALSE) || !attach) {
		input_attached_ = attach;
	}
}

bool WinEmbed::blender_has_focus() {
	if (!embedded_ || !input_attached_ || !hwnd_) {
		return false;
	}
	HWND f = GetFocus(); // shared input state while attached
	return f && (f == H(hwnd_) || IsChild(H(hwnd_), f));
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
	if (waiting) {
		busy_ = now - probe_sent_ > 300; // no answer for 300 ms: Blender's UI thread is stuck
	} else {
		busy_ = false;
	}
	if (!waiting && now >= next_probe_) {
		next_probe_ = now + 200;
		probe_sent_ = now;
		if (!SendMessageCallbackW(H(hwnd_), WM_NULL, 0, 0, reinterpret_cast<SENDASYNCPROC>(&WinEmbed::probe_reply),
					0)) {
			g_probe_replied = now;
		}
	}
}

void WinEmbed::focus_blender() {
	if (!embedded_ || !visible_ || !has_window()) {
		return;
	}
	// Never share input with a frozen Blender: Godot's own input would freeze with it.
	if (busy_ || IsHungAppWindow(H(hwnd_))) {
		return;
	}
	// Handing focus over is synchronous with Blender's UI thread. Wait until a freshly shown
	// window has redrawn and Blender answered a recent probe, so Godot never stalls on it.
	uint64_t now = GetTickCount64();
	if (now - shown_at_ < 250 || probe_sent_ > probe_replied_) {
		return;
	}
	// Share the input queue only while Blender owns the keyboard, so a busy Blender
	// can never stall Godot's input the rest of the time.
	attach_input(true);
	if (!input_attached_) {
		return;
	}
	SetFocus(H(hwnd_));
	// Child windows never get WM_ACTIVATE; GHOST uses it to mark its window active.
	PostMessageW(H(hwnd_), WM_ACTIVATE, WA_CLICKACTIVE, 0);
}

void WinEmbed::focus_godot() {
	bool was_attached = input_attached_;
	// Detach first: then SetFocus stays inside Godot's own thread and never waits on Blender.
	attach_input(false);
	if (!parent_) {
		return;
	}
	SetFocus(H(parent_));
	if (was_attached && hwnd_) {
		PostMessageW(H(hwnd_), WM_KILLFOCUS, reinterpret_cast<WPARAM>(H(parent_)), 0);
		PostMessageW(H(hwnd_), WM_ACTIVATE, WA_INACTIVE, 0);
	}
}

bool WinEmbed::cursor_over_blender(int &r_x, int &r_y) {
	POINT pt;
	GetCursorPos(&pt);
	r_x = pt.x;
	r_y = pt.y;
	HWND w = WindowFromPoint(pt);
	return w && (w == H(hwnd_) || IsChild(H(hwnd_), w));
}

void WinEmbed::update_focus(bool follow_mouse, bool godot_text_focused) {
	if (!embedded_ || !visible_ || !has_window() || !parent_) {
		return;
	}
	HWND top = GetAncestor(H(parent_), GA_ROOT);
	if (GetForegroundWindow() != top) {
		return; // another app (or a Godot dialog / Blender preferences window) is in front
	}
	// Hang guard: while input is shared, a frozen Blender would freeze Godot's input too.
	probe_responsiveness();
	if (busy_) {
		if (input_attached_) {
			attach_input(false); // detach first, then Godot can take its keyboard back
			SetFocus(H(parent_));
		}
		return;
	}

	bool buttons = any_button_down();
	bool pressed = buttons && !prev_buttons_;
	prev_buttons_ = buttons;

	int x, y;
	bool over_blender = cursor_over_blender(x, y);
	bool has = blender_has_focus();

	if (over_blender) {
		godot_hover_since_ = 0;
		pending_click_ = pending_click_ || pressed; // focus may be deferred a few frames; keep the click
		if (!has && (pending_click_ || (follow_mouse && !buttons && !godot_text_focused))) {
			focus_blender();
		}
		if (blender_has_focus()) {
			pending_click_ = false;
		}
		return;
	}

	pending_click_ = false;
	if (!has) {
		return;
	}
	// Cursor is outside Blender. Only react when it is over a Godot window.
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
	if (pressed) {
		focus_godot();
		godot_hover_since_ = 0;
		return;
	}
	if (follow_mouse && !buttons) {
		uint64_t now = GetTickCount64();
		if (!godot_hover_since_) {
			godot_hover_since_ = now;
		} else if (now - godot_hover_since_ > 250) {
			focus_godot();
			godot_hover_since_ = 0;
		}
	}
}
