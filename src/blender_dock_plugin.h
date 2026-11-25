#pragma once

#include "blender_link.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/editor_dock.hpp>
#include <godot_cpp/classes/editor_file_dialog.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include <memory>

class WinEmbed;

namespace godot {

// One Blender process, shown in whichever host is visible:
//   - the "Blender" main-screen tab (next to 2D / 3D / Script), or
//   - the "Blender" dock (bottom panel by default; can be moved to a side or floated),
// so Blender can sit right under Godot's 3D viewport while you work.
class BlenderDockPlugin : public EditorPlugin {
	GDCLASS(BlenderDockPlugin, EditorPlugin)

public:
	BlenderDockPlugin();
	~BlenderDockPlugin();

	void _enter_tree() override;
	void _exit_tree() override;
	void _process(double p_delta) override;

	bool _has_main_screen() const override { return true; }
	void _make_visible(bool p_visible) override;
	String _get_plugin_name() const override { return "Blender"; }
	Ref<Texture2D> _get_plugin_icon() const override;

protected:
	static void _bind_methods();

private:
	struct Panel {
		VBoxContainer *root = nullptr;
		Control *host = nullptr; // Blender's window is laid over this rect
		Label *placeholder = nullptr;
		Button *launch = nullptr;
		Button *open = nullptr;
		Button *popout = nullptr;
		Button *add_link = nullptr;
		CheckBox *sync = nullptr;
		Label *status = nullptr;
	};

	void _build_panel(Panel &p);
	void _launch(const String &p_blend);
	String _find_blender() const;
	Control *_active_host();
	void _update_ui();
	void _ensure_settings();
	Variant _setting(const String &p_name, const Variant &p_default) const;

	void _on_launch_pressed();
	void _on_open_pressed();
	void _on_popout_pressed();
	void _on_add_link_pressed();
	void _on_sync_toggled(bool p_on);
	void _on_file_selected(const String &p_path);

	std::unique_ptr<WinEmbed> embed;
	Ref<BlenderLink> link;

	Panel main_panel;
	Panel dock_panel;
	EditorDock *dock = nullptr;
	EditorFileDialog *file_dialog = nullptr;

	bool main_visible = false;
	bool popped_out = false;
	bool sync_enabled = true;
	bool pending_auto_link = false;
	double find_cooldown = 0.0;
	double ui_cooldown = 0.0;
	String last_error;
	mutable Ref<Texture2D> blender_icon;
	bool tab_icon_fixed = false;
	bool _untint_tab_icon(Node *p_node);
};

} // namespace godot
