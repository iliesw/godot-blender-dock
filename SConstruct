#!/usr/bin/env python
# Builds the Blender Dock GDExtension (Windows, Godot 4.7 editor).
#   scons                 -> demo/addons/blender_dock/bin/blender_dock.windows.editor.x86_64.dll

ARGUMENTS.setdefault("target", "editor")  # editor-only plugin
env = SConscript("godot-cpp/SConstruct", {"api_version": "4.7"})

env.Append(CPPPATH=["src/"])
if env["platform"] == "windows":
    env.Append(LIBS=["user32", "gdi32", "shell32"])

sources = Glob("src/*.cpp")

library = env.SharedLibrary(
    "demo/addons/blender_dock/bin/blender_dock{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
    source=sources,
)
env.NoCache(library)

# Small native helper loaded INTO Blender (ctypes) so Blender does not lose focus to the Godot
# that docks it. Plain Win32: no godot-cpp.
hook = None
if env["platform"] == "windows":
    henv = Environment(tools=["msvc", "mslink"], TARGET_ARCH="x86_64")
    henv.Append(CCFLAGS=["/O2", "/EHsc", "/MT", "/nologo"], LIBS=["user32"])
    hook = henv.SharedLibrary("demo/addons/blender_dock/bin/blender_focus_hook", ["src_hook/blender_focus_hook.cpp"])
Default(library, hook) if hook else Default(library)
