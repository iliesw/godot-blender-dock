#pragma once

#include "blender_link.h"

#include <godot_cpp/classes/accept_dialog.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/confirmation_dialog.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/editor_dock.hpp>
#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/progress_bar.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include <memory>

class WinEmbed;
struct EmbedRect;

namespace godot {

// One Blender process, shown in whichever host is visible:
//   - the "Blender" main-screen tab (next to 2D / 3D / Script), or
//   - the "Blender" dock (bottom panel by default; can be moved to a side or floated).
class BlenderDockPlugin : public EditorPlugin {
	GDCLASS(BlenderDockPlugin, EditorPlugin)

public:
	BlenderDockPlugin();
	~BlenderDockPlugin();

	void _enter_tree() override;
	void _exit_tree() override;
	void _process(double p_delta) override;
	void _save_external_data() override;

	bool _has_main_screen() const override { return true; }
	void _make_visible(bool p_visible) override;
	String _get_plugin_name() const override { return "Blender"; }
	Ref<Texture2D> _get_plugin_icon() const override;

protected:
	static void _bind_methods();

private:
	struct Panel {
		VBoxContainer *root = nullptr;
		Control *host = nullptr;
		Label *placeholder = nullptr;
		Button *launch = nullptr;
		Button *open = nullptr;
		Button *popout = nullptr;
		Button *add_link = nullptr;
		Button *bake = nullptr;
		Button *settings = nullptr;
		CheckBox *sync = nullptr;
		CheckBox *follow = nullptr;
		Label *status = nullptr;
		Control *loading = nullptr; // startup progress, shown over the host
		Label *loading_label = nullptr;
		ProgressBar *loading_bar = nullptr;
	};

	struct SettingsUI {
		AcceptDialog *dialog = nullptr;
		LineEdit *path = nullptr;
		SpinBox *port = nullptr;
		SpinBox *recovery = nullptr;
		CheckBox *auto_launch = nullptr;
		CheckBox *follow_mouse = nullptr;
		CheckBox *theme = nullptr;
		CheckBox *statusbar = nullptr;
		CheckBox *splash = nullptr;
		EditorFileDialog *browse = nullptr;
	};

	void _build_panel(Panel &p);
	void _build_settings();
	void _launch(const String &p_blend);
	void _launch_checked(const String &p_blend);
	String _find_blender() const;
	String _recovery_path() const;
	String _default_blend_path() const;
	Control *_active_host();
	void _update_ui();
	void _ensure_settings();
	Variant _setting(const String &p_name, const Variant &p_default) const;
	void _follow_view();
	void _send_chrome();
	bool _untint_tab_icon(Node *p_node);

	void _on_launch_pressed();
	void _on_open_pressed();
	void _on_popout_pressed();
	void _on_add_link_pressed();
	void _on_bake_pressed();
	void _on_settings_pressed();
	void _on_settings_confirmed();
	void _on_browse_blender();
	void _on_blender_path_selected(const String &p_path);
	void _on_sync_toggled(bool p_on);
	void _on_follow_toggled(bool p_on);
	void _on_file_selected(const String &p_path);
	void _on_selection_changed();
	void _on_recover_confirmed();
	void _on_recover_canceled();

	std::unique_ptr<WinEmbed> embed;
	Ref<BlenderLink> link;

	Panel main_panel;
	Panel dock_panel;
	EditorDock *dock = nullptr;
	EditorFileDialog *file_dialog = nullptr;
	ConfirmationDialog *recover_dialog = nullptr;
	SettingsUI settings;

	bool main_visible = false;
	bool popped_out = false;
	bool sync_enabled = true;
	bool follow_view = false;
	bool pending_auto_link = false;
	// Startup: Blender stays hidden until it has loaded and synced; the panels show progress.
	bool blender_ready = false;
	double launch_elapsed = 0.0;
	double window_elapsed = -1.0;
	double loading_value = 0.0;
	String loading_text;
	void _update_startup(double p_delta);
	EmbedRect _launch_rect();
	bool tab_icon_fixed = false;
	bool chrome_sent_docked = false;
	intptr_t embedded_sent_parent = 0;
	String pending_launch_blend;
	Transform3D last_view;
	double last_fov = 0.0;
	double find_cooldown = 0.0;
	double ui_cooldown = 0.0;
	double notice_time = 0.0;
	String last_error;
	String notice;
	mutable Ref<Texture2D> blender_icon;
	// BLENDER_DOCK_SELFTEST=1: scripted checks without mouse automation (see _self_test).
	int selftest_step = -1;
	double selftest_time = 0.0;
	void _self_test(double p_delta);
	void _process_inner(double p_delta);
	void _perf_test(double p_delta, uint64_t p_cost_us);
	int perf_step = -1;
	bool perf_focus_mode = false; // BLENDER_DOCK_PERFTEST=focus: bounce keyboard focus Godot <-> Blender
	double perf_time = 0.0, perf_max_gap = 0.0, perf_sum_gap = 0.0;
	int perf_frames = 0;
	uint64_t perf_max_cost = 0;
	uint64_t perf_sec[8] = {};
	uint64_t frame_sec[8] = {};
	void _log_hitch(double p_delta);
	void _sync_unfocused_speed();
	void _restore_unfocused_speed();
	bool unfocused_overridden = false;
};

} // namespace godot
