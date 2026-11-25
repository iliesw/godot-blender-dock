# Lists Blender (GHOST) windows of a process: visibility and screen rectangle.
Add-Type @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class R {
  delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EP f, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [StructLayout(LayoutKind.Sequential)] struct RC { public int l, t, r, b; }
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RC r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public static List<string> List(uint pid) {
    var o = new List<string>();
    EnumWindows((h, l) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid) {
        var sb = new StringBuilder(64); GetClassName(h, sb, 64);
        RC r; GetWindowRect(h, out r);
        o.Add(sb + " visible=" + IsWindowVisible(h) + " rect=(" + r.l + "," + r.t + ")-(" + r.r + "," + r.b + ")");
      }
      return true;
    }, IntPtr.Zero);
    return o;
  }
}
'@
[R]::SetProcessDPIAware() | Out-Null
