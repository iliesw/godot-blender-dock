#pragma once
// TCP live link: Blender (client, godot_link.py) -> Godot editor (server).
//
// Wire format, little endian, both directions:
//   u32 frame_len | u32 header_len | header (UTF-8 JSON) | blob (frame_len - 4 - header_len bytes)
//
// Blender -> Godot headers:
//   hello  {file, blender}
//   mat    {name, color[4], metallic, roughness, emission[3], emission_strength, alpha_blend, double_sided, texture}
//   mesh   {key, surfaces:[{mat, nv, ni, uv, off}]}   blob = per surface: pos f32x3, nrm f32x3, [uv f32x2], idx i32
//   obj    {name, kind, xform[12], visible, mesh?, light/camera params}
//   scene  {objects:[names]}   anything not listed is removed
// Godot -> Blender headers:
//   sync {}, open {path}, save {}, quit {}, select {name}
//
// All coordinates arrive already converted to Godot space (Y up, -Z forward, CW winding).

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/stream_peer_tcp.hpp>
#include <godot_cpp/classes/tcp_server.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace godot {

class BlenderLink : public RefCounted {
	GDCLASS(BlenderLink, RefCounted)

public:
	static constexpr const char *ROOT_META = "blender_link_root";
	static constexpr const char *NAME_META = "blender_name";

	Error start(int p_port);
	void stop();

	// Network + scene sync. Call every editor tick.
	void poll(bool p_sync_enabled);

	bool is_listening() const { return server.is_valid() && server->is_listening(); }
	bool is_peer_connected() const;
	int get_port() const { return port; }
	int get_object_count() const { return objects.size(); }
	String get_blend_file() const { return blend_file; }

	// Creates the "BlenderLink" container in the edited scene (if it is 3D) and fills it.
	bool add_to_edited_scene();
	bool edited_scene_has_link();

	void send(const Dictionary &p_header);

protected:
	static void _bind_methods() {}

private:
	void _read_peer();
	void _handle_frame(const Dictionary &p_header, const uint8_t *p_blob, int64_t p_blob_len);

	void _on_material(const Dictionary &h);
	void _on_mesh(const Dictionary &h, const uint8_t *blob, int64_t blob_len);
	void _on_object(const Dictionary &h);
	void _on_scene(const Dictionary &h);

	// Scene side
	Node3D *_find_link_root();
	void _bind_scene(); // (re)attach to the current edited scene, rebuild node map
	void _apply_object(const String &p_name, const Dictionary &p_state);
	void _remove_object_node(const String &p_name);
	Ref<StandardMaterial3D> _material(const String &p_name);

	Ref<TCPServer> server;
	Ref<StreamPeerTCP> peer;
	int port = 6789;

	PackedByteArray inbuf;
	int64_t inbuf_off = 0;

	String blend_file;
	HashMap<String, Ref<ArrayMesh>> meshes;
	HashMap<String, Ref<StandardMaterial3D>> materials;
	HashMap<String, String> material_textures; // material -> texture path currently applied
	Dictionary objects; // name -> last obj header

	// Current scene binding
	bool sync_enabled = true;
	uint64_t bound_scene_id = 0;
	uint64_t link_root_id = 0;
	HashMap<String, uint64_t> nodes; // blender name -> Node3D instance id
	bool scene_changed = false;
	int last_reported = -1;
};

} // namespace godot
