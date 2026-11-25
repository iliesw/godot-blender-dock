# blender -b --factory-startup --python tests/test_focus_hook.py
import ctypes, os
dll = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "demo", "addons", "blender_dock", "bin", "blender_focus_hook.dll")
h = ctypes.CDLL(dll)
h.bdh_update.argtypes = [ctypes.c_uint]
h.bdh_update.restype = ctypes.c_int
n = h.bdh_update(os.getpid() + 1)   # background mode: no GHOST windows to hook
h.bdh_shutdown()
print("HOOK OK windows_hooked=", n)
