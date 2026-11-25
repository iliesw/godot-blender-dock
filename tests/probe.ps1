param([int]$X, [int]$Y)
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public class P {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct PT { public int x, y; }
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(PT p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [StructLayout(LayoutKind.Sequential)] public struct GTI { public int cb, flags; public IntPtr act, foc, cap, menu, move, caret; public int l,t,r,b; }
  [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint tid, ref GTI g);
}
'@
[P]::SetProcessDPIAware() | Out-Null
$pt = New-Object P+PT; $pt.x = $X; $pt.y = $Y
$h = [P]::WindowFromPoint($pt); $pid2 = 0; [P]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
$sb = New-Object Text.StringBuilder 64; [P]::GetClassName($h, $sb, 64) | Out-Null
$g = New-Object P+GTI; $g.cb = [Runtime.InteropServices.Marshal]::SizeOf($g); [P]::GetGUIThreadInfo(0, [ref]$g) | Out-Null
$fp = 0; [P]::GetWindowThreadProcessId([P]::GetForegroundWindow(), [ref]$fp) | Out-Null
"under=($X,$Y) pid=$pid2 class=$($sb.ToString()) | fgpid=$fp capture=$($g.cap) focus=$($g.foc)"
