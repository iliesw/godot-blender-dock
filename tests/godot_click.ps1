param([int]$GodotPid, [int]$X, [int]$Y)
Add-Type @'
using System; using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(int f, int x, int y, int d, int e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, int f, int e);
}
'@
[W]::SetProcessDPIAware() | Out-Null
$h = (Get-Process -Id $GodotPid).MainWindowHandle
[W]::keybd_event(0x12,0,0,0); [W]::keybd_event(0x12,0,2,0)   # Alt tap lets SetForegroundWindow succeed
[W]::ShowWindow($h, 3) | Out-Null; [W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 400
$fp = 0; [W]::GetWindowThreadProcessId([W]::GetForegroundWindow(), [ref]$fp) | Out-Null
if ($fp -ne $GodotPid) { "Godot not in front (fg pid $fp); no click"; exit 1 }
[W]::SetCursorPos($X, $Y) | Out-Null; Start-Sleep -Milliseconds 150
[W]::mouse_event(2,0,0,0,0); Start-Sleep -Milliseconds 60; [W]::mouse_event(4,0,0,0,0)
"clicked"
