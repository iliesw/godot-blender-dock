#include "blender_link.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/classes/convex_polygon_shape3d.hpp>
#include <godot_cpp/classes/shape3d.hpp>
#include <godot_cpp/classes/spot_light3d.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>

using namespace godot;

namespace {

constexpr int64_t MAX_FRAME = 512ll * 1024 * 1024;

uint32_t rd_u32(const uint8_t *p) {
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void wr_u32(uint8_t *p, uint32_t v) {
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

Color to_color(const Variant &v, const Color &def = Color(1, 1, 1, 1)) {
	Array a = v;
	if (a.size() < 3) {
		return def;
	}
	return Color(a[0], a[1], a[2], a.size() > 3 ? float(a[3]) : 1.0f);
}

template <typename T>
T *instance_from(uint64_t id) {
	return id ? Object::cast_to<T>(ObjectDB::get_instance(ObjectID(id))) : nullptr;
}

uint64_t now_ms() {
	return Time::get_singleton()->get_ticks_msec();
}

template <typename P, typename E>
P read_packed(const uint8_t *&p, int64_t count, int64_t elem_size) {
	P arr;
	arr.resize(count);
	memcpy(arr.ptrw(), p, count * elem_size);
	p += count * elem_size;
	return arr;
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

BaseMaterial3D::TextureChannel to_channel(const String &c) {
	if (c == "r") {
		return BaseMaterial3D::TEXTURE_CHANNEL_RED;
	} else if (c == "g") {
		return BaseMaterial3D::TEXTURE_CHANNEL_GREEN;
	} else if (c == "b") {
		return BaseMaterial3D::TEXTURE_CHANNEL_BLUE;
	} else if (c == "a") {
		return BaseMaterial3D::TEXTURE_CHANNEL_ALPHA;
	}
	return BaseMaterial3D::TEXTURE_CHANNEL_GRAYSCALE;
}

} // namespace

// ---------------------------------------------------------------------------- network

Error BlenderLink::start(int p_port) {
	stop();
	port = p_port;
	server.instantiate();
	Error err = server->listen(port, "127.0.0.1");
	if (err != OK) {
		UtilityFunctions::push_warning("Blender Dock: cannot listen on 127.0.0.1:", port, " (", err, ")");
	}
	return err;
}

void BlenderLink::stop() {
	_preview_clear();
	if (peer.is_valid()) {
		peer->disconnect_from_host();
		peer.unref();
	}
	if (server.is_valid()) {
		server->stop();
		server.unref();
	}
	inbuf.clear();
	inbuf_off = 0;
}

bool BlenderLink::is_peer_connected() const {
	return peer.is_valid() && peer->get_status() == StreamPeerTCP::STATUS_CONNECTED;
}

bool BlenderLink::take_just_connected() {
	bool v = just_connected;
	just_connected = false;
	return v;
}

bool BlenderLink::take_blender_view(Dictionary &r_view) {
	if (!has_blender_view) {
		return false;
	}
	has_blender_view = false;
	r_view = blender_view;
	return true;
}

String BlenderLink::take_notice() {
	String n = notice;
	notice = "";
	return n;
}

void BlenderLink::send(const Dictionary &p_header) {
	if (!is_peer_connected()) {
		return;
	}
	CharString js = JSON::stringify(p_header).utf8();
	uint32_t hl = uint32_t(js.length());
	PackedByteArray out;
	out.resize(8 + hl);
	uint8_t *w = out.ptrw();
	wr_u32(w, 4 + hl);
	wr_u32(w + 4, hl);
	memcpy(w + 8, js.get_data(), hl);
	peer->put_data(out);
}

bool BlenderLink::send_save_quit(const String &p_path, int p_timeout_ms) {
	if (!is_peer_connected()) {
		return false;
	}
	bye_received = false;
	Dictionary d;
	d["t"] = "save_quit";
	d["path"] = p_path;
	send(d);
	uint64_t until = now_ms() + p_timeout_ms;
	while (now_ms() < until && !bye_received && peer.is_valid()) {
		peer->poll();
		if (peer->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
			break;
		}
		_read_peer();
		OS::get_singleton()->delay_msec(10);
	}
	UtilityFunctions::print("Blender Dock: save_quit ", bye_received ? "acknowledged" : "NOT acknowledged");
	return bye_received;
}

void BlenderLink::poll(bool p_sync_enabled) {
	bool was_enabled = sync_enabled;
	sync_enabled = p_sync_enabled;

	if (server.is_valid() && server->is_connection_available()) {
		Ref<StreamPeerTCP> incoming = server->take_connection();
		if (incoming.is_valid()) {
			if (peer.is_valid()) {
				peer->disconnect_from_host(); // newest Blender wins
			}
			peer = incoming;
			peer->set_no_delay(true);
			inbuf.clear();
			inbuf_off = 0;
			just_connected = true;
			scene_received = false;
			last_sel.clear();
			Dictionary d;
			d["t"] = "sync";
			send(d);
		}
	}

	if (peer.is_valid()) {
		peer->poll();
		StreamPeerTCP::Status st = peer->get_status();
		if (st == StreamPeerTCP::STATUS_ERROR || st == StreamPeerTCP::STATUS_NONE) {
			peer.unref();
		} else if (st == StreamPeerTCP::STATUS_CONNECTED) {
			_read_peer();
		}
	}

	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	uint64_t rid = root ? uint64_t(root->get_instance_id()) : 0;
	if (rid != bound_scene_id || (link_root_id && !instance_from<Node3D>(link_root_id)) || (!was_enabled && sync_enabled)) {
		_bind_scene();
	}

	if (preview_mode) {
		_preview_follow_root();
	}
	if (!pending_parent.is_empty()) {
		_resolve_parents();
	}
	if (!collision_dirty.is_empty() && now_ms() >= next_collision_ms) {
		next_collision_ms = now_ms() + 300;
		_refresh_collisions();
	}

	if (scene_changed && now_ms() >= next_unsaved_ms) {
		scene_changed = false;
		next_unsaved_ms = now_ms() + 1000; // the editor repaints tabs/title on every call
		if (instance_from<Node3D>(link_root_id)) {
			EditorInterface::get_singleton()->mark_scene_as_unsaved();
		}
	}
}

void BlenderLink::_read_peer() {
	for (int guard = 0; guard < 64; guard++) {
		int avail = peer->get_available_bytes();
		if (avail <= 0) {
			break;
		}
		Array r = peer->get_partial_data(avail);
		if (int(r[0]) != OK) {
			break;
		}
		PackedByteArray chunk = r[1];
		if (chunk.is_empty()) {
			break;
		}
		inbuf.append_array(chunk);
	}

	int64_t size = inbuf.size();
	const uint8_t *data = inbuf.ptr();
	uint64_t t0 = Time::get_singleton()->get_ticks_usec();
	while (size - inbuf_off >= 8) {
		if (Time::get_singleton()->get_ticks_usec() - t0 > FRAME_BUDGET_USEC) {
			break; // keep the editor responsive: the rest is applied next frame
		}
		uint32_t frame_len = rd_u32(data + inbuf_off);
		if (frame_len < 4 || frame_len > MAX_FRAME) {
			UtilityFunctions::push_error("Blender Dock: corrupt link frame, resetting connection.");
			peer->disconnect_from_host();
			peer.unref();
			inbuf.clear();
			inbuf_off = 0;
			return;
		}
		if (size - inbuf_off - 4 < int64_t(frame_len)) {
			break;
		}
		const uint8_t *f = data + inbuf_off + 4;
		uint32_t hl = rd_u32(f);
		if (hl <= frame_len - 4) {
			Variant hv = JSON::parse_string(String::utf8(reinterpret_cast<const char *>(f + 4), hl));
			if (hv.get_type() == Variant::DICTIONARY) {
				_handle_frame(hv, f + 4 + hl, int64_t(frame_len) - 4 - hl);
			}
		}
		inbuf_off += 4 + frame_len;
	}

	if (inbuf_off > 0 && inbuf_off >= size) {
		inbuf.clear();
		inbuf_off = 0;
	} else if (inbuf_off > (1 << 20) && inbuf_off > size / 2) {
		inbuf = inbuf.slice(inbuf_off);
		inbuf_off = 0;
	}
}

void BlenderLink::_handle_frame(const Dictionary &h, const uint8_t *blob, int64_t blob_len) {
	String t = h.get("t", "");
	if (t == "obj") {
		_on_object(h);
	} else if (t == "mesh") {
		_on_mesh(h, blob, blob_len);
	} else if (t == "mat") {
		_on_material(h);
	} else if (t == "coll") {
		_on_collection(h);
	} else if (t == "inst") {
		_on_instances(h, blob, blob_len);
	} else if (t == "scene") {
		_on_scene(h);
	} else if (t == "sel") {
		_on_selection(h);
	} else if (t == "baked") {
		if (bool(h.get("ok", false))) {
			notice = "Baked " + String(h.get("path", "")).get_file() + " into res://blender_assets";
			EditorInterface::get_singleton()->get_resource_filesystem()->scan();
		} else {
			notice = "Bake failed: " + String(h.get("msg", "unknown error"));
		}
		UtilityFunctions::print("Blender Dock: ", notice);
	} else if (t == "view") {
		blender_view = h; // only the newest matters
		has_blender_view = true;
	} else if (t == "bye") {
		bye_received = true;
	} else if (t == "hello") {
		blend_file = h.get("file", "");
		UtilityFunctions::print("Blender Dock: linked to Blender ", h.get("blender", ""), " ",
				blend_file.is_empty() ? String("(unsaved file)") : blend_file);
	}
}

// ---------------------------------------------------------------------------- materials

Ref<Texture2D> BlenderLink::_texture(const String &p_path) {
	if (p_path.is_empty()) {
		return Ref<Texture2D>();
	}
	String key = p_path + String("@") + itos(FileAccess::get_modified_time(p_path));
	if (Ref<Texture2D> *t = textures.getptr(key)) {
		return *t;
	}
	Ref<Image> img = Image::load_from_file(p_path);
	Ref<Texture2D> tex;
	if (img.is_valid() && !img->is_empty()) {
		img->generate_mipmaps();
		tex = ImageTexture::create_from_image(img);
	}
	textures[key] = tex;
	return tex;
}

Ref<StandardMaterial3D> BlenderLink::_material(const String &p_name) {
	if (p_name.is_empty()) {
		return Ref<StandardMaterial3D>();
	}
	if (Ref<StandardMaterial3D> *m = materials.getptr(p_name)) {
		return *m;
	}
	Ref<StandardMaterial3D> m;
	m.instantiate();
	m->set_name(p_name);
	materials[p_name] = m;
	return m;
}

void BlenderLink::_on_material(const Dictionary &h) {
	Ref<StandardMaterial3D> m = _material(h.get("name", ""));
	if (m.is_null()) {
		return;
	}
	Color c = to_color(h.get("color", Variant()));
	m->set_albedo(c);
	m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, _texture(h.get("texture", "")));
	m->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, h.get("vertex_color", false));
	m->set_metallic(h.get("metallic", 0.0));
	m->set_roughness(h.get("roughness", 0.5));
	m->set_specular(h.get("specular", 0.5));

	Ref<Texture2D> rough = _texture(h.get("rough_tex", ""));
	m->set_texture(BaseMaterial3D::TEXTURE_ROUGHNESS, rough);
	m->set_roughness_texture_channel(to_channel(h.get("rough_ch", "")));
	Ref<Texture2D> metal = _texture(h.get("metal_tex", ""));
	m->set_texture(BaseMaterial3D::TEXTURE_METALLIC, metal);
	m->set_metallic_texture_channel(to_channel(h.get("metal_ch", "")));

	Ref<Texture2D> normal = _texture(h.get("normal_tex", ""));
	m->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, normal.is_valid());
	m->set_texture(BaseMaterial3D::TEXTURE_NORMAL, normal);
	m->set_normal_scale(h.get("normal_strength", 1.0));

	Color e = to_color(h.get("emission", Variant()), Color(0, 0, 0));
	double es = h.get("emission_strength", 0.0);
	Ref<Texture2D> etex = _texture(h.get("emission_tex", ""));
	bool emit = es > 0.0 && ((e.r + e.g + e.b) > 0.0 || etex.is_valid());
	m->set_feature(BaseMaterial3D::FEATURE_EMISSION, emit);
	if (emit) {
		m->set_emission(Color(e.r, e.g, e.b));
		m->set_emission_energy_multiplier(es);
		m->set_texture(BaseMaterial3D::TEXTURE_EMISSION, etex);
	}

	bool blend = h.get("alpha_blend", false);
	m->set_transparency(blend ? BaseMaterial3D::TRANSPARENCY_ALPHA
							  : (c.a < 0.999f ? BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS
											  : BaseMaterial3D::TRANSPARENCY_DISABLED));
	m->set_cull_mode(bool(h.get("double_sided", false)) ? BaseMaterial3D::CULL_DISABLED : BaseMaterial3D::CULL_BACK);
}

// ---------------------------------------------------------------------------- meshes

void BlenderLink::_on_mesh(const Dictionary &h, const uint8_t *blob, int64_t blob_len) {
	String key = h.get("key", "");
	if (key.is_empty()) {
		return;
	}
	MeshCache &mc = meshes[key];
	if (mc.mesh.is_null()) {
		mc.mesh.instantiate();
		mc.mesh->set_name(key.trim_prefix("inst:"));
	}
	bool pos_only = h.get("pos_only", false);
	Array surfaces = h.get("surfaces", Array());
	if (pos_only && surfaces.size() != mc.surface_arrays.size()) {
		Dictionary d;
		d["t"] = "sync"; // cache mismatch (e.g. after a scene reload): ask for everything again
		send(d);
		return;
	}

	Vector<Array> new_arrays;
	Vector<String> new_mats;
	for (int i = 0; i < surfaces.size(); i++) {
		Dictionary s = surfaces[i];
		int64_t nv = int64_t(s.get("nv", 0));
		int64_t ni = int64_t(s.get("ni", 0));
		int64_t off = int64_t(s.get("off", 0));
		int64_t len = int64_t(s.get("len", 0));
		if (nv <= 0 || ni <= 0 || off < 0 || off + len > blob_len) {
			continue;
		}
		const uint8_t *p = blob + off;
		bool has_tan = s.get("tan", false);

		Array arrays;
		if (pos_only) {
			arrays = mc.surface_arrays[i].duplicate();
			if (PackedVector3Array(arrays[Mesh::ARRAY_VERTEX]).size() != nv) {
				Dictionary d;
				d["t"] = "sync";
				send(d);
				return;
			}
		} else {
			arrays.resize(Mesh::ARRAY_MAX);
		}
		arrays[Mesh::ARRAY_VERTEX] = read_packed<PackedVector3Array, Vector3>(p, nv, 12);
		arrays[Mesh::ARRAY_NORMAL] = read_packed<PackedVector3Array, Vector3>(p, nv, 12);
		if (has_tan) {
			arrays[Mesh::ARRAY_TANGENT] = read_packed<PackedFloat32Array, float>(p, nv * 4, 4);
		}
		if (!pos_only) {
			if (bool(s.get("uv", false))) {
				arrays[Mesh::ARRAY_TEX_UV] = read_packed<PackedVector2Array, Vector2>(p, nv, 8);
			}
			if (bool(s.get("uv2", false))) {
				arrays[Mesh::ARRAY_TEX_UV2] = read_packed<PackedVector2Array, Vector2>(p, nv, 8);
			}
			if (bool(s.get("col", false))) {
				arrays[Mesh::ARRAY_COLOR] = read_packed<PackedColorArray, Color>(p, nv, 16);
			}
			arrays[Mesh::ARRAY_INDEX] = read_packed<PackedInt32Array, int32_t>(p, ni, 4);
			new_mats.push_back(s.get("mat", ""));
		} else {
			new_mats.push_back(mc.surface_mats[i]);
		}
		new_arrays.push_back(arrays);
	}

	if (pos_only && _fast_vertex_update(mc, new_arrays)) {
		mc.surface_arrays = new_arrays; // GPU buffers updated in place, no rebuild
		mc.revision++;
		for (const KeyValue<String, uint64_t> &kv : nodes) {
			if (kv.key.begins_with("o:") && kv.key.substr(2) == key) {
				collision_dirty.insert(kv.key);
			}
		}
		return;
	}

	mc.mesh->set_custom_aabb(AABB());
	mc.mesh->clear_surfaces();
	for (int i = 0; i < new_arrays.size(); i++) {
		mc.mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, new_arrays[i]);
		Ref<StandardMaterial3D> mat = _material(new_mats[i]);
		if (mat.is_valid()) {
			mc.mesh->surface_set_material(i, mat);
		}
	}
	mc.surface_arrays = new_arrays;
	mc.surface_mats = new_mats;
	mc.revision++;
	if (!pos_only || !mc.fast_verified) {
		_verify_fast_path(mc, key); // topology changed: re-check the direct-update encoder
	}

	// Collisions built from this mesh need regenerating; instances need the mesh hooked up.
	for (const KeyValue<String, uint64_t> &kv : nodes) {
		if (kv.key.begins_with("o:") && kv.key.substr(2) == key) {
			collision_dirty.insert(kv.key);
		}
	}
	for (const KeyValue<String, String> &kv : instance_mesh) {
		if (kv.value == key) {
			_apply_instances(kv.key);
		}
	}
	scene_changed = true;
}

// ---------------------------------------------------------------------------- objects / collections / instances

void BlenderLink::_on_object(const Dictionary &h) {
	String name = h.get("name", "");
	if (name.is_empty()) {
		return;
	}
	objects[name] = h;
	_apply_object(name, h);
}

void BlenderLink::_on_collection(const Dictionary &h) {
	String name = h.get("name", "");
	if (name.is_empty()) {
		return;
	}
	collections[name] = h;
	_apply_collection(name, h);
}

void BlenderLink::_on_instances(const Dictionary &h, const uint8_t *blob, int64_t blob_len) {
	String key = h.get("key", "");
	int64_t count = int64_t(h.get("count", 0));
	if (key.is_empty() || count < 0 || count * 48 > blob_len) {
		return;
	}
	PackedFloat32Array buf;
	buf.resize(count * 12);
	memcpy(buf.ptrw(), blob, count * 48);
	instance_xforms[key] = buf;
	instance_mesh[key] = h.get("mesh", "");
	_apply_instances(key);
}

void BlenderLink::_on_scene(const Dictionary &h) {
	scene_received = true;
	auto to_set = [](const Array &a) {
		HashSet<String> s;
		for (int i = 0; i < a.size(); i++) {
			s.insert(a[i]);
		}
		return s;
	};
	HashSet<String> keep_obj = to_set(h.get("objects", Array()));
	HashSet<String> keep_col = to_set(h.get("collections", Array()));

	Array names = objects.keys();
	for (int i = 0; i < names.size(); i++) {
		if (!keep_obj.has(names[i])) {
			objects.erase(names[i]);
			_remove_node("o:" + String(names[i]));
		}
	}
	names = collections.keys();
	for (int i = 0; i < names.size(); i++) {
		if (!keep_col.has(names[i])) {
			collections.erase(names[i]);
			_remove_node("c:" + String(names[i]));
		}
	}
	if (h.has("instances")) {
		HashSet<String> keep_inst = to_set(h["instances"]);
		Vector<String> dead;
		for (const KeyValue<String, PackedFloat32Array> &kv : instance_xforms) {
			if (!keep_inst.has(kv.key)) {
				dead.push_back(kv.key);
			}
		}
		for (const String &k : dead) {
			instance_xforms.erase(k);
			instance_mesh.erase(k);
			_remove_node("i:" + k);
		}
	}

	// Drop meshes nobody references any more.
	HashSet<String> used;
	Array vals = objects.values();
	for (int i = 0; i < vals.size(); i++) {
		Dictionary o = vals[i];
		if (o.has("mesh")) {
			used.insert(o["mesh"]);
		}
	}
	for (const KeyValue<String, String> &kv : instance_mesh) {
		used.insert(kv.value);
	}
	Vector<String> dead;
	for (const KeyValue<String, MeshCache> &kv : meshes) {
		if (!used.has(kv.key)) {
			dead.push_back(kv.key);
		}
	}
	for (const String &k : dead) {
		meshes.erase(k);
	}

	String summary = vformat("%d objects, %d collections, %d instance groups, %d meshes | %s: %d drawn directly, %d nodes",
			objects.size(), collections.size(), (int)instance_xforms.size(), (int)meshes.size(),
			preview_mode ? "preview" : "scene", (int)preview.size(), (int)nodes.size());
	if (summary != last_summary) {
		last_summary = summary;
		UtilityFunctions::print("Blender Dock: ", summary);
	}
}

void BlenderLink::_on_selection(const Dictionary &h) {
	if (preview_mode) {
		return; // no nodes to select in preview mode: leave Godot's selection alone
	}
	PackedStringArray names = PackedStringArray(Array(h.get("names", Array())));
	names.sort();
	if (names == last_sel || !instance_from<Node3D>(link_root_id)) {
		last_sel = names;
		return;
	}
	last_sel = names;
	EditorSelection *es = EditorInterface::get_singleton()->get_selection();
	es->clear();
	String active = h.get("active", "");
	Node *active_node = nullptr;
	for (const String &n : names) {
		Node3D *node = _node("o:" + n);
		if (node) {
			es->add_node(node);
			if (n == active) {
				active_node = node;
			}
		}
	}
	if (active_node) {
		// inspect_object (not edit_node): updates the Inspector without switching main screens.
		EditorInterface::get_singleton()->inspect_object(active_node, "", true);
	}
}

// ---------------------------------------------------------------------------- two-way (Godot -> Blender)

void BlenderLink::on_editor_selection_changed() {
	Node3D *lr = instance_from<Node3D>(link_root_id);
	if (!lr || !is_peer_connected() || preview_mode) {
		return; // preview mode: Godot selections never touch Blender's selection
	}
	TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
	PackedStringArray names;
	for (int i = 0; i < sel.size(); i++) {
		Node *n = Object::cast_to<Node>(sel[i]);
		if (n && n->has_meta(NAME_META)) {
			String key = n->get_meta(NAME_META);
			if (key.begins_with("o:")) {
				names.push_back(key.substr(2));
			}
		}
	}
	names.sort();
	if (names == last_sel) {
		return;
	}
	last_sel = names;
	Dictionary d;
	d["t"] = "sel";
	d["names"] = names;
	d["active"] = names.size() == 1 ? names[0] : String();
	send(d);
}

void BlenderLink::watch_selected_transforms() {
	Node3D *lr = instance_from<Node3D>(link_root_id);
	if (!lr || !is_peer_connected() || now_ms() < next_xform_ms) {
		return;
	}
	next_xform_ms = now_ms() + 33;
	TypedArray<Node> sel = EditorInterface::get_singleton()->get_selection()->get_selected_nodes();
	for (int i = 0; i < sel.size(); i++) {
		Node3D *n = Object::cast_to<Node3D>(sel[i]);
		if (!n || !n->has_meta(NAME_META) || !n->is_inside_tree()) {
			continue;
		}
		String key = n->get_meta(NAME_META);
		if (!key.begins_with("o:")) {
			continue;
		}
		Transform3D local = n->get_transform();
		Transform3D *applied = applied_xform.getptr(key);
		if (applied && applied->is_equal_approx(local)) {
			continue;
		}
		applied_xform[key] = local;
		local_edit_until[key] = now_ms() + 400;
		Dictionary d;
		d["t"] = "xform";
		d["name"] = key.substr(2);
		d["xform"] = xform_list(lr->get_global_transform().affine_inverse() * n->get_global_transform());
		send(d);
	}
}

// ---------------------------------------------------------------------------- scene

Node3D *BlenderLink::_find_link_root() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!root) {
		return nullptr;
	}
	for (int i = 0; i < root->get_child_count(); i++) {
		Node3D *n = Object::cast_to<Node3D>(root->get_child(i));
		if (n && n->has_meta(ROOT_META)) {
			return n;
		}
	}
	return nullptr;
}

Node3D *BlenderLink::get_link_root() {
	return instance_from<Node3D>(link_root_id);
}

bool BlenderLink::edited_scene_has_link() {
	return instance_from<Node3D>(link_root_id) != nullptr;
}

bool BlenderLink::add_to_edited_scene() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!Object::cast_to<Node3D>(root)) {
		return false;
	}
	if (!_find_link_root()) {
		Node3D *lr = memnew(Node3D);
		lr->set_name("BlenderLink");
		lr->set_meta(ROOT_META, true);
		lr->set_meta(MODE_META, "preview");
		root->add_child(lr, true);
		lr->set_owner(root);
	}
	_bind_scene();
	EditorInterface::get_singleton()->mark_scene_as_unsaved();
	return true;
}

Node3D *BlenderLink::_node(const String &p_key) {
	uint64_t *id = nodes.getptr(p_key);
	return id ? instance_from<Node3D>(*id) : nullptr;
}

void BlenderLink::_own_recursive(Node *p_node, Node *p_owner) {
	if (p_node != p_owner) {
		p_node->set_owner(p_owner);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_own_recursive(p_node->get_child(i), p_owner);
	}
}

void BlenderLink::_bind_scene() {
	_preview_clear();
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	bound_scene_id = root ? uint64_t(root->get_instance_id()) : 0;
	nodes.clear();
	pending_parent.clear();
	applied_xform.clear();
	Node3D *lr = _find_link_root();
	link_root_id = lr ? uint64_t(lr->get_instance_id()) : 0;
	if (!lr) {
		return;
	}
	// Index existing nodes (saved scenes), anywhere below the link root.
	Vector<Node *> stack;
	stack.push_back(lr);
	while (!stack.is_empty()) {
		Node *n = stack[stack.size() - 1];
		stack.remove_at(stack.size() - 1);
		for (int i = 0; i < n->get_child_count(); i++) {
			Node *c = n->get_child(i);
			if (Node3D *c3 = Object::cast_to<Node3D>(c)) {
				if (c3->has_meta(NAME_META)) {
					String key = c3->get_meta(NAME_META);
					if (key.find(":") != 1) {
						key = "o:" + key; // scenes saved by the first version
						c3->set_meta(NAME_META, key);
					}
					nodes[key] = uint64_t(c3->get_instance_id());
				}
			}
			stack.push_back(c);
		}
	}
	// Mode: stored on the BlenderLink node. Links made before preview mode existed already hold
	// real nodes, so they stay in scene mode; new links start in preview mode.
	if (lr->has_meta(MODE_META)) {
		preview_mode = String(lr->get_meta(MODE_META)) == "preview";
	} else {
		preview_mode = nodes.is_empty();
	}
	if (preview_mode) {
		Vector<String> mirrored; // nodes left from scene mode are regenerated on demand
		for (const KeyValue<String, uint64_t> &kv : nodes) {
			mirrored.push_back(kv.key);
		}
		for (const String &k : mirrored) {
			_remove_node(k);
		}
		if (sync_enabled) {
			_preview_rebuild();
		}
		return;
	}
	if (!sync_enabled) {
		return;
	}
	Array names = collections.keys();
	for (int i = 0; i < names.size(); i++) {
		_apply_collection(names[i], collections[names[i]]);
	}
	names = objects.keys();
	for (int i = 0; i < names.size(); i++) {
		_apply_object(names[i], objects[names[i]]);
	}
	for (const KeyValue<String, PackedFloat32Array> &kv : instance_xforms) {
		_apply_instances(kv.key);
	}
	Vector<String> stale;
	for (const KeyValue<String, uint64_t> &kv : nodes) {
		String rest = kv.key.substr(2);
		bool alive = (kv.key.begins_with("o:") && objects.has(rest)) || (kv.key.begins_with("c:") && collections.has(rest)) ||
				(kv.key.begins_with("i:") && instance_xforms.has(rest));
		if (!alive && (objects.size() || collections.size())) {
			stale.push_back(kv.key);
		}
	}
	for (const String &k : stale) {
		_remove_node(k);
	}
}

void BlenderLink::_remove_node(const String &p_key) {
	_preview_remove(p_key);
	uint64_t *id = nodes.getptr(p_key);
	if (!id) {
		return;
	}
	Node3D *n = instance_from<Node3D>(*id);
	nodes.erase(p_key);
	applied_xform.erase(p_key);
	if (!n) {
		return;
	}
	// Rescue linked children (they may just be moving to another parent).
	Node3D *lr = instance_from<Node3D>(link_root_id);
	for (int i = n->get_child_count() - 1; i >= 0; i--) {
		Node3D *c = Object::cast_to<Node3D>(n->get_child(i));
		if (c && c->has_meta(NAME_META) && lr) {
			c->reparent(lr, false);
			_own_recursive(c, lr->get_owner() ? lr->get_owner() : lr);
		}
	}
	n->get_parent()->remove_child(n);
	n->queue_free();
	scene_changed = true;
}

void BlenderLink::_place(Node3D *p_node, const String &p_parent_key, const String &p_key) {
	Node3D *lr = instance_from<Node3D>(link_root_id);
	Node3D *target = p_parent_key.is_empty() ? lr : _node(p_parent_key);
	if (!target) {
		target = lr;
		pending_parent[p_key] = p_parent_key;
	} else {
		pending_parent.erase(p_key);
	}
	Node *scene_root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!p_node->get_parent()) {
		target->add_child(p_node, true);
		_own_recursive(p_node, scene_root);
	} else if (p_node->get_parent() != target) {
		p_node->reparent(target, false);
		_own_recursive(p_node, scene_root);
	}
}

void BlenderLink::_resolve_parents() {
	Vector<String> keys;
	for (const KeyValue<String, String> &kv : pending_parent) {
		if (_node(kv.value)) {
			keys.push_back(kv.key);
		}
	}
	for (const String &k : keys) {
		String rest = k.substr(2);
		if (k.begins_with("o:") && objects.has(rest)) {
			_apply_object(rest, objects[rest]);
		} else if (k.begins_with("c:") && collections.has(rest)) {
			_apply_collection(rest, collections[rest]);
		} else {
			pending_parent.erase(k);
		}
	}
}

void BlenderLink::_apply_collection(const String &p_name, const Dictionary &h) {
	if (!sync_enabled || !instance_from<Node3D>(link_root_id) || preview_mode) {
		return; // collections have no geometry of their own: nothing to preview
	}
	String key = "c:" + p_name;
	Node3D *node = _node(key);
	if (!node) {
		node = memnew(Node3D);
		node->set_name(p_name.validate_node_name());
		node->set_meta(NAME_META, key);
		nodes[key] = uint64_t(node->get_instance_id());
	}
	_place(node, h.get("parent", ""), key);
	bool vis = h.get("visible", true);
	if (node->is_visible() != vis) {
		node->set_visible(vis);
	}
}

void BlenderLink::_apply_instances(const String &p_key) {
	if (!sync_enabled || !instance_from<Node3D>(link_root_id)) {
		return;
	}
	if (preview_mode) {
		_preview_instances(p_key);
		return;
	}
	PackedFloat32Array *buf = instance_xforms.getptr(p_key);
	if (!buf) {
		return;
	}
	String key = "i:" + p_key;
	MultiMeshInstance3D *mmi = Object::cast_to<MultiMeshInstance3D>(_node(key));
	if (!mmi) {
		mmi = memnew(MultiMeshInstance3D);
		mmi->set_name(("Instances " + p_key.replace("/", " ")).validate_node_name());
		mmi->set_meta(NAME_META, key);
		nodes[key] = uint64_t(mmi->get_instance_id());
		_place(mmi, "", key);
	}
	Ref<MultiMesh> mm = mmi->get_multimesh();
	if (mm.is_null()) {
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mmi->set_multimesh(mm);
	}
	MeshCache *mc = meshes.getptr(instance_mesh[p_key]);
	Ref<Mesh> mesh = mc ? Ref<Mesh>(mc->mesh) : Ref<Mesh>();
	if (mm->get_mesh() != mesh) {
		mm->set_mesh(mesh);
	}
	int count = buf->size() / 12;
	if (mm->get_instance_count() != count) {
		mm->set_instance_count(count);
	}
	if (count > 0) {
		mm->set_buffer(*buf);
	}
	scene_changed = true;
}

void BlenderLink::_update_collision(Node3D *p_node, const String &p_key, const Dictionary &h) {
	String kind = h.get("col", "");
	StaticBody3D *body = nullptr;
	for (int i = 0; i < p_node->get_child_count(); i++) {
		body = Object::cast_to<StaticBody3D>(p_node->get_child(i));
		if (body && body->has_meta("blender_collision")) {
			break;
		}
		body = nullptr;
	}
	if (kind.is_empty() || kind == "none") {
		if (body) {
			p_node->remove_child(body);
			body->queue_free();
			collision_rev.erase(p_key);
		}
		return;
	}
	MeshCache *mc = meshes.getptr(h.get("mesh", ""));
	if (!mc || mc->mesh.is_null() || mc->mesh->get_surface_count() == 0) {
		return;
	}
	String kind_key = p_key + String("#") + kind;
	uint64_t *rev = collision_rev.getptr(kind_key);
	if (body && rev && *rev == mc->revision) {
		return;
	}
	Ref<Shape3D> shape = kind == "convex" ? Ref<Shape3D>(mc->mesh->create_convex_shape(true, false))
										  : Ref<Shape3D>(mc->mesh->create_trimesh_shape());
	if (shape.is_null()) {
		return;
	}
	Node *scene_root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (!body) {
		body = memnew(StaticBody3D);
		body->set_name("Collision");
		body->set_meta("blender_collision", true);
		CollisionShape3D *cs = memnew(CollisionShape3D);
		cs->set_name("Shape");
		body->add_child(cs);
		p_node->add_child(body);
		_own_recursive(body, scene_root);
		body->set_owner(scene_root);
	}
	CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(body->get_child(0));
	if (cs) {
		cs->set_shape(shape);
	}
	collision_rev[kind_key] = mc->revision;
	scene_changed = true;
}

void BlenderLink::_refresh_collisions() {
	Vector<String> keys;
	for (const String &k : collision_dirty) {
		keys.push_back(k);
	}
	collision_dirty.clear();
	for (const String &k : keys) {
		Node3D *n = _node(k);
		String name = k.substr(2);
		if (n && objects.has(name)) {
			_update_collision(n, k, objects[name]);
		}
	}
}

void BlenderLink::_apply_object(const String &p_name, const Dictionary &h) {
	if (!sync_enabled) {
		return;
	}
	Node3D *lr = instance_from<Node3D>(link_root_id);
	if (OS::get_singleton()->get_environment("BLENDER_DOCK_DEBUG") == "1") {
		UtilityFunctions::print("Blender Dock debug: apply ", p_name, " root=", lr != nullptr, " preview=", preview_mode);
	}
	if (!lr) {
		return;
	}
	if (preview_mode) {
		_preview_object(p_name, h);
		return;
	}
	if (OS::get_singleton()->get_environment("BLENDER_DOCK_DEBUG") == "1") {
		UtilityFunctions::print("Blender Dock debug: scene-mode apply ", p_name);
	}
	String key = "o:" + p_name;
	String kind = h.get("kind", "empty");

	Node3D *node = _node(key);
	if (node && String(node->get_meta("blender_kind", "")) != kind) {
		_remove_node(key);
		node = nullptr;
	}
	bool created = false;
	if (!node) {
		if (kind == "mesh") {
			node = memnew(MeshInstance3D);
		} else if (kind == "omni") {
			node = memnew(OmniLight3D);
		} else if (kind == "spot") {
			node = memnew(SpotLight3D);
		} else if (kind == "sun") {
			node = memnew(DirectionalLight3D);
		} else if (kind == "camera") {
			node = memnew(Camera3D);
		} else {
			node = memnew(Node3D);
		}
		node->set_name(p_name.validate_node_name());
		node->set_meta(NAME_META, key);
		node->set_meta("blender_kind", kind);
		nodes[key] = uint64_t(node->get_instance_id());
		created = true;
	}
	_place(node, h.get("parent", ""), key);

	uint64_t *until = local_edit_until.getptr(key);
	bool godot_editing = until && now_ms() < *until;
	// Only touch what changed: every property write makes the editor refresh the Inspector /
	// scene tree / gizmos, which is where idle-time hitches came from.
	Array x = h.get("xform", Array());
	if (x.size() == 12 && !godot_editing) {
		Basis b(Vector3(x[0], x[1], x[2]), Vector3(x[3], x[4], x[5]), Vector3(x[6], x[7], x[8]));
		Transform3D t(b, Vector3(x[9], x[10], x[11]));
		if (!node->get_transform().is_equal_approx(t)) {
			node->set_transform(t);
		}
		applied_xform[key] = t;
	}
	bool vis = h.get("visible", true);
	if (node->is_visible() != vis) {
		node->set_visible(vis);
	}

	if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(node)) {
		MeshCache *mc = meshes.getptr(h.get("mesh", ""));
		Ref<Mesh> want = (mc && !bool(h.get("colonly", false))) ? Ref<Mesh>(mc->mesh) : Ref<Mesh>();
		if (mi->get_mesh() != want) {
			mi->set_mesh(want);
		}
		GeometryInstance3D::ShadowCastingSetting sh = bool(h.get("shadow", true)) ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON
																				 : GeometryInstance3D::SHADOW_CASTING_SETTING_OFF;
		if (mi->get_cast_shadows_setting() != sh) {
			mi->set_cast_shadows_setting(sh);
		}
		collision_dirty.insert(key);
	} else if (Light3D *l = Object::cast_to<Light3D>(node)) {
		auto param = [l](Light3D::Param p, double v) {
			if (!Math::is_equal_approx(double(l->get_param(p)), v)) {
				l->set_param(p, v);
			}
		};
		Color col = to_color(h.get("color", Variant()));
		if (!l->get_color().is_equal_approx(col)) {
			l->set_color(col);
		}
		param(Light3D::PARAM_ENERGY, h.get("energy", 1.0));
		bool shadow = h.get("shadow", true);
		if (l->has_shadow() != shadow) {
			l->set_shadow(shadow);
		}
		if (Object::cast_to<OmniLight3D>(l) || Object::cast_to<SpotLight3D>(l)) {
			param(Light3D::PARAM_RANGE, h.get("range", 10.0));
		}
		if (Object::cast_to<SpotLight3D>(l)) {
			param(Light3D::PARAM_SPOT_ANGLE, h.get("spot_angle", 45.0));
			param(Light3D::PARAM_SPOT_ATTENUATION, h.get("spot_attenuation", 1.0));
		}
	} else if (Camera3D *c = Object::cast_to<Camera3D>(node)) {
		Camera3D::KeepAspect ka = String(h.get("fit", "width")) == "width" ? Camera3D::KEEP_WIDTH : Camera3D::KEEP_HEIGHT;
		if (c->get_keep_aspect_mode() != ka) {
			c->set_keep_aspect_mode(ka);
		}
		bool ortho = String(h.get("projection", "persp")) == "ortho";
		Camera3D::ProjectionType pt = ortho ? Camera3D::PROJECTION_ORTHOGONAL : Camera3D::PROJECTION_PERSPECTIVE;
		if (c->get_projection() != pt) {
			c->set_projection(pt);
		}
		double size = h.get("size", 10.0), fov = h.get("fov", 75.0), near_v = h.get("near", 0.05), far_v = h.get("far", 1000.0);
		if (ortho && !Math::is_equal_approx(double(c->get_size()), size)) {
			c->set_size(size);
		}
		if (!ortho && !Math::is_equal_approx(double(c->get_fov()), fov)) {
			c->set_fov(fov);
		}
		if (!Math::is_equal_approx(double(c->get_near()), near_v)) {
			c->set_near(near_v);
		}
		if (!Math::is_equal_approx(double(c->get_far()), far_v)) {
			c->set_far(far_v);
		}
	}
	(void)created;
	scene_changed = true;
}
