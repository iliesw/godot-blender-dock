#include "blender_link.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/spot_light3d.hpp>
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

void BlenderLink::poll(bool p_sync_enabled) {
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

	// Follow the editor's current scene.
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	uint64_t rid = root ? uint64_t(root->get_instance_id()) : 0;
	if (rid != bound_scene_id || (link_root_id && !instance_from<Node3D>(link_root_id))) {
		_bind_scene();
	}

	if (scene_changed) {
		scene_changed = false;
		if (instance_from<Node3D>(link_root_id)) {
			EditorInterface::get_singleton()->mark_scene_as_unsaved();
		}
	}
}

void BlenderLink::_read_peer() {
	// Drain everything available this tick (bounded so a huge burst can't freeze the editor).
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
	while (size - inbuf_off >= 8) {
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
			break; // incomplete
		}
		const uint8_t *f = data + inbuf_off + 4;
		uint32_t hl = rd_u32(f);
		if (hl > frame_len - 4) {
			inbuf_off += 4 + frame_len;
			continue;
		}
		String hs = String::utf8(reinterpret_cast<const char *>(f + 4), hl);
		Variant hv = JSON::parse_string(hs);
		if (hv.get_type() == Variant::DICTIONARY) {
			_handle_frame(hv, f + 4 + hl, int64_t(frame_len) - 4 - hl);
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
	} else if (t == "scene") {
		_on_scene(h);
	} else if (t == "hello") {
		blend_file = h.get("file", "");
		UtilityFunctions::print("Blender Dock: linked to Blender ", h.get("blender", ""), " ",
				blend_file.is_empty() ? String("(unsaved file)") : blend_file);
	}
}

// ---------------------------------------------------------------------------- data

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
	m->set_metallic(h.get("metallic", 0.0));
	m->set_roughness(h.get("roughness", 0.5));
	m->set_specular(h.get("specular", 0.5));

	Color e = to_color(h.get("emission", Variant()), Color(0, 0, 0));
	double es = h.get("emission_strength", 0.0);
	bool emit = es > 0.0 && (e.r + e.g + e.b) > 0.0;
	m->set_feature(BaseMaterial3D::FEATURE_EMISSION, emit);
	if (emit) {
		m->set_emission(Color(e.r, e.g, e.b));
		m->set_emission_energy_multiplier(es);
	}

	bool blend = h.get("alpha_blend", false);
	m->set_transparency(blend ? BaseMaterial3D::TRANSPARENCY_ALPHA
							  : (c.a < 0.999f ? BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS
											  : BaseMaterial3D::TRANSPARENCY_DISABLED));
	m->set_cull_mode(bool(h.get("double_sided", false)) ? BaseMaterial3D::CULL_DISABLED : BaseMaterial3D::CULL_BACK);

	String tex = h.get("texture", "");
	String *cur = material_textures.getptr(m->get_name());
	if (!cur || *cur != tex) {
		Ref<Texture2D> t;
		if (!tex.is_empty()) {
			Ref<Image> img = Image::load_from_file(tex);
			if (img.is_valid() && !img->is_empty()) {
				img->generate_mipmaps();
				t = ImageTexture::create_from_image(img);
			}
		}
		m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, t);
		material_textures[m->get_name()] = tex;
	}
}

void BlenderLink::_on_mesh(const Dictionary &h, const uint8_t *blob, int64_t blob_len) {
	String key = h.get("key", "");
	if (key.is_empty()) {
		return;
	}
	Ref<ArrayMesh> mesh;
	if (Ref<ArrayMesh> *found = meshes.getptr(key)) {
		mesh = *found;
	} else {
		mesh.instantiate();
		mesh->set_name(key);
		meshes[key] = mesh;
	}
	mesh->clear_surfaces();

	Array surfaces = h.get("surfaces", Array());
	for (int i = 0; i < surfaces.size(); i++) {
		Dictionary s = surfaces[i];
		int64_t nv = int64_t(s.get("nv", 0));
		int64_t ni = int64_t(s.get("ni", 0));
		bool has_uv = s.get("uv", false);
		int64_t off = int64_t(s.get("off", 0));
		int64_t need = nv * 12 * 2 + (has_uv ? nv * 8 : 0) + ni * 4;
		if (nv <= 0 || ni <= 0 || off < 0 || off + need > blob_len) {
			continue;
		}
		const uint8_t *p = blob + off;

		PackedVector3Array pos;
		pos.resize(nv);
		memcpy(pos.ptrw(), p, nv * 12);
		p += nv * 12;
		PackedVector3Array nrm;
		nrm.resize(nv);
		memcpy(nrm.ptrw(), p, nv * 12);
		p += nv * 12;
		PackedVector2Array uv;
		if (has_uv) {
			uv.resize(nv);
			memcpy(uv.ptrw(), p, nv * 8);
			p += nv * 8;
		}
		PackedInt32Array idx;
		idx.resize(ni);
		memcpy(idx.ptrw(), p, ni * 4);

		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = pos;
		arrays[Mesh::ARRAY_NORMAL] = nrm;
		if (has_uv) {
			arrays[Mesh::ARRAY_TEX_UV] = uv;
		}
		arrays[Mesh::ARRAY_INDEX] = idx;
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		int si = mesh->get_surface_count() - 1;
		Ref<StandardMaterial3D> mat = _material(s.get("mat", ""));
		if (mat.is_valid()) {
			mesh->surface_set_material(si, mat);
		}
	}
	scene_changed = true;
}

void BlenderLink::_on_object(const Dictionary &h) {
	String name = h.get("name", "");
	if (name.is_empty()) {
		return;
	}
	objects[name] = h;
	_apply_object(name, h);
}

void BlenderLink::_on_scene(const Dictionary &h) {
	Array list = h.get("objects", Array());
	Dictionary keep;
	for (int i = 0; i < list.size(); i++) {
		keep[list[i]] = true;
	}
	Array names = objects.keys();
	for (int i = 0; i < names.size(); i++) {
		String n = names[i];
		if (!keep.has(n)) {
			objects.erase(n);
			_remove_object_node(n);
		}
	}
	// Drop meshes nobody references any more.
	Dictionary used;
	Array vals = objects.values();
	for (int i = 0; i < vals.size(); i++) {
		Dictionary o = vals[i];
		if (o.has("mesh")) {
			used[o["mesh"]] = true;
		}
	}
	Vector<String> dead;
	for (const KeyValue<String, Ref<ArrayMesh>> &kv : meshes) {
		if (!used.has(kv.key)) {
			dead.push_back(kv.key);
		}
	}
	for (const String &k : dead) {
		meshes.erase(k);
	}
	if (objects.size() != last_reported) {
		last_reported = objects.size();
		PackedStringArray in_scene;
		for (const KeyValue<String, uint64_t> &kv : nodes) {
			if (Node3D *n = instance_from<Node3D>(kv.value)) {
				in_scene.push_back(String(n->get_name()) + "(" + n->get_class() + ")");
			}
		}
		UtilityFunctions::print("Blender Dock: ", objects.size(), " Blender objects, ", meshes.size(),
				" meshes; in scene: ", String(", ").join(in_scene));
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
		root->add_child(lr, true);
		lr->set_owner(root);
	}
	_bind_scene();
	EditorInterface::get_singleton()->mark_scene_as_unsaved();
	return true;
}

void BlenderLink::_bind_scene() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	bound_scene_id = root ? uint64_t(root->get_instance_id()) : 0;
	nodes.clear();
	Node3D *lr = _find_link_root();
	link_root_id = lr ? uint64_t(lr->get_instance_id()) : 0;
	if (!lr) {
		return;
	}
	for (int i = 0; i < lr->get_child_count(); i++) {
		Node3D *n = Object::cast_to<Node3D>(lr->get_child(i));
		if (n && n->has_meta(NAME_META)) {
			nodes[String(n->get_meta(NAME_META))] = uint64_t(n->get_instance_id());
		}
	}
	if (!sync_enabled) {
		return;
	}
	// Bring this scene up to date with everything we know.
	Array names = objects.keys();
	for (int i = 0; i < names.size(); i++) {
		_apply_object(names[i], objects[names[i]]);
	}
	Vector<String> stale;
	for (const KeyValue<String, uint64_t> &kv : nodes) {
		if (!objects.has(kv.key)) {
			stale.push_back(kv.key);
		}
	}
	for (const String &n : stale) {
		_remove_object_node(n);
	}
}

void BlenderLink::_remove_object_node(const String &p_name) {
	uint64_t *id = nodes.getptr(p_name);
	if (!id) {
		return;
	}
	if (Node *n = instance_from<Node>(*id)) {
		n->get_parent()->remove_child(n);
		n->queue_free();
		scene_changed = true;
	}
	nodes.erase(p_name);
}

void BlenderLink::_apply_object(const String &p_name, const Dictionary &h) {
	if (!sync_enabled) {
		return;
	}
	Node3D *lr = instance_from<Node3D>(link_root_id);
	if (!lr) {
		return;
	}
	Node *scene_root = EditorInterface::get_singleton()->get_edited_scene_root();
	String kind = h.get("kind", "empty");

	Node3D *node = nullptr;
	if (uint64_t *id = nodes.getptr(p_name)) {
		node = instance_from<Node3D>(*id);
		if (node && String(node->get_meta("blender_kind", "")) != kind) {
			_remove_object_node(p_name);
			node = nullptr;
		}
	}
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
		node->set_meta(NAME_META, p_name);
		node->set_meta("blender_kind", kind);
		lr->add_child(node, true);
		node->set_owner(scene_root);
		nodes[p_name] = uint64_t(node->get_instance_id());
	}

	Array x = h.get("xform", Array());
	if (x.size() == 12) {
		Basis b(Vector3(x[0], x[1], x[2]), Vector3(x[3], x[4], x[5]), Vector3(x[6], x[7], x[8]));
		node->set_transform(Transform3D(b, Vector3(x[9], x[10], x[11])));
	}
	node->set_visible(h.get("visible", true));

	if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(node)) {
		String key = h.get("mesh", "");
		Ref<ArrayMesh> *m = meshes.getptr(key);
		Ref<Mesh> want = m ? Ref<Mesh>(*m) : Ref<Mesh>();
		if (mi->get_mesh() != want) {
			mi->set_mesh(want);
		}
		mi->set_cast_shadows_setting(bool(h.get("shadow", true)) ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON
																  : GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	} else if (Light3D *l = Object::cast_to<Light3D>(node)) {
		l->set_color(to_color(h.get("color", Variant())));
		l->set_param(Light3D::PARAM_ENERGY, h.get("energy", 1.0));
		l->set_shadow(h.get("shadow", true));
		if (OmniLight3D *o = Object::cast_to<OmniLight3D>(l)) {
			o->set_param(Light3D::PARAM_RANGE, h.get("range", 10.0));
		} else if (SpotLight3D *s = Object::cast_to<SpotLight3D>(l)) {
			s->set_param(Light3D::PARAM_RANGE, h.get("range", 10.0));
			s->set_param(Light3D::PARAM_SPOT_ANGLE, h.get("spot_angle", 45.0));
			s->set_param(Light3D::PARAM_SPOT_ATTENUATION, h.get("spot_attenuation", 1.0));
		}
	} else if (Camera3D *c = Object::cast_to<Camera3D>(node)) {
		c->set_keep_aspect_mode(String(h.get("fit", "width")) == "width" ? Camera3D::KEEP_WIDTH : Camera3D::KEEP_HEIGHT);
		if (String(h.get("projection", "persp")) == "ortho") {
			c->set_projection(Camera3D::PROJECTION_ORTHOGONAL);
			c->set_size(h.get("size", 10.0));
		} else {
			c->set_projection(Camera3D::PROJECTION_PERSPECTIVE);
			c->set_fov(h.get("fov", 75.0));
		}
		c->set_near(h.get("near", 0.05));
		c->set_far(h.get("far", 1000.0));
	}
	scene_changed = true;
}
