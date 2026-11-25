#pragma once
// TCP live link: Blender (client, godot_link.py) <-> Godot editor (server).
//
// Frame (little endian, both directions):
//   u32 frame_len | u32 header_len | header (UTF-8 JSON) | blob (frame_len - 4 - header_len bytes)
//
// Blender -> Godot
//   hello {token, file, blender}   must be the first frame; wrong/missing token -> connection dropped
//   coll  {name, parent, visible}                          collection -> Node3D
//   obj   {name, kind, parent, xform[12] (local), visible, mesh, col, colonly, light/camera params}
//   mesh  {key, pos_only, surfaces:[{nv, ni, off, len, tan, mat, uv, uv2, col}]}
//         blob per surface: pos f32x3, nrm f32x3, [tan f32x4], (unless pos_only:) [uv f32x2] [uv2 f32x2] [col f32x4] idx i32
//   mat   {name, color, metallic, roughness, specular, emission, emission_strength, alpha_blend, double_sided,
//          texture, vertex_color, normal_tex, normal_strength, rough_tex, rough_ch, metal_tex, metal_ch, emission_tex}
//   inst  {key, mesh, count}  blob = count * 12 f32 (MultiMesh 3x4 row-major, Godot world space under BlenderLink)
//   scene {objects, collections, [instances]}             anything not listed is removed
//   sel   {names, active}
//   baked {ok, path | msg}
// Godot -> Blender
//   sync {token} (first frame, proves this is the Godot that launched Blender), open {path}, save {path}, quit, sel {names, active}, xform {name, xform (relative to BlenderLink)},
//   view {xform, fov, ortho, size, distance}, chrome {docked}, embedded {hwnd, parent}, bake {dir}
//
// Parent keys: "" = BlenderLink root, "o:<object>", "c:<collection>". Node keys add "i:<instance group>".

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/stream_peer_tcp.hpp>
#include <godot_cpp/classes/tcp_server.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace godot {

class BlenderLink : public RefCounted {
	GDCLASS(BlenderLink, RefCounted)

public:
	static constexpr const char *ROOT_META = "blender_link_root";
	static constexpr const char *NAME_META = "blender_name"; // node key, e.g. "o:Cube"
	static constexpr const char *MODE_META = "blender_link_mode"; // "preview" (default) or "scene"

	// Listens on p_port, or the next free port (up to p_tries ports). get_port() gives the one in use.
	Error start(int p_port, int p_tries = 32);
	// Random per-editor-session secret, passed to Blender on its command line.
	String get_token();
	void stop();

	// Network + scene sync. Call every editor tick.
	void poll(bool p_sync_enabled);

	bool is_listening() const { return server.is_valid() && server->is_listening(); }
	bool is_peer_connected() const;
	bool take_just_connected();
	bool has_scene() const { return scene_received; }
	int get_port() const { return port; }
	int get_object_count() const { return objects.size(); }
	String get_blend_file() const { return blend_file; }
	String take_notice(); // one-shot user-facing message (bake results...)
	bool take_blender_view(Dictionary &r_view); // newest Blender viewport, if any arrived

	bool add_to_edited_scene();
	bool edited_scene_has_link();
	Node3D *get_link_root();

	void send(const Dictionary &p_header);

	// Preview: draw through RenderingServer only (no nodes, no editor overhead).
	// Scene: real nodes in the scene (selectable, editable, saved).
	bool is_preview_mode() const { return preview_mode; }
	bool set_preview_mode(bool p_preview);
	static String self_test_encoder(); // BLENDER_DOCK_ENCODER_TEST=1

	~BlenderLink() { _preview_clear(); }
	// Editor shutdown: send save_quit and block (up to p_timeout_ms) until Blender acknowledges.
	bool send_save_quit(const String &p_path, int p_timeout_ms);

	// Two-way helpers driven by the plugin.
	void on_editor_selection_changed();
	void watch_selected_transforms();

protected:
	static void _bind_methods() {}

private:
	struct MeshCache {
		Ref<ArrayMesh> mesh;
		Vector<Array> surface_arrays; // last full arrays, reused by position-only updates
		Vector<String> surface_mats;
		uint64_t revision = 0;
		bool fast_verified = false; // encoder checked against Godot's own buffer
		Vector<bool> fast_ok;
	};
	struct PreviewItem {
		RID instance;
		RID light;
		String mesh;
		String kind;
	};

	static bool _encode_vertex_buffer(uint64_t p_format, const Array &p_arrays, PackedByteArray &r_out);
	void _verify_fast_path(MeshCache &mc, const String &p_key);
	bool _fast_vertex_update(MeshCache &mc, const Vector<Array> &p_new_arrays);
	RID _preview_scenario();
	void _preview_object(const String &p_name, const Dictionary &h);
	void _preview_instances(const String &p_key);
	void _preview_remove(const String &p_key);
	void _preview_clear();
	void _preview_rebuild();
	void _preview_follow_root();

	bool preview_mode = true;
	bool fast_path_reported = false;
	HashMap<String, PreviewItem> preview;
	HashMap<String, Ref<MultiMesh>> preview_mm;
	Transform3D preview_root_xform;
	static constexpr uint64_t FRAME_BUDGET_USEC = 4000; // max time per editor frame applying Blender data

	void _read_peer();
	void _poll_pending();
	void _adopt(const Ref<StreamPeerTCP> &p_peer);
	void _handle_frame(const Dictionary &p_header, const uint8_t *p_blob, int64_t p_blob_len);

	void _on_material(const Dictionary &h);
	void _on_mesh(const Dictionary &h, const uint8_t *blob, int64_t blob_len);
	void _on_object(const Dictionary &h);
	void _on_collection(const Dictionary &h);
	void _on_instances(const Dictionary &h, const uint8_t *blob, int64_t blob_len);
	void _on_scene(const Dictionary &h);
	void _on_selection(const Dictionary &h);

	Ref<Texture2D> _texture(const String &p_path);
	Ref<StandardMaterial3D> _material(const String &p_name);

	// Scene side
	Node3D *_find_link_root();
	void _bind_scene();
	Node3D *_node(const String &p_key);
	void _apply_object(const String &p_name, const Dictionary &p_state);
	void _apply_collection(const String &p_name, const Dictionary &p_state);
	void _apply_instances(const String &p_key);
	void _place(Node3D *p_node, const String &p_parent_key, const String &p_key);
	void _resolve_parents();
	void _remove_node(const String &p_key);
	void _update_collision(Node3D *p_node, const String &p_key, const Dictionary &p_state);
	void _refresh_collisions();
	void _own_recursive(Node *p_node, Node *p_owner);

	Ref<TCPServer> server;
	Ref<StreamPeerTCP> peer;
	// A new connection waits here until its hello carries the token (a stranger cannot
	// replace or talk to the live Blender).
	Ref<StreamPeerTCP> pending;
	PackedByteArray pending_buf;
	uint64_t pending_deadline = 0;
	String token;
	int port = 6789;
	bool just_connected = false;
	bool scene_received = false;
	bool bye_received = false;
	Dictionary blender_view;
	bool has_blender_view = false;

	PackedByteArray inbuf;
	int64_t inbuf_off = 0;

	String blend_file;
	String notice;
	HashMap<String, MeshCache> meshes;
	HashMap<String, Ref<StandardMaterial3D>> materials;
	HashMap<String, Ref<Texture2D>> textures;
	Dictionary objects; // name -> last obj header
	Dictionary collections; // name -> last coll header
	HashMap<String, PackedFloat32Array> instance_xforms;
	HashMap<String, String> instance_mesh;

	// Scene binding
	bool sync_enabled = true;
	uint64_t bound_scene_id = 0;
	uint64_t link_root_id = 0;
	HashMap<String, uint64_t> nodes; // key -> Node3D instance id
	HashMap<String, String> pending_parent; // key -> parent key not yet available
	HashMap<String, uint64_t> collision_rev; // key -> mesh revision used for its shape
	HashSet<String> collision_dirty;
	uint64_t next_collision_ms = 0;
	bool scene_changed = false;
	uint64_t next_unsaved_ms = 0;
	String last_summary;

	// Two-way state
	PackedStringArray last_sel;
	HashMap<String, Transform3D> applied_xform; // what we last set (or sent) per object key
	HashMap<String, uint64_t> local_edit_until; // ignore Blender echoes while Godot edits
	uint64_t next_xform_ms = 0;
};

} // namespace godot
