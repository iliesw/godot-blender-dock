#include "blender_dock_plugin.h"
#include "blender_link.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/editor_plugin_registration.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static void initialize_blender_dock(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_EDITOR) {
		return;
	}
	GDREGISTER_INTERNAL_CLASS(BlenderLink);
	GDREGISTER_INTERNAL_CLASS(BlenderDockPlugin);
	EditorPlugins::add_by_type<BlenderDockPlugin>();
}

static void uninitialize_blender_dock(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_EDITOR) {
		return;
	}
	EditorPlugins::remove_by_type<BlenderDockPlugin>();
}

extern "C" {
GDExtensionBool GDE_EXPORT blender_dock_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(initialize_blender_dock);
	init_obj.register_terminator(uninitialize_blender_dock);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_EDITOR);
	return init_obj.init();
}
}
