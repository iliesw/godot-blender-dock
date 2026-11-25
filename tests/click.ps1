param([int]$X, [int]$Y)
Add-Type @'
using System; using System.Runtime.InteropServices;
public class M {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(int f, int x, int y, int d, int e);
}
'@
[M]::SetProcessDPIAware() | Out-Null
[M]::SetCursorPos($X, $Y) | Out-Null; Start-Sleep -Milliseconds 150
[M]::mouse_event(2,0,0,0,0); Start-Sleep -Milliseconds 60; [M]::mouse_event(4,0,0,0,0)
