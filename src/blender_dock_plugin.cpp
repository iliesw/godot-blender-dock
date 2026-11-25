#include "blender_dock_plugin.h"

#include "win_embed.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/center_container.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_flow_container.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/v_separator.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <cstring>

using namespace godot;

namespace {

const char *S_BLENDER_PATH = "blender_dock/blender_path";
const char *S_PORT = "blender_dock/link_port";
const char *S_FOLLOW_MOUSE = "blender_dock/hover_gives_focus"; // opt-in: switching focus on hover costs a window activation
const char *S_AUTO_LAUNCH = "blender_dock/auto_launch";
const char *S_MATCH_THEME = "blender_dock/match_godot_theme";
const char *S_HIDE_STATUSBAR = "blender_dock/hide_blender_status_bar";
const char *S_RECOVERY = "blender_dock/recovery_interval_seconds";
const char *S_KEEP_SPLASH = "blender_dock/show_blender_splash";
const char *S_HITCH_LOG = "blender_dock/log_hitches";
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

PackedFloat32Array xform_list(const Transform3D &t) {
	PackedFloat32Array a;
	a.resize(12);
	float *w = a.ptrw();
	for (int c = 0; c < 3; c++) {
		Vector3 col = t.basis.get_column(c);
		w[c * 3 + 0] = col.x;
		w[c * 3 + 1] = col.y;
		w[c * 3 + 2] = col.z;
	}
	w[9] = t.origin.x;
	w[10] = t.origin.y;
	w[11] = t.origin.z;
	return a;
}

void ensure_ignored_dir(const String &p_res_dir) {
	String abs = ProjectSettings::get_singleton()->globalize_path(p_res_dir);
	DirAccess::make_dir_recursive_absolute(abs);
	String ignore = abs.path_join(".gdignore");
	if (!FileAccess::file_exists(ignore)) {
		Ref<FileAccess> f = FileAccess::open(ignore, FileAccess::WRITE);
	}
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
	ClassDB::bind_method(D_METHOD("_on_bake_pressed"), &BlenderDockPlugin::_on_bake_pressed);
	ClassDB::bind_method(D_METHOD("_on_settings_pressed"), &BlenderDockPlugin::_on_settings_pressed);
	ClassDB::bind_method(D_METHOD("_on_settings_confirmed"), &BlenderDockPlugin::_on_settings_confirmed);
	ClassDB::bind_method(D_METHOD("_on_browse_blender"), &BlenderDockPlugin::_on_browse_blender);
	ClassDB::bind_method(D_METHOD("_on_blender_path_selected", "path"), &BlenderDockPlugin::_on_blender_path_selected);
	ClassDB::bind_method(D_METHOD("_on_sync_toggled", "on"), &BlenderDockPlugin::_on_sync_toggled);
	ClassDB::bind_method(D_METHOD("_on_follow_toggled", "on"), &BlenderDockPlugin::_on_follow_toggled);
	ClassDB::bind_method(D_METHOD("_on_file_selected", "path"), &BlenderDockPlugin::_on_file_selected);
	ClassDB::bind_method(D_METHOD("_on_selection_changed"), &BlenderDockPlugin::_on_selection_changed);
	ClassDB::bind_method(D_METHOD("_on_recover_confirmed"), &BlenderDockPlugin::_on_recover_confirmed);
	ClassDB::bind_method(D_METHOD("_on_recover_canceled"), &BlenderDockPlugin::_on_recover_canceled);
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
	add(S_FOLLOW_MOUSE, false, PROPERTY_HINT_NONE, "");
	add(S_HITCH_LOG, true, PROPERTY_HINT_NONE, "");
	add(S_AUTO_LAUNCH, false, PROPERTY_HINT_NONE, "");
	add(S_MATCH_THEME, true, PROPERTY_HINT_NONE, "");
	add(S_HIDE_STATUSBAR, true, PROPERTY_HINT_NONE, "");
	add(S_RECOVERY, 30, PROPERTY_HINT_RANGE, "0,600,1,suffix:s");
	add(S_KEEP_SPLASH, false, PROPERTY_HINT_NONE, "");

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
	// Test hook: run a second editor without fighting over the port.
	if (p_name == S_PORT) {
		String env_port = OS::get_singleton()->get_environment("BLENDER_DOCK_PORT");
		if (env_port.is_valid_int()) {
			return env_port.to_int();
		}
	}
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	return es.is_valid() && es->has_setting(p_name) ? es->get_setting(p_name) : p_default;
}

String BlenderDockPlugin::_find_blender() const {
	String configured = _setting(S_BLENDER_PATH, String());
	if (!configured.is_empty() && FileAccess::file_exists(configured)) {
		return configured;
	}
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

String BlenderDockPlugin::_recovery_path() const {
	return OS::get_singleton()->get_user_data_dir().path_join("blender_dock_recovery.blend");
}

String BlenderDockPlugin::_default_blend_path() const {
	String blend = ProjectSettings::get_singleton()->get_setting(P_BLEND_FILE, String());
	if (!blend.is_empty()) {
		return ProjectSettings::get_singleton()->globalize_path(blend);
	}
	String scene = "main";
	if (Node *root = EditorInterface::get_singleton()->get_edited_scene_root()) {
		if (!root->get_scene_file_path().is_empty()) {
			scene = root->get_scene_file_path().get_file().get_basename();
		}
	}
	return ProjectSettings::get_singleton()->globalize_path("res://blender").path_join(scene + ".blend");
}

// ---------------------------------------------------------------------------- UI

void BlenderDockPlugin::_build_panel(Panel &p) {
	p.root = memnew(VBoxContainer);
	p.root->set_name("BlenderPanel");
	p.root->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	p.root->set_h_size_flags(Control::SIZE_EXPAND_FILL);

	HFlowContainer *bar = memnew(HFlowContainer); // wraps when the dock is narrow
	p.root->add_child(bar);
	Ref<Theme> theme = EditorInterface::get_singleton()->get_editor_theme();

	auto button = [&](const String &text, const String &icon, const String &tip, const char *method) {
		Button *b = memnew(Button);
		b->set_text(text);
		if (!icon.is_empty() && theme.is_valid() && theme->has_icon(icon, "EditorIcons")) {
			b->set_button_icon(theme->get_icon(icon, "EditorIcons"));
		}
		b->set_tooltip_text(tip);
		b->set_flat(true);
		b->connect("pressed", Callable(this, method));
		bar->add_child(b);
		return b;
	};
	auto check = [&](const String &text, bool on, const String &tip, const char *method) {
		CheckBox *c = memnew(CheckBox);
		c->set_text(text);
		c->set_pressed(on);
		c->set_tooltip_text(tip);
		c->connect("toggled", Callable(this, method));
		bar->add_child(c);
		return c;
	};
	p.launch = button("Launch", "Play", "Start Blender inside the editor (opens the project's .blend if set).", "_on_launch_pressed");
	p.open = button("Open .blend", "Load", "Open a .blend file in the embedded Blender.", "_on_open_pressed");
	p.popout = button("Pop Out", "MakeFloating", "Move Blender to its own window / dock it back.", "_on_popout_pressed");
	bar->add_child(memnew(VSeparator));
	p.add_link = button("Link to Scene", "Node3D", "Add a BlenderLink node to the current 3D scene; Blender objects stream into it live.", "_on_add_link_pressed");
	p.sync = check("Live Sync", sync_enabled, "Apply Blender changes to the scene in real time.", "_on_sync_toggled");
	p.follow = check("Follow View", follow_view, "Blender's 3D view follows Godot's 3D editor camera.", "_on_follow_toggled");
	p.bake = button("Bake glTF", "PackedScene", "Export Blender's selection (rigs, skinning, shape keys, animations) as .glb into res://blender_assets.", "_on_bake_pressed");
	p.settings = button("", "Tools", "Blender Dock settings", "_on_settings_pressed");

	p.status = memnew(Label);
	p.status->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	p.status->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_RIGHT);
	p.status->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	p.status->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
	p.status->set_custom_minimum_size(Vector2(120, 0));
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

	CenterContainer *center = memnew(CenterContainer);
	center->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	center->hide();
	VBoxContainer *box = memnew(VBoxContainer);
	box->set_custom_minimum_size(Vector2(320, 0));
	center->add_child(box);
	p.loading_label = memnew(Label);
	p.loading_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	box->add_child(p.loading_label);
	p.loading_bar = memnew(ProgressBar);
	p.loading_bar->set_min(0);
	p.loading_bar->set_max(100);
	p.loading_bar->set_custom_minimum_size(Vector2(320, 0));
	box->add_child(p.loading_bar);
	p.host->add_child(center);
	p.loading = center;
}

void BlenderDockPlugin::_build_settings() {
	SettingsUI &s = settings;
	s.dialog = memnew(AcceptDialog);
	s.dialog->set_title("Blender Dock Settings");
	s.dialog->set_ok_button_text("Save");
	s.dialog->add_cancel_button("Cancel");
	s.dialog->connect("confirmed", Callable(this, "_on_settings_confirmed"));

	GridContainer *grid = memnew(GridContainer);
	grid->set_columns(2);
	grid->set_custom_minimum_size(Vector2(560, 0));
	s.dialog->add_child(grid);

	auto label = [&](const String &t) {
		Label *l = memnew(Label);
		l->set_text(t);
		grid->add_child(l);
	};
	label("Blender executable");
	HBoxContainer *row = memnew(HBoxContainer);
	row->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	s.path = memnew(LineEdit);
	s.path->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	s.path->set_placeholder("Auto-detect newest Blender in Program Files");
	row->add_child(s.path);
	Button *browse = memnew(Button);
	browse->set_text("Browse...");
	browse->connect("pressed", Callable(this, "_on_browse_blender"));
	row->add_child(browse);
	grid->add_child(row);

	label("Link port");
	s.port = memnew(SpinBox);
	s.port->set_min(1024);
	s.port->set_max(65535);
	grid->add_child(s.port);

	label("Recovery copy every");
	s.recovery = memnew(SpinBox);
	s.recovery->set_min(0);
	s.recovery->set_max(600);
	s.recovery->set_suffix("s (0 = off)");
	grid->add_child(s.recovery);

	auto flag = [&](const String &title, const String &text, CheckBox *&r_box) {
		label(title);
		r_box = memnew(CheckBox);
		r_box->set_text(text);
		grid->add_child(r_box);
	};
	flag("Startup", "Launch Blender when the editor opens", s.auto_launch);
	flag("Keyboard focus", "Hovering gives focus (default: click, like normal windows)", s.follow_mouse);
	flag("Look", "Match Blender's colors to the Godot theme", s.theme);
	flag("", "Hide Blender's status bar while docked", s.statusbar);
	flag("", "Show Blender's splash screen", s.splash);

	s.browse = memnew(EditorFileDialog);
	s.browse->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
	s.browse->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	s.browse->add_filter("blender.exe", "Blender");
	s.browse->connect("file_selected", Callable(this, "_on_blender_path_selected"));
	s.dialog->add_child(s.browse);

	EditorInterface::get_singleton()->get_base_control()->add_child(s.dialog);
}

void BlenderDockPlugin::_enter_tree() {
	_ensure_settings();
	_restore_unfocused_speed();

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
	dock->set_closable(false); // always keep the "Blender" tab in its slot (it can still be collapsed)
	dock->add_child(dock_panel.root);
	add_dock(dock);
	// A layout saved while the dock was closed would otherwise hide it for good.
	callable_mp(dock, &EditorDock::open).call_deferred();

	file_dialog = memnew(EditorFileDialog);
	file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
	file_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	file_dialog->add_filter("*.blend", "Blender file");
	file_dialog->set_title("Open .blend");
	file_dialog->connect("file_selected", Callable(this, "_on_file_selected"));
	EditorInterface::get_singleton()->get_base_control()->add_child(file_dialog);

	recover_dialog = memnew(ConfirmationDialog);
	recover_dialog->set_title("Recover Blender work?");
	recover_dialog->set_text("Blender did not close cleanly last time (the editor may have crashed).\n"
							 "A recovery copy of your unsaved Blender work exists.\n\nOpen the recovered copy?");
	recover_dialog->set_ok_button_text("Open Recovered Copy");
	recover_dialog->set_cancel_button_text("Discard It");
	recover_dialog->connect("confirmed", Callable(this, "_on_recover_confirmed"));
	recover_dialog->connect("canceled", Callable(this, "_on_recover_canceled"));
	EditorInterface::get_singleton()->get_base_control()->add_child(recover_dialog);

	_build_settings();

	EditorInterface::get_singleton()->get_selection()->connect("selection_changed", Callable(this, "_on_selection_changed"));

	link.instantiate();
	link->start(int(_setting(S_PORT, 6789)));

	set_process(true);
	_update_ui();

	bool env_auto = OS::get_singleton()->get_environment("BLENDER_DOCK_AUTOLAUNCH") == "1";
	if (env_auto || bool(_setting(S_AUTO_LAUNCH, false))) {
		callable_mp(this, &BlenderDockPlugin::_on_launch_pressed).call_deferred();
		pending_auto_link = env_auto;
	}
	String perf_env = OS::get_singleton()->get_environment("BLENDER_DOCK_PERFTEST");
	if (perf_env == "1" || perf_env == "focus") {
		perf_step = 0;
		perf_focus_mode = perf_env == "focus";
	}
	if (OS::get_singleton()->get_environment("BLENDER_DOCK_SELFTEST") == "1") {
		selftest_step = 0;
	}
}

void BlenderDockPlugin::_exit_tree() {
	set_process(false);
	_restore_unfocused_speed();
	EditorSelection *sel = EditorInterface::get_singleton()->get_selection();
	if (sel && sel->is_connected("selection_changed", Callable(this, "_on_selection_changed"))) {
		sel->disconnect("selection_changed", Callable(this, "_on_selection_changed"));
	}
	// Close together: Blender saves its file (like Ctrl+S) and quits with the editor.
	// Without a live link Blender cannot be told to save, so it is handed back as a normal
	// window instead (never risk unsaved work).
	if (embed->is_running() && link.is_valid() && link->is_peer_connected()) {
		embed->release_for_quit();
		String path = _default_blend_path();
		if (link->get_blend_file().is_empty()) {
			ensure_ignored_dir(ProjectSettings::get_singleton()->localize_path(path.get_base_dir()));
		}
		if (!link->send_save_quit(path, 2000)) {
			// Blender did not confirm: its watchdog will pop it out as a normal window (nothing lost).
			UtilityFunctions::print("Blender Dock: Blender will be shown as a normal window instead.");
		}
	} else {
		UtilityFunctions::print("Blender Dock: no live link at exit; handing Blender back as a normal window.");
		embed->detach();
	}
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
	}
	main_panel = Panel();
	dock_panel = Panel();
	for (Node *n : { (Node *)file_dialog, (Node *)recover_dialog, (Node *)settings.dialog }) {
		if (n) {
			n->queue_free();
		}
	}
	file_dialog = nullptr;
	recover_dialog = nullptr;
	settings = SettingsUI();
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
	if (b && b->get_button_icon() == blender_icon) {
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
	bool connected = link.is_valid() && link->is_peer_connected();

	String blender = !running ? String("Blender: stopped")
							  : (!window ? String("Blender: starting...")
										 : (embed->is_blender_busy() ? String("Blender: busy") : String("Blender: running")));
	String linkst;
	if (!link.is_valid() || !link->is_listening()) {
		linkst = "Link: off (port busy?)";
	} else if (connected) {
		linkst = vformat("Link: live, %d objects", link->get_object_count());
	} else {
		linkst = vformat("Link: waiting on :%d", link->get_port());
	}
	String scene = link.is_valid() && link->edited_scene_has_link() ? String(" | scene linked") : String();
	String status = blender + " | " + linkst + scene;
	double now = Time::get_singleton()->get_ticks_msec() / 1000.0;
	if (!notice.is_empty() && now - notice_time < 6.0) {
		status = notice;
	}
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
		p->bake->set_disabled(!connected);
		p->sync->set_pressed_no_signal(sync_enabled);
		p->follow->set_pressed_no_signal(follow_view);
		p->follow->set_disabled(!connected);

		String ph;
		if (!running) {
			ph = "Blender is not running.\nPress \"Launch\".";
		} else if (!blender_ready) {
			ph = ""; // progress bar shows instead
		} else if (popped_out) {
			ph = "Blender is in its own window.\nPress \"Dock\" to bring it back.";
		} else if (p->host != active) {
			ph = "Blender is shown in the other Blender panel.";
		}
		p->placeholder->set_text(ph);
		p->placeholder->set_visible(!ph.is_empty());
	}
}

// Real startup stages, smoothed so the bar keeps moving inside a stage:
//   launched 0-30%  -> window exists 30-60% -> add-on connected 60-90% -> scene synced 100%.
void BlenderDockPlugin::_update_startup(double p_delta) {
	bool running = embed->is_running();
	if (!running) {
		blender_ready = false;
		window_elapsed = -1.0;
		launch_elapsed = 0.0;
		loading_value = 0.0;
	}
	bool show = running && !blender_ready;
	if (show) {
		launch_elapsed += p_delta;
		bool window = embed->has_window();
		if (window && window_elapsed < 0.0) {
			window_elapsed = 0.0;
		}
		if (window_elapsed >= 0.0) {
			window_elapsed += p_delta;
		}
		bool connected = link.is_valid() && link->is_peer_connected();
		double floor_v, cap;
		if (!window) {
			loading_text = "Starting Blender...";
			floor_v = 0.0;
			cap = 30.0;
		} else if (!connected) {
			loading_text = "Loading Blender...";
			floor_v = 30.0;
			cap = 60.0;
		} else {
			loading_text = "Syncing scene...";
			floor_v = 60.0;
			cap = 90.0;
		}
		// Ease toward the stage cap (never reaching it until the stage really completes).
		loading_value = MAX(loading_value, floor_v);
		loading_value += (cap - loading_value) * MIN(1.0, p_delta * 1.2);
		if (connected && link->has_scene()) {
			blender_ready = true; // fully loaded and synced: reveal Blender
		} else if (window_elapsed > 25.0) {
			blender_ready = true; // add-on never connected: show Blender anyway rather than hang
			last_error = "Blender started, but the Godot link did not connect (check the Blender console).";
		}
		if (blender_ready) {
			loading_value = 100.0;
			embed->set_hold_hidden(false); // revealed by the normal embed/visibility path next frame
		}
	}
	for (Panel *p : { &main_panel, &dock_panel }) {
		if (!p->loading) {
			continue;
		}
		p->loading->set_visible(show && !blender_ready);
		if (show) {
			p->loading_label->set_text(loading_text);
			p->loading_bar->set_value(loading_value);
		}
	}
}

EmbedRect BlenderDockPlugin::_launch_rect() {
	// Blender's -p takes x from the left and y from the BOTTOM of the screen. Starting it at the
	// panel position (and behind Godot, --no-window-focus) means it is never seen elsewhere.
	Control *host = _active_host();
	if (!host) {
		host = main_panel.host;
	}
	Vector2 pos = host->get_screen_position();
	Vector2 size = host->get_size();
	if (size.x < 64 || size.y < 64) {
		Window *w = host->get_window();
		pos = Vector2(w->get_position()) + Vector2(64, 64);
		size = Vector2(800, 600);
	}
	DisplayServer *ds = DisplayServer::get_singleton();
	Rect2i usable = ds->screen_get_usable_rect(ds->window_get_current_screen());
	EmbedRect r;
	r.x = int(pos.x);
	r.w = int(size.x);
	r.h = int(size.y);
	r.y = MAX(0, usable.position.y + usable.size.y - int(pos.y + size.y));
	return r;
}

// ---------------------------------------------------------------------------- loop

void BlenderDockPlugin::_process(double p_delta) {
	uint64_t perf_t0 = Time::get_singleton()->get_ticks_usec();
	_log_hitch(p_delta);
	_process_inner(p_delta);
	if (perf_step >= 0) {
		_perf_test(p_delta, Time::get_singleton()->get_ticks_usec() - perf_t0);
	}
}

// BLENDER_DOCK_PERFTEST=1: flip between the 3D and Blender screens and log frame gaps / plugin cost.
void BlenderDockPlugin::_perf_test(double p_delta, uint64_t p_cost_us) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (perf_step == 0) {
		if (link->is_peer_connected() && embed->is_embedded() && blender_ready && link->get_object_count() >= 3) {
			OS::get_singleton()->set_low_processor_usage_mode(false); // continuous frames: gaps = stalls
			perf_step = 1;
			perf_time = 0;
			if (perf_focus_mode) {
				ei->set_main_screen_editor("3D"); // Blender in the bottom dock, under the 3D view
				dock->make_visible();
			} else {
				ei->set_main_screen_editor("Blender");
			}
		}
		return;
	}
	perf_time += p_delta;
	perf_frames++;
	perf_max_gap = MAX(perf_max_gap, p_delta);
	perf_sum_gap += p_delta;
	perf_max_cost = MAX(perf_max_cost, p_cost_us);
	if (perf_time > 1.5) {
		String label = perf_focus_mode ? String(embed->blender_has_focus() ? "focus=Blender" : (this_process_is_foreground() ? "focus=Godot" : "focus=other"))
										: String(main_visible ? "Blender" : "3D");
		UtilityFunctions::print(vformat("PERF phase %d (%s): frames %d, avg %.1f ms, worst gap %.1f ms, worst plugin cost %.2f ms",
				perf_step, label, perf_frames, 1000.0 * perf_sum_gap / MAX(1, perf_frames),
				1000.0 * perf_max_gap, perf_max_cost / 1000.0));
		UtilityFunctions::print(vformat("PERF   sections ms: link %.1f find %.1f embed %.1f rect/vis %.1f focus %.1f msgs %.1f ui %.1f",
				perf_sec[0] / 1000.0, perf_sec[1] / 1000.0, perf_sec[2] / 1000.0, perf_sec[3] / 1000.0, perf_sec[4] / 1000.0,
				perf_sec[5] / 1000.0, perf_sec[6] / 1000.0));
		for (uint64_t &v : perf_sec) {
			v = 0;
		}
		perf_frames = 0;
		perf_max_gap = perf_sum_gap = 0;
		perf_max_cost = 0;
		perf_time = 0;
		perf_step++;
		if (perf_step > 10) {
			UtilityFunctions::print("PERF done");
			OS::get_singleton()->set_low_processor_usage_mode(true);
			perf_step = -1;
			return;
		}
		if (perf_focus_mode) {
			if (perf_step % 2) {
				embed->focus_blender_now();
			} else {
				embed->focus_godot();
			}
		} else {
			ei->set_main_screen_editor((perf_step % 2) ? "Blender" : "3D");
		}
	}
}

#define PERF_MARK(i)                                                                   \
	do {                                                                               \
		uint64_t _n = Time::get_singleton()->get_ticks_usec();                         \
		frame_sec[i] = _n - _perf_t;                                                   \
		if (perf_step > 0) {                                                           \
			perf_sec[i] = MAX(perf_sec[i], _n - _perf_t);                              \
		}                                                                              \
		_perf_t = _n;                                                                  \
	} while (0)

// Diagnostics: any frame gap over 50 ms is written to user://blender_dock_hitches.log together
// with the previous frame's plugin section times and who had focus, so lag reports have data.
void BlenderDockPlugin::_log_hitch(double p_delta) {
	if (!bool(_setting(S_HITCH_LOG, true)) || !embed->is_running() || !blender_ready || p_delta < 0.05) {
		return;
	}
	String who = embed->blender_has_focus() ? "Blender" : (this_process_is_foreground() ? "Godot" : "other app");
	if (who == "other app" && !unfocused_overridden) {
		return; // throttling while another app is active is normal; not a lag
	}
	String line = vformat("%s gap %.0f ms | focus %s | screen %s | plugin ms: link %.1f find %.1f embed %.1f rect %.1f focus %.1f msgs %.1f ui %.1f",
			Time::get_singleton()->get_time_string_from_system(), p_delta * 1000.0, who, main_visible ? "Blender" : "other",
			frame_sec[0] / 1000.0, frame_sec[1] / 1000.0, frame_sec[2] / 1000.0, frame_sec[3] / 1000.0, frame_sec[4] / 1000.0,
			frame_sec[5] / 1000.0, frame_sec[6] / 1000.0);
	String path = OS::get_singleton()->get_user_data_dir().path_join("blender_dock_hitches.log");
	Ref<FileAccess> f = FileAccess::file_exists(path) ? FileAccess::open(path, FileAccess::READ_WRITE) : FileAccess::open(path, FileAccess::WRITE);
	if (f.is_valid()) {
		f->seek_end();
		f->store_line(line);
	}
}

// While Blender runs next to it, Godot is "unfocused" whenever Blender (or anything else) is the
// active window, and the editor then throttles itself to unfocused_low_processor_mode_sleep_usec
// (100 ms = 10 fps by default). Every hand-over between the 3D view and Blender went through
// those throttled frames. So while Blender is running, the unfocused speed equals the normal
// speed; the user's value is saved and restored when Blender stops or the plugin unloads.
static const char *UNFOCUSED_KEY = "interface/editor/timers/unfocused_low_processor_mode_sleep_usec"; // Godot 4.7 path
static const char *FOCUSED_KEY = "interface/editor/timers/low_processor_mode_sleep_usec";
static const char *SAVED_UNFOCUSED_KEY = "blender_dock/saved_unfocused_sleep_usec";

void BlenderDockPlugin::_sync_unfocused_speed() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null() || !es->has_setting(UNFOCUSED_KEY)) {
		return;
	}
	bool want = embed->is_running();
	if (want == unfocused_overridden) {
		return;
	}
	int focused = es->has_setting(FOCUSED_KEY) ? int(es->get_setting(FOCUSED_KEY)) : 6900;
	if (want) {
		if (!es->has_setting(SAVED_UNFOCUSED_KEY)) { // keep the original across crashes
			es->set_setting(SAVED_UNFOCUSED_KEY, es->get_setting(UNFOCUSED_KEY));
		}
		es->set_setting(UNFOCUSED_KEY, focused);
		OS::get_singleton()->set_low_processor_usage_mode_sleep_usec(focused);
	} else {
		_restore_unfocused_speed();
	}
	unfocused_overridden = want;
}

void BlenderDockPlugin::_restore_unfocused_speed() {
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	if (es.is_null() || !es->has_setting(SAVED_UNFOCUSED_KEY)) {
		return;
	}
	es->set_setting(UNFOCUSED_KEY, es->get_setting(SAVED_UNFOCUSED_KEY));
	es->erase(SAVED_UNFOCUSED_KEY);
	unfocused_overridden = false;
}

void BlenderDockPlugin::_process_inner(double p_delta) {
	uint64_t _perf_t = Time::get_singleton()->get_ticks_usec();
	_sync_unfocused_speed();

	if (link.is_valid()) {
		link->poll(sync_enabled);
		if (link->take_just_connected()) {
			embedded_sent_parent = 0;
			chrome_sent_docked = !chrome_sent_docked; // force resend
			last_view = Transform3D();
		}
		String n = link->take_notice();
		if (!n.is_empty()) {
			notice = n;
			notice_time = Time::get_singleton()->get_ticks_msec() / 1000.0;
		}
		link->watch_selected_transforms();
	}
	PERF_MARK(0); // link

	if (embed->is_running()) {
		if (!embed->has_window()) {
			embed->find_window(); // every frame while starting: hide it the moment it appears
		}
		embed->enforce_hold();
		PERF_MARK(1); // find/hold
		if (embed->has_window() && !popped_out) {
			Control *host = _active_host();
			Vector2 size = host ? host->get_size() : Vector2();
			if (host && size.x >= 8 && size.y >= 8 && host->get_window() &&
					host->get_window()->get_mode() != Window::MODE_MINIMIZED) {
				int64_t parent = DisplayServer::get_singleton()->window_get_native_handle(
						DisplayServer::WINDOW_HANDLE, host->get_window()->get_window_id());
				// Restyling / re-owning another process's window is synchronous with its UI thread.
				// Only do it once Blender is loaded and has just answered a probe, so Godot never waits.
				if (blender_ready && (embed->parent() != intptr_t(parent) || !embed->is_embedded()) &&
						embed->is_responsive()) {
					embed->embed(intptr_t(parent));
				}
				Vector2 pos = host->get_screen_position();
				EmbedRect r;
				r.x = int(std::floor(pos.x));
				r.y = int(std::floor(pos.y));
				r.w = int(std::ceil(size.x));
				r.h = int(std::ceil(size.y));
				PERF_MARK(2); // embed
				embed->set_rect(r);
				embed->set_visible(true);
				PERF_MARK(3); // rect+visible

				Control *focus = EditorInterface::get_singleton()->get_base_control()->get_viewport()->gui_get_focus_owner();
				bool text = Object::cast_to<LineEdit>(focus) || Object::cast_to<TextEdit>(focus);
				embed->update_focus(bool(_setting(S_FOLLOW_MOUSE, false)), text);
				PERF_MARK(4); // focus
			} else {
				embed->set_visible(false);
			}
		}
		// Tell Blender where it lives, so it can rescue its window if the editor dies.
		if (link.is_valid() && link->is_peer_connected() && embed->is_embedded() &&
				embed->parent() != embedded_sent_parent) {
			embedded_sent_parent = embed->parent();
			Dictionary d;
			d["t"] = "embedded";
			d["hwnd"] = int64_t(embed->hwnd());
			d["parent"] = int64_t(embed->parent());
			link->send(d);
		}
		_send_chrome();
	}

	PERF_MARK(5); // messages
	_update_startup(p_delta);

	if (follow_view) {
		_follow_view();
	}

	if (!tab_icon_fixed && blender_icon.is_valid()) {
		tab_icon_fixed = _untint_tab_icon(EditorInterface::get_singleton()->get_base_control()->get_parent());
	}

	if (pending_auto_link && Object::cast_to<Node3D>(EditorInterface::get_singleton()->get_edited_scene_root())) {
		pending_auto_link = false;
		link->add_to_edited_scene();
	}

	if (selftest_step >= 0) {
		_self_test(p_delta);
	}

	ui_cooldown -= p_delta;
	if (ui_cooldown <= 0.0) {
		ui_cooldown = 0.25;
		_update_ui();
	}
	PERF_MARK(6); // startup/ui
}

void BlenderDockPlugin::_self_test(double p_delta) {
	selftest_time += p_delta;
	auto log = [](const String &s) { UtilityFunctions::print("SELFTEST ", s); };
	EditorInterface *ei = EditorInterface::get_singleton();
	switch (selftest_step) {
		case 0: // wait for Blender + link + linked scene with a Cube
			if (link->is_peer_connected() && link->edited_scene_has_link() && link->get_object_count() >= 3 &&
					embed->has_window() && selftest_time > 3.0) {
				log("start");
				ei->set_main_screen_editor("3D");
				selftest_step = 1;
				selftest_time = 0;
			}
			break;
		case 1:
			if (selftest_time > 1.0) {
				log(vformat("before dock open: main_visible=%s", main_visible));
				dock->make_visible();
				selftest_step = 2;
				selftest_time = 0;
			}
			break;
		case 2:
			if (selftest_time > 1.0) {
				log(vformat("item2 dock open keeps 3D screen: %s (main_visible=%s, dock host active=%s)",
						!main_visible ? "PASS" : "FAIL", main_visible, _active_host() == dock_panel.host));
				Node3D *lr = link->get_link_root();
				Node3D *cube = nullptr;
				for (int i = 0; lr && i < lr->get_child_count() && !cube; i++) {
					cube = Object::cast_to<Node3D>(lr->find_child("Cube", true, false));
				}
				if (cube) {
					ei->get_selection()->clear();
					ei->get_selection()->add_node(cube);
				}
				log(vformat("selected Cube in Godot: %s", cube != nullptr));
				selftest_step = 3;
				selftest_time = 0;
			}
			break;
		case 3:
			if (selftest_time > 1.0) {
				TypedArray<Node> sel = ei->get_selection()->get_selected_nodes();
				Node3D *cube = sel.size() ? Object::cast_to<Node3D>(sel[0]) : nullptr;
				if (cube) {
					cube->set_position(cube->get_position() + Vector3(1.5, 0, 0));
					log(vformat("moved Cube in Godot to %s", cube->get_position()));
				}
				follow_view = true;
				selftest_step = 4;
				selftest_time = 0;
			}
			break;
		case 4:
			if (selftest_time > 1.5) {
				_save_external_data();
				log("requested save of .blend to " + _default_blend_path());
				selftest_step = 5;
				selftest_time = 0;
			}
			break;
		case 5:
			if (selftest_time > 2.0) {
				_on_bake_pressed();
				log("requested glTF bake");
				selftest_step = 6;
				selftest_time = 0;
			}
			break;
		case 6:
			if (selftest_time > 4.0) {
				log("done");
				selftest_step = -1;
			}
			break;
	}
}

void BlenderDockPlugin::_send_chrome() {
	if (!link.is_valid() || !link->is_peer_connected()) {
		return;
	}
	bool docked = embed->is_embedded() && !popped_out;
	if (docked == chrome_sent_docked) {
		return;
	}
	chrome_sent_docked = docked;
	Dictionary d;
	d["t"] = "chrome";
	d["docked"] = docked;
	link->send(d);
}

void BlenderDockPlugin::_follow_view() {
	if (!link.is_valid() || !link->is_peer_connected()) {
		return;
	}
	SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(0);
	Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
	if (!cam) {
		return;
	}
	Transform3D t = cam->get_global_transform();
	if (Node3D *lr = link->get_link_root()) {
		t = lr->get_global_transform().affine_inverse() * t;
	}
	double fov = cam->get_projection() == Camera3D::PROJECTION_ORTHOGONAL ? cam->get_size() : cam->get_fov();
	if (t.is_equal_approx(last_view) && Math::is_equal_approx(fov, last_fov)) {
		return;
	}
	last_view = t;
	last_fov = fov;
	Dictionary d;
	d["t"] = "view";
	d["xform"] = xform_list(t);
	d["fov"] = cam->get_fov();
	d["ortho"] = cam->get_projection() == Camera3D::PROJECTION_ORTHOGONAL;
	d["size"] = cam->get_size();
	d["distance"] = 10.0;
	link->send(d);
}

void BlenderDockPlugin::_save_external_data() {
	// Ctrl+S in Godot also saves the .blend.
	if (!link.is_valid() || !link->is_peer_connected()) {
		return;
	}
	String path = _default_blend_path();
	if (link->get_blend_file().is_empty()) {
		ensure_ignored_dir(ProjectSettings::get_singleton()->localize_path(path.get_base_dir()));
		ProjectSettings *ps = ProjectSettings::get_singleton();
		if (String(ps->get_setting(P_BLEND_FILE, String())).is_empty()) {
			ps->set_setting(P_BLEND_FILE, ps->localize_path(path));
			ps->save();
		}
	}
	Dictionary d;
	d["t"] = "save";
	d["path"] = path;
	link->send(d);
}

// ---------------------------------------------------------------------------- actions

void BlenderDockPlugin::_launch(const String &p_blend) {
	last_error = "";
	String exe = _find_blender();
	if (exe.is_empty()) {
		last_error = "Blender not found. Set it in the Blender Dock settings (gear button).";
		_update_ui();
		return;
	}
	ProjectSettings *ps = ProjectSettings::get_singleton();
	String args;
	if (!p_blend.is_empty()) {
		args += quote(p_blend) + " ";
	}
	EmbedRect lr = _launch_rect();
	args += vformat("--no-window-focus -p %d %d %d %d ", lr.x, lr.y, lr.w, lr.h);
	args += "--python " + quote(ps->globalize_path(SCRIPT_PATH)) + " --";
	args += " --godot-port " + itos(int(_setting(S_PORT, 6789)));
	args += " --godot-pid " + itos(OS::get_singleton()->get_process_id());
	args += " --godot-project " + quote(ps->globalize_path("res://").trim_suffix("/"));
	int recovery = int(_setting(S_RECOVERY, 30));
	if (recovery > 0) {
		args += " --godot-recovery " + quote(_recovery_path());
		args += " --godot-recovery-interval " + itos(recovery);
	}
	args += String(" --godot-hide-statusbar ") + (bool(_setting(S_HIDE_STATUSBAR, true)) ? "1" : "0");
	if (bool(_setting(S_MATCH_THEME, true))) {
		Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
		if (es->has_setting("interface/theme/base_color")) {
			args += " --godot-base " + Color(es->get_setting("interface/theme/base_color")).to_html(false);
		}
		if (es->has_setting("interface/theme/accent_color")) {
			args += " --godot-accent " + Color(es->get_setting("interface/theme/accent_color")).to_html(false);
		}
	}
	if (bool(_setting(S_KEEP_SPLASH, false))) {
		args += " --godot-keep-splash";
	}

	std::wstring err;
	popped_out = false;
	blender_ready = false;
	window_elapsed = -1.0;
	launch_elapsed = 0.0;
	loading_value = 0.0;
	embed->set_hold_hidden(true);
	if (!embed->launch(to_w(exe), to_w(args), to_w(exe.get_base_dir()), err)) {
		last_error = "Could not start Blender: " + from_w(err).strip_edges();
	}
	find_cooldown = 0.0;
	_update_ui();
}

void BlenderDockPlugin::_launch_checked(const String &p_blend) {
	if (FileAccess::file_exists(_recovery_path())) {
		pending_launch_blend = p_blend;
		if (selftest_step >= 0) {
			UtilityFunctions::print("SELFTEST recovery file found -> recovering");
			_on_recover_confirmed();
			return;
		}
		recover_dialog->popup_centered();
		return;
	}
	_launch(p_blend);
}

void BlenderDockPlugin::_on_recover_confirmed() {
	// Never open the recovery file itself (it is deleted on Blender's clean exit): copy it into the project.
	String stamp = Time::get_singleton()->get_datetime_string_from_system(false, true).replace(":", "-").replace(" ", "_");
	ensure_ignored_dir("res://blender");
	String dst = ProjectSettings::get_singleton()->globalize_path("res://blender").path_join("recovered_" + stamp + ".blend");
	Error err = DirAccess::copy_absolute(_recovery_path(), dst);
	if (err == OK) {
		DirAccess::remove_absolute(_recovery_path());
		DirAccess::remove_absolute(_recovery_path() + "1");
		notice = "Recovered Blender work saved as res://blender/" + dst.get_file();
		notice_time = Time::get_singleton()->get_ticks_msec() / 1000.0;
		_launch(dst);
	} else {
		last_error = "Could not copy the recovery file; it is still at " + _recovery_path();
		_launch(pending_launch_blend);
	}
}

void BlenderDockPlugin::_on_recover_canceled() {
	DirAccess::remove_absolute(_recovery_path());
	DirAccess::remove_absolute(_recovery_path() + "1");
	_launch(pending_launch_blend);
}

void BlenderDockPlugin::_on_launch_pressed() {
	String blend = ProjectSettings::get_singleton()->get_setting(P_BLEND_FILE, String());
	if (!blend.is_empty()) {
		blend = ProjectSettings::get_singleton()->globalize_path(blend);
		if (!FileAccess::file_exists(blend)) {
			blend = "";
		}
	}
	_launch_checked(blend);
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
		_launch_checked(path);
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
	_send_chrome();
	_update_ui();
}

void BlenderDockPlugin::_on_add_link_pressed() {
	last_error = link->add_to_edited_scene() ? String() : String("Open a 3D scene (Node3D root) first.");
	_update_ui();
}

void BlenderDockPlugin::_on_bake_pressed() {
	ensure_ignored_dir("res://blender"); // keep the folder layout predictable
	Dictionary d;
	d["t"] = "bake";
	d["dir"] = ProjectSettings::get_singleton()->globalize_path("res://blender_assets");
	link->send(d);
	notice = "Baking Blender selection to glTF...";
	notice_time = Time::get_singleton()->get_ticks_msec() / 1000.0;
	_update_ui();
}

void BlenderDockPlugin::_on_sync_toggled(bool p_on) {
	sync_enabled = p_on;
	_update_ui();
}

void BlenderDockPlugin::_on_follow_toggled(bool p_on) {
	follow_view = p_on;
	last_view = Transform3D();
	_update_ui();
}

void BlenderDockPlugin::_on_selection_changed() {
	if (link.is_valid()) {
		link->on_editor_selection_changed();
	}
}

void BlenderDockPlugin::_on_settings_pressed() {
	SettingsUI &s = settings;
	s.path->set_text(_setting(S_BLENDER_PATH, String()));
	s.port->set_value(int(_setting(S_PORT, 6789)));
	s.recovery->set_value(int(_setting(S_RECOVERY, 30)));
	s.auto_launch->set_pressed(_setting(S_AUTO_LAUNCH, false));
	s.follow_mouse->set_pressed(_setting(S_FOLLOW_MOUSE, false));
	s.theme->set_pressed(_setting(S_MATCH_THEME, true));
	s.statusbar->set_pressed(_setting(S_HIDE_STATUSBAR, true));
	s.splash->set_pressed(_setting(S_KEEP_SPLASH, false));
	s.dialog->popup_centered();
}

void BlenderDockPlugin::_on_browse_blender() {
	settings.browse->popup_file_dialog();
}

void BlenderDockPlugin::_on_blender_path_selected(const String &p_path) {
	settings.path->set_text(p_path);
}

void BlenderDockPlugin::_on_settings_confirmed() {
	SettingsUI &s = settings;
	Ref<EditorSettings> es = EditorInterface::get_singleton()->get_editor_settings();
	int old_port = _setting(S_PORT, 6789);
	es->set_setting(S_BLENDER_PATH, s.path->get_text().strip_edges());
	es->set_setting(S_PORT, int(s.port->get_value()));
	es->set_setting(S_RECOVERY, int(s.recovery->get_value()));
	es->set_setting(S_AUTO_LAUNCH, s.auto_launch->is_pressed());
	es->set_setting(S_FOLLOW_MOUSE, s.follow_mouse->is_pressed());
	es->set_setting(S_MATCH_THEME, s.theme->is_pressed());
	es->set_setting(S_HIDE_STATUSBAR, s.statusbar->is_pressed());
	es->set_setting(S_KEEP_SPLASH, s.splash->is_pressed());
	if (int(s.port->get_value()) != old_port) {
		link->start(int(s.port->get_value()));
	}
	notice = embed->is_running() ? String("Settings saved. Some apply the next time Blender starts.") : String("Settings saved.");
	notice_time = Time::get_singleton()->get_ticks_msec() / 1000.0;
	_update_ui();
}
