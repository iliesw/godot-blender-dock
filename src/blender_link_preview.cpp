// Preview mode + fast vertex updates.
//
// Preview mode draws Blender objects straight through RenderingServer (raw instances and
// lights in the editor's 3D world). No nodes are created, so Godot's scene tree, Inspector,
// gizmos and "unsaved" tracking never run for live updates: the editor does no extra work.
//
// Fast vertex updates write moved vertices directly into the existing GPU vertex buffer
// (mesh_surface_update_vertex_region) instead of rebuilding the mesh. The encoder mirrors
// RenderingServer::_surface_set_data and is verified byte-for-byte against the buffer Godot
// itself produced for each surface; any mismatch permanently falls back to a full rebuild.

#include "blender_link.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>

using namespace godot;

namespace {

template <typename T>
T *instance_from(uint64_t id) {
	return id ? Object::cast_to<T>(ObjectDB::get_instance(ObjectID(id))) : nullptr;
}

Transform3D xform_from(const Array &x) {
	if (x.size() != 12) {
		return Transform3D();
	}
	return Transform3D(Basis(Vector3(x[0], x[1], x[2]), Vector3(x[3], x[4], x[5]), Vector3(x[6], x[7], x[8])),
			Vector3(x[9], x[10], x[11]));
}

Color to_color(const Variant &v) {
	Array a = v;
	if (a.size() < 3) {
		return Color(1, 1, 1);
	}
	return Color(a[0], a[1], a[2]);
}

inline void put_oct16(uint8_t *dst, const Vector2 &res, bool tangent_fix) {
	uint16_t v[2] = {
		(uint16_t)CLAMP(res.x * 65535, 0, 65535),
		(uint16_t)CLAMP(res.y * 65535, 0, 65535),
	};
	if (tangent_fix && v[0] == 0 && v[1] == 65535) {
		v[0] = 65535; // same sanitizing as RenderingServer::_surface_set_data
	}
	memcpy(dst, v, 4);
}

} // namespace

// Mirrors the uncompressed path of RenderingServer::_surface_set_data for the vertex buffer
// (positions block, then interleaved normal/tangent block).
bool BlenderLink::_encode_vertex_buffer(uint64_t p_format, const Array &p_arrays, PackedByteArray &r_out) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (p_format & (RenderingServer::ARRAY_FLAG_COMPRESS_ATTRIBUTES | RenderingServer::ARRAY_FLAG_USE_2D_VERTICES)) {
		return false;
	}
	PackedVector3Array pos = p_arrays[Mesh::ARRAY_VERTEX];
	int n = pos.size();
	if (n == 0) {
		return false;
	}
	BitField<RenderingServer::ArrayFormat> fmt = p_format;
	uint32_t vstride = rs->mesh_surface_get_format_vertex_stride(fmt, n);
	uint32_t ntstride = rs->mesh_surface_get_format_normal_tangent_stride(fmt, n);
	uint32_t off_v = rs->mesh_surface_get_format_offset(fmt, n, RenderingServer::ARRAY_VERTEX);
	r_out.resize(int64_t(vstride + ntstride) * n);
	uint8_t *w = r_out.ptrw();
	memset(w, 0, r_out.size());

	const Vector3 *p = pos.ptr();
	for (int i = 0; i < n; i++) {
		float v[3] = { (float)p[i].x, (float)p[i].y, (float)p[i].z };
		memcpy(w + off_v + i * vstride, v, 12);
	}
	if (!(p_format & RenderingServer::ARRAY_FORMAT_NORMAL)) {
		return true;
	}
	PackedVector3Array nrm = p_arrays[Mesh::ARRAY_NORMAL];
	if (nrm.size() != n) {
		return false;
	}
	const Vector3 *np = nrm.ptr();
	uint32_t off_n = rs->mesh_surface_get_format_offset(fmt, n, RenderingServer::ARRAY_NORMAL);
	for (int i = 0; i < n; i++) {
		put_oct16(w + off_n + i * ntstride, np[i].octahedron_encode(), false);
	}
	if (!(p_format & RenderingServer::ARRAY_FORMAT_TANGENT)) {
		return true;
	}
	uint32_t off_t = rs->mesh_surface_get_format_offset(fmt, n, RenderingServer::ARRAY_TANGENT);
	Variant tv = p_arrays[Mesh::ARRAY_TANGENT];
	if (tv.get_type() == Variant::PACKED_FLOAT32_ARRAY) {
		PackedFloat32Array t = tv;
		if (t.size() != n * 4) {
			return false;
		}
		const float *tp = t.ptr();
		for (int i = 0; i < n; i++) {
			Vector3 src(tp[i * 4 + 0], tp[i * 4 + 1], tp[i * 4 + 2]);
			put_oct16(w + off_t + i * ntstride, src.octahedron_tangent_encode(tp[i * 4 + 3]), true);
		}
	} else if (tv.get_type() == Variant::NIL) {
		// Godot generates an arbitrary tangent when only normals are given.
		for (int i = 0; i < n; i++) {
			Vector3 tan = Vector3(np[i].z, -np[i].x, np[i].y).cross(np[i].normalized()).normalized();
			put_oct16(w + off_t + i * ntstride, tan.octahedron_tangent_encode(1.0), true);
		}
	} else {
		return false;
	}
	return true;
}

// After a full rebuild: does our encoder reproduce Godot's buffer exactly for this surface?
void BlenderLink::_verify_fast_path(MeshCache &mc, const String &p_key) {
	RenderingServer *rs = RenderingServer::get_singleton();
	mc.fast_ok.resize(mc.surface_arrays.size());
	bool all = true;
	for (int i = 0; i < mc.surface_arrays.size(); i++) {
		Dictionary sd = rs->mesh_get_surface(mc.mesh->get_rid(), i);
		PackedByteArray actual = sd.get("vertex_data", PackedByteArray());
		PackedByteArray mine;
		bool ok = _encode_vertex_buffer(mc.mesh->surface_get_format(i), mc.surface_arrays[i], mine) &&
				mine.size() == actual.size() && memcmp(mine.ptr(), actual.ptr(), mine.size()) == 0;
		mc.fast_ok.set(i, ok);
		all = all && ok;
	}
	mc.fast_verified = true;
	if (!fast_path_reported) {
		fast_path_reported = true;
		UtilityFunctions::print("Blender Dock: direct GPU vertex updates ", all ? "verified" : "unavailable (falling back to rebuilds)",
				" for '", p_key, "'");
	}
}

// Position-only update without rebuilding the mesh. Returns false if a rebuild is needed.
bool BlenderLink::_fast_vertex_update(MeshCache &mc, const Vector<Array> &p_new_arrays) {
	if (!mc.fast_verified || mc.fast_ok.size() != p_new_arrays.size() || mc.mesh->get_surface_count() != p_new_arrays.size()) {
		return false;
	}
	for (int i = 0; i < mc.fast_ok.size(); i++) {
		if (!mc.fast_ok[i]) {
			return false;
		}
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	AABB box;
	bool first = true;
	Vector<PackedByteArray> bufs;
	for (int i = 0; i < p_new_arrays.size(); i++) {
		PackedByteArray b;
		if (!_encode_vertex_buffer(mc.mesh->surface_get_format(i), p_new_arrays[i], b)) {
			return false;
		}
		bufs.push_back(b);
		PackedVector3Array pos = p_new_arrays[i][Mesh::ARRAY_VERTEX];
		const Vector3 *pp = pos.ptr();
		for (int j = 0; j < pos.size(); j++) {
			if (first) {
				box = AABB(pp[j], Vector3());
				first = false;
			} else {
				box.expand_to(pp[j]);
			}
		}
	}
	for (int i = 0; i < bufs.size(); i++) {
		rs->mesh_surface_update_vertex_region(mc.mesh->get_rid(), i, 0, bufs[i]);
	}
	mc.mesh->set_custom_aabb(box.grow(0.001)); // culling bounds follow the moved vertices
	return true;
}

// ---------------------------------------------------------------------------- preview mode

RID BlenderLink::_preview_scenario() {
	Node3D *lr = instance_from<Node3D>(link_root_id);
	if (!lr || !lr->is_inside_tree()) {
		return RID();
	}
	Ref<World3D> world = lr->get_world_3d();
	return world.is_valid() ? world->get_scenario() : RID();
}

void BlenderLink::_preview_remove(const String &p_key) {
	PreviewItem *it = preview.getptr(p_key);
	if (!it) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (it->instance.is_valid()) {
		rs->free_rid(it->instance);
	}
	if (it->light.is_valid()) {
		rs->free_rid(it->light);
	}
	preview.erase(p_key);
	preview_mm.erase(p_key);
}

void BlenderLink::_preview_clear() {
	Vector<String> keys;
	for (const KeyValue<String, PreviewItem> &kv : preview) {
		keys.push_back(kv.key);
	}
	for (const String &k : keys) {
		_preview_remove(k);
	}
	preview_mm.clear();
}

void BlenderLink::_preview_rebuild() {
	_preview_clear();
	Array names = objects.keys();
	for (int i = 0; i < names.size(); i++) {
		_preview_object(names[i], objects[names[i]]);
	}
	for (const KeyValue<String, PackedFloat32Array> &kv : instance_xforms) {
		_preview_instances(kv.key);
	}
	Node3D *lr = instance_from<Node3D>(link_root_id);
	preview_root_xform = lr ? lr->get_global_transform() : Transform3D();
}

void BlenderLink::_preview_object(const String &p_name, const Dictionary &h) {
	RID scenario = _preview_scenario();
	if (!scenario.is_valid()) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			Node3D *lr = instance_from<Node3D>(link_root_id);
			UtilityFunctions::print("Blender Dock: preview has no 3D world yet (link root ", lr != nullptr, ", in tree ",
					lr && lr->is_inside_tree(), ")");
		}
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	String key = "o:" + p_name;
	String kind = h.get("kind", "empty");
	PreviewItem *it = preview.getptr(key);
	if (it && it->kind != kind) {
		_preview_remove(key);
		it = nullptr;
	}
	if (kind == "empty" || kind == "camera") {
		return; // nothing to draw
	}
	if (!it) {
		PreviewItem item;
		item.kind = kind;
		if (kind == "mesh") {
			String mkey = h.get("mesh", "");
			MeshCache &mc = meshes[mkey];
			if (mc.mesh.is_null()) {
				mc.mesh.instantiate(); // filled when the geometry arrives; same RID
				mc.mesh->set_name(mkey);
			}
			item.mesh = mkey;
			item.instance = rs->instance_create2(mc.mesh->get_rid(), scenario);
		} else {
			if (kind == "sun") {
				item.light = rs->directional_light_create();
			} else if (kind == "spot") {
				item.light = rs->spot_light_create();
			} else {
				item.light = rs->omni_light_create();
			}
			item.instance = rs->instance_create2(item.light, scenario);
		}
		preview[key] = item;
		it = preview.getptr(key);
	}

	Node3D *lr = instance_from<Node3D>(link_root_id);
	Transform3D root = lr ? lr->get_global_transform() : Transform3D();
	Transform3D world = xform_from(h.has("world") ? Array(h["world"]) : Array(h.get("xform", Array())));
	rs->instance_set_transform(it->instance, root * world);
	bool visible = bool(h.get("visible", true)) && !bool(h.get("colonly", false));
	rs->instance_set_visible(it->instance, visible);

	if (kind == "mesh") {
		String mkey = h.get("mesh", "");
		if (mkey != it->mesh) {
			MeshCache &mc = meshes[mkey];
			if (mc.mesh.is_null()) {
				mc.mesh.instantiate();
			}
			rs->instance_set_base(it->instance, mc.mesh->get_rid());
			it->mesh = mkey;
		}
		rs->instance_geometry_set_cast_shadows_setting(it->instance, bool(h.get("shadow", true))
						? RenderingServer::SHADOW_CASTING_SETTING_ON
						: RenderingServer::SHADOW_CASTING_SETTING_OFF);
	} else {
		rs->light_set_color(it->light, to_color(h.get("color", Variant())));
		rs->light_set_param(it->light, RenderingServer::LIGHT_PARAM_ENERGY, h.get("energy", 1.0));
		rs->light_set_shadow(it->light, h.get("shadow", true));
		if (kind != "sun") {
			rs->light_set_param(it->light, RenderingServer::LIGHT_PARAM_RANGE, h.get("range", 10.0));
		}
		if (kind == "spot") {
			rs->light_set_param(it->light, RenderingServer::LIGHT_PARAM_SPOT_ANGLE, h.get("spot_angle", 45.0));
			rs->light_set_param(it->light, RenderingServer::LIGHT_PARAM_SPOT_ATTENUATION, h.get("spot_attenuation", 1.0));
		}
	}
}

void BlenderLink::_preview_instances(const String &p_key) {
	RID scenario = _preview_scenario();
	PackedFloat32Array *buf = instance_xforms.getptr(p_key);
	if (!scenario.is_valid() || !buf) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	String key = "i:" + p_key;
	Ref<MultiMesh> &mm = preview_mm[key];
	if (mm.is_null()) {
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
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
	PreviewItem *it = preview.getptr(key);
	if (!it) {
		PreviewItem item;
		item.kind = "instances";
		item.instance = rs->instance_create2(mm->get_rid(), scenario);
		preview[key] = item;
		it = preview.getptr(key);
	}
	Node3D *lr = instance_from<Node3D>(link_root_id);
	rs->instance_set_transform(it->instance, lr ? lr->get_global_transform() : Transform3D());
}

void BlenderLink::_preview_follow_root() {
	Node3D *lr = instance_from<Node3D>(link_root_id);
	if (!lr || !lr->is_inside_tree()) {
		return;
	}
	Transform3D t = lr->get_global_transform();
	if (t.is_equal_approx(preview_root_xform)) {
		return;
	}
	preview_root_xform = t;
	// The BlenderLink node was moved in Godot: move the whole preview with it.
	Array names = objects.keys();
	for (int i = 0; i < names.size(); i++) {
		if (preview.has("o:" + String(names[i]))) {
			_preview_object(names[i], objects[names[i]]);
		}
	}
	for (const KeyValue<String, PackedFloat32Array> &kv : instance_xforms) {
		_preview_instances(kv.key);
	}
}

bool BlenderLink::set_preview_mode(bool p_preview) {
	Node3D *lr = instance_from<Node3D>(link_root_id);
	if (!lr) {
		return false;
	}
	lr->set_meta(MODE_META, p_preview ? "preview" : "scene");
	EditorInterface::get_singleton()->mark_scene_as_unsaved();
	_bind_scene(); // reads the mode and switches cleanly (frees preview / creates or removes nodes)
	return true;
}

// Builds meshes the way the link does and checks the direct-update encoder against Godot's own
// buffers, then performs a direct update and reads the vertices back.
String BlenderLink::self_test_encoder() {
	Ref<BlenderLink> link;
	link.instantiate();
	PackedStringArray report;
	for (int variant = 0; variant < 3; variant++) {
		const int n = 300;
		PackedVector3Array pos, nrm;
		PackedFloat32Array tan;
		PackedInt32Array idx;
		for (int i = 0; i < n; i++) {
			float a = i * 0.1f;
			pos.push_back(Vector3(Math::cos(a), Math::sin(a * 0.7f), a * 0.01f));
			nrm.push_back(Vector3(Math::sin(a), Math::cos(a), 0.3f * (i % 5 - 2)).normalized());
			Vector3 t = Vector3(Math::cos(a * 1.3f), 0.2f, Math::sin(a * 1.3f)).normalized();
			tan.push_back(t.x);
			tan.push_back(t.y);
			tan.push_back(t.z);
			tan.push_back(i % 3 == 0 ? -1.0f : 1.0f);
		}
		for (int i = 0; i + 2 < n; i += 3) {
			idx.push_back(i);
			idx.push_back(i + 1);
			idx.push_back(i + 2);
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = pos;
		arrays[Mesh::ARRAY_NORMAL] = nrm;
		if (variant != 1) {
			arrays[Mesh::ARRAY_TANGENT] = tan; // variant 1: Godot generates tangents itself
		}
		arrays[Mesh::ARRAY_INDEX] = idx;
		MeshCache mc;
		mc.mesh.instantiate();
		int surfaces = variant == 2 ? 2 : 1;
		for (int s = 0; s < surfaces; s++) {
			mc.mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
			mc.surface_arrays.push_back(arrays);
		}
		link->fast_path_reported = true;
		link->_verify_fast_path(mc, "test");
		bool verified = true;
		for (int s = 0; s < mc.fast_ok.size(); s++) {
			verified = verified && mc.fast_ok[s];
		}
		// Move every vertex, update in place, read back.
		Vector<Array> moved;
		PackedVector3Array pos2 = pos;
		for (int i = 0; i < n; i++) {
			pos2.set(i, pos[i] + Vector3(0.5f, -0.25f, 1.0f));
		}
		for (int s = 0; s < surfaces; s++) {
			Array a2 = arrays.duplicate();
			a2[Mesh::ARRAY_VERTEX] = pos2;
			moved.push_back(a2);
		}
		bool updated = link->_fast_vertex_update(mc, moved);
		PackedVector3Array back = mc.mesh->surface_get_arrays(surfaces - 1)[Mesh::ARRAY_VERTEX];
		bool readback = back.size() == n;
		for (int i = 0; readback && i < n; i++) {
			readback = back[i].is_equal_approx(pos2[i]);
		}
		report.push_back(vformat("variant %d (%s): verified=%s updated=%s readback=%s", variant,
				variant == 0 ? "tangents" : (variant == 1 ? "generated tangents" : "2 surfaces"), verified, updated, readback));
	}
	return String("\n").join(report);
}
