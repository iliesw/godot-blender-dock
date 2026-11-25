#include "blender_dock_plugin.h"

#include "win_embed.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_separator.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>

using namespace godot;

namespace {

const char *S_BLENDER_PATH = "blender_dock/blender_path";
const char *S_PORT = "blender_dock/link_port";
const char *S_FOLLOW_MOUSE = "blender_dock/focus_follows_mouse";
const char *P_BLEND_FILE = "blender_dock/blend_file";
const char *SCRIPT_PATH = "res://addons/blender_dock/blender/godot_link.py";

std::wstring to_w(const String &s) {
	Char16String u = s.utf16();
	return std::wstring(reinterpret_cast<const wchar_t *>(u.get_data()), u.length());
}

String from_w(const std::wstring &s) {
	return String::utf16(reinterpret_cast<const char16_t *>(s.c_str()), int(s.size()));
}

String quote(const String &s) {
	return "\"" + s + "\"";
}

} // namespace

BlenderDockPlugin::BlenderDockPlugin() :
		embed(std::make_unique<WinEmbed>()) {}

BlenderDockPlugin::~BlenderDockPlugin() = default; // WinEmbed's destructor hands Blender back as a normal window

void BlenderDockPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_launch_pressed"), &BlenderDockPlugin::_on_launch_pressed);
	ClassDB::bind_method(D_METHOD("_on_open_pressed"), &BlenderDockPlugin::_on_open_pressed);
	ClassDB::bind_method(D_METHOD("_on_popout_pressed"), &BlenderDockPlugin::_on_popout_pressed);
	ClassDB::bind_method(D_METHOD("_on_add_link_pressed"), &BlenderDockPlugin::_on_add_link_pressed);
	ClassDB::bind_method(D_METHOD("_on_sync_toggled", "on"), &BlenderDockPlugin::_on_sync_toggled);
	ClassDB::bind_method(D_METHOD("_on_file_selected", "path"), &BlenderDockPlugin::_on_file_selected);
}

// ---------------------------------------------------------------------------- settings

void BlenderDockPlugin::_ensure_settings() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	auto add = [&](const String &name, const Variant &def, PropertyHint hint, const String &hint_str) {
		if (!es->has_setting(name)) {
			es->set_setting(name, def);
		}
		es->set_initial_value(name, def, false);
		Dictionary info;
		info["name"] = name;
		info["type"] = def.get_type();
		info["hint"] = hint;
		info["hint_string"] = hint_str;
		es->add_property_info(info);
	};
	add(S_BLENDER_PATH, String(), PROPERTY_HINT_GLOBAL_FILE, "*.exe");
	add(S_PORT, 6789, PROPERTY_HINT_RANGE, "1024,65535,1");
	add(S_FOLLOW_MOUSE, true, PROPERTY_HINT_NONE, "");

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps->has_setting(P_BLEND_FILE)) {
		ps->set_setting(P_BLEND_FILE, String());
	}
	ps->set_initial_value(P_BLEND_FILE, String());
	Dictionary info;
	info["name"] = P_BLEND_FILE;
	info["type"] = Variant::STRING;
	info["hint"] = PROPERTY_HINT_GLOBAL_FILE;
	info["hint_string"] = "*.blend";
	ps->add_property_info(info);
}

Variant BlenderDockPlugin::_setting(const String &p_name, const Variant &p_default) const {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	return es.is_valid() && es->has_setting(p_name) ? es->get_setting(p_name) : p_default;
}

String BlenderDockPlugin::_find_blender() const {
	String configured = _setting(S_BLENDER_PATH, String());
	if (!configured.is_empty() && FileAccess::file_exists(configured)) {
		return configured;
	}
	// Newest "Blender X.Y" under Program Files.
	const String base = "C:/Program Files/Blender Foundation";
	PackedStringArray dirs = DirAccess::get_directories_at(base);
	String best;
	String best_dir;
	for (int i = 0; i < dirs.size(); i++) {
		String d = dirs[i];
		String exe = base.path_join(d).path_join("blender.exe");
		if (FileAccess::file_exists(exe) && (best_dir.is_empty() || d.naturalnocasecmp_to(best_dir) > 0)) {
			best_dir = d;
			best = exe;
		}
	}
	return best;
}

// ---------------------------------------------------------------------------- UI

void BlenderDockPlugin::_build_panel(Panel &p) {
	p.root = memnew(VBoxContainer);
	p.root->set_name("BlenderPanel");
	p.root->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	p.root->set_h_size_flags(Control::SIZE_EXPAND_FILL);

	HBoxContainer *bar = memnew(HBoxContainer);
	p.root->add_child(bar);

	auto button = [&](const String &text, const String &tip, const char *method) {
		Button *b = memnew(Button);
		b->set_text(text);
		b->set_tooltip_text(tip);
		b->set_flat(true);
		b->connect("pressed", Callable(this, method));
		bar->add_child(b);
		return b;
	};
	p.launch = button("Launch Blender", "Start Blender inside the editor (opens the project's .blend if set).", "_on_launch_pressed");
	p.open = button("Open .blend...", "Open a .blend file in the embedded Blender.", "_on_open_pressed");
	p.popout = button("Pop Out", "Move Blender to its own window / dock it back.", "_on_popout_pressed");
	bar->add_child(memnew(VSeparator));
	p.add_link = button("Link to Scene", "Add a BlenderLink node to the current 3D scene; Blender objects stream into it live.", "_on_add_link_pressed");
	p.sync = memnew(CheckBox);
	p.sync->set_text("Live Sync");
	p.sync->set_pressed(sync_enabled);
	p.sync->set_tooltip_text("Apply Blender changes to the scene in real time.");
	p.sync->connect("toggled", Callable(this, "_on_sync_toggled"));
	bar->add_child(p.sync);

	p.status = memnew(Label);
	p.status->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	p.status->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	p.status->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	p.status->set_modulate(Color(1, 1, 1, 0.65));
	bar->add_child(p.status);

	p.host = memnew(Control);
	p.host->set_name("BlenderHost");
	p.host->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	p.host->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	p.host->set_custom_minimum_size(Vector2(0, 260));
	p.host->set_clip_contents(true);
	p.root->add_child(p.host);

	p.placeholder = memnew(Label);
	p.placeholder->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	p.placeholder->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	p.placeholder->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	p.placeholder->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	p.placeholder->set_modulate(Color(1, 1, 1, 0.5));
	p.host->add_child(p.placeholder);
}

void BlenderDockPlugin::_enter_tree() {
	_ensure_settings();

	_build_panel(main_panel);
	EditorInterface::get_singleton()->get_editor_main_screen()->add_child(main_panel.root);
	main_panel.root->hide();

	_build_panel(dock_panel);
	dock = memnew(EditorDock);
	dock->set_title("Blender");
	dock->set_layout_key("BlenderDock");
	dock->set_default_slot(EditorDock::DOCK_SLOT_BOTTOM);
	dock->set_available_layouts(EditorDock::DOCK_LAYOUT_ALL);
	Ref<Texture2D> icon = _get_plugin_icon();
	if (icon.is_valid()) {
		dock->set_dock_icon(icon);
	}
	dock->add_child(dock_panel.root);
	add_dock(dock);

	file_dialog = memnew(EditorFileDialog);
	file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
	file_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	file_dialog->add_filter("*.blend", "Blender file");
	file_dialog->set_title("Open .blend");
	file_dialog->connect("file_selected", Callable(this, "_on_file_selected"));
	EditorInterface::get_singleton()->get_base_control()->add_child(file_dialog);

	link.instantiate();
	link->start(int(_setting(S_PORT, 6789)));

	set_process(true);
	_update_ui();

	// Test / power-user hook: BLENDER_DOCK_AUTOLAUNCH=1 starts Blender with the editor.
	if (OS::get_singleton()->get_environment("BLENDER_DOCK_AUTOLAUNCH") == "1") {
		callable_mp(this, &BlenderDockPlugin::_on_launch_pressed).call_deferred();
		pending_auto_link = true;
	}
}

void BlenderDockPlugin::_exit_tree() {
	set_process(false);
	// Leave Blender running as a normal window: never risk unsaved work.
	embed->detach();
	if (link.is_valid()) {
		link->stop();
		link.unref();
	}
	if (dock) {
		remove_dock(dock);
		dock->queue_free();
		dock = nullptr;
	}
	if (main_panel.root) {
		main_panel.root->queue_free();
		main_panel = Panel();
	}
	dock_panel = Panel();
	if (file_dialog) {
		file_dialog->queue_free();
		file_dialog = nullptr;
	}
}

void BlenderDockPlugin::_make_visible(bool p_visible) {
	main_visible = p_visible;
	if (main_panel.root) {
		main_panel.root->set_visible(p_visible);
	}
}

Ref<Texture2D> BlenderDockPlugin::_get_plugin_icon() const {
	if (blender_icon.is_valid()) {
		return blender_icon;
	}
	// Blender's own application icon, read from blender.exe (no logo file shipped with the add-on).
	String exe = _find_blender();
	if (!exe.is_empty()) {
		int size = int(std::round(16.0 * EditorInterface::get_singleton()->get_editor_scale()));
		std::vector<uint8_t> rgba;
		if (extract_exe_icon_rgba(to_w(exe.replace("/", "\\")), size, rgba)) {
			PackedByteArray data;
			data.resize(int64_t(rgba.size()));
			memcpy(data.ptrw(), rgba.data(), rgba.size());
			Ref<Image> img = Image::create_from_data(size, size, false, Image::FORMAT_RGBA8, data);
			blender_icon = ImageTexture::create_from_image(img);
			return blender_icon;
		}
	}
	Ref<Theme> theme = EditorInterface::get_singleton()->get_editor_theme();
	return theme.is_valid() ? theme->get_icon("MeshInstance3D", "EditorIcons") : Ref<Texture2D>();
}

// The editor tints main-screen tab icons with the theme font color, which turns
// Blender's orange logo olive. Find our tab button and show the icon untinted.
bool BlenderDockPlugin::_untint_tab_icon(Node *p_node) {
	if (!p_node) {
		return false;
	}
	Button *b = Object::cast_to<Button>(p_node);
	if (b && b->get_button_icon() == blender_icon && b != (dock_panel.launch)) {
		for (const char *c : { "icon_normal_color", "icon_hover_color", "icon_pressed_color", "icon_hover_pressed_color",
					 "icon_focus_color" }) {
			b->add_theme_color_override(c, Color(1, 1, 1));
		}
		b->add_theme_color_override("icon_disabled_color", Color(1, 1, 1, 0.5));
		return true;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		if (_untint_tab_icon(p_node->get_child(i))) {
			return true;
		}
	}
	return false;
}

Control *BlenderDockPlugin::_active_host() {
	if (main_visible && main_panel.host && main_panel.host->is_visible_in_tree()) {
		return main_panel.host;
	}
	if (dock_panel.host && dock_panel.host->is_visible_in_tree()) {
		return dock_panel.host;
	}
	return nullptr;
}

void BlenderDockPlugin::_update_ui() {
	bool running = embed->is_running();
	bool window = running && embed->has_window();

	String blender = !running ? String("Blender: stopped")
							  : (window ? String("Blender: running") : String("Blender: starting..."));
	String linkst;
	if (!link.is_valid() || !link->is_listening()) {
		linkst = "Link: off (port busy?)";
	} else if (link->is_peer_connected()) {
		linkst = vformat("Link: live | %d objects", link->get_object_count());
	} else {
		linkst = vformat("Link: waiting on :%d", link->get_port());
	}
	String scene = link.is_valid() && link->edited_scene_has_link() ? String(" | scene linked") : String();
	String status = blender + " | " + linkst + scene;
	if (!last_error.is_empty()) {
		status = last_error;
	}

	Control *active = _active_host();
	for (Panel *p : { &main_panel, &dock_panel }) {
		if (!p->root) {
			continue;
		}
		p->status->set_text(status);
		p->status->set_tooltip_text(link.is_valid() ? link->get_blend_file() : String());
		p->launch->set_disabled(running);
		p->popout->set_disabled(!window);
		p->popout->set_text(popped_out ? "Dock" : "Pop Out");
		p->add_link->set_disabled(!link.is_valid() || link->edited_scene_has_link());
		p->sync->set_pressed_no_signal(sync_enabled);

		String ph;
		if (!running) {
			ph = "Blender is not running.\nPress \"Launch Blender\".";
		} else if (!window) {
			ph = "Starting Blender...";
		} else if (popped_out) {
			ph = "Blender is in its own window.\nPress \"Dock\" to bring it back.";
		} else if (p->host != active) {
			ph = "Blender is shown in the other Blender panel.";
		}
		p->placeholder->set_text(ph);
		p->placeholder->set_visible(!ph.is_empty());
	}
}

// ---------------------------------------------------------------------------- loop

void BlenderDockPlugin::_process(double p_delta) {
	if (link.is_valid()) {
		link->poll(sync_enabled);
	}

	if (embed->is_running()) {
		find_cooldown -= p_delta;
		if (!embed->has_window() && find_cooldown <= 0.0) {
			find_cooldown = 0.1;
			embed->find_window();
		}
		if (embed->has_window() && !popped_out) {
			Control *host = _active_host();
			Vector2 size = host ? host->get_size() : Vector2();
			if (host && size.x >= 8 && size.y >= 8 && host->get_window() &&
					host->get_window()->get_mode() != Window::MODE_MINIMIZED) {
				int64_t parent = DisplayServer::get_singleton()->window_get_native_handle(
						DisplayServer::WINDOW_HANDLE, host->get_window()->get_window_id());
				embed->embed(intptr_t(parent));
				Vector2 pos = host->get_screen_position();
				EmbedRect r;
				r.x = int(std::floor(pos.x));
				r.y = int(std::floor(pos.y));
				r.w = int(std::ceil(size.x));
				r.h = int(std::ceil(size.y));
				embed->set_rect(r);
				embed->set_visible(true);

				Control *focus = EditorInterface::get_singleton()->get_base_control()->get_viewport()->gui_get_focus_owner();
				bool text = Object::cast_to<LineEdit>(focus) || Object::cast_to<TextEdit>(focus);
				embed->update_focus(bool(_setting(S_FOLLOW_MOUSE, true)), text);
			} else {
				embed->set_visible(false);
			}
		}
	}

	if (!tab_icon_fixed && blender_icon.is_valid()) {
		tab_icon_fixed = _untint_tab_icon(EditorInterface::get_singleton()->get_base_control()->get_parent());
	}

	if (pending_auto_link &&Object::cast_to<Node3D>(EditorInterface::get_singleton()->get_edited_scene_root())) {
		pending_auto_link = false;
		link->add_to_edited_scene();
	}

	ui_cooldown -= p_delta;
	if (ui_cooldown <= 0.0) {
		ui_cooldown = 0.25;
		_update_ui();
	}
}

// ---------------------------------------------------------------------------- actions

void BlenderDockPlugin::_launch(const String &p_blend) {
	last_error = "";
	String exe = _find_blender();
	if (exe.is_empty()) {
		last_error = "Blender not found. Set Editor Settings > Blender Dock > Blender Path.";
		_update_ui();
		return;
	}
	String script = ProjectSettings::get_singleton()->globalize_path(SCRIPT_PATH);
	String args;
	if (!p_blend.is_empty()) {
		args += quote(p_blend) + " ";
	}
	args += "--no-window-focus --python " + quote(script) + " -- --godot-port " + itos(int(_setting(S_PORT, 6789)));

	std::wstring err;
	popped_out = false;
	if (!embed->launch(to_w(exe), to_w(args), to_w(exe.get_base_dir()), err)) {
		last_error = "Could not start Blender: " + from_w(err).strip_edges();
	}
	find_cooldown = 0.0;
	_update_ui();
}

void BlenderDockPlugin::_on_launch_pressed() {
	String blend = ProjectSettings::get_singleton()->get_setting(P_BLEND_FILE, String());
	if (!blend.is_empty()) {
		blend = ProjectSettings::get_singleton()->globalize_path(blend);
		if (!FileAccess::file_exists(blend)) {
			blend = "";
		}
	}
	_launch(blend);
	// Bring a Blender panel into view.
	if (!_active_host()) {
		EditorInterface::get_singleton()->set_main_screen_editor("Blender");
	}
}

void BlenderDockPlugin::_on_open_pressed() {
	file_dialog->popup_file_dialog();
}

void BlenderDockPlugin::_on_file_selected(const String &p_path) {
	String path = ProjectSettings::get_singleton()->globalize_path(p_path);
	ProjectSettings::get_singleton()->set_setting(P_BLEND_FILE, ProjectSettings::get_singleton()->localize_path(path));
	ProjectSettings::get_singleton()->save();
	if (embed->is_running() && link->is_peer_connected()) {
		Dictionary d;
		d["t"] = "open";
		d["path"] = path;
		link->send(d);
	} else if (!embed->is_running()) {
		_launch(path);
	} else {
		last_error = "Blender is running but the link is not connected yet; open the file from Blender.";
	}
	_update_ui();
}

void BlenderDockPlugin::_on_popout_pressed() {
	popped_out = !popped_out;
	if (popped_out) {
		embed->detach();
	}
	_update_ui();
}

void BlenderDockPlugin::_on_add_link_pressed() {
	if (!link->add_to_edited_scene()) {
		last_error = "Open a 3D scene (Node3D root) first.";
	} else {
		last_error = "";
	}
	_update_ui();
}

void BlenderDockPlugin::_on_sync_toggled(bool p_on) {
	sync_enabled = p_on;
	_update_ui();
}
