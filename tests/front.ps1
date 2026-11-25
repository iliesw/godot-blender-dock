param([int]$TargetPid)
# Bring a test window to the front without clicking anything.
Add-Type @'
using System; using System.Runtime.InteropServices;
public class Fr {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, int f, int e);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
}
'@
$h = (Get-Process -Id $TargetPid).MainWindowHandle
[Fr]::keybd_event(0x12,0,0,0); [Fr]::keybd_event(0x12,0,2,0)
[Fr]::ShowWindow($h, 3) | Out-Null; [Fr]::SetForegroundWindow($h) | Out-Null
