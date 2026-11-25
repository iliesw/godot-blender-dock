param([int]$ProcessId, [string]$Out, [int]$Width = 0, [int]$Height = 0)
# Capture one process's main window (as shown on screen, including owned popups such as the
# docked Blender). Optionally resizes the window first. Used for README / store screenshots.
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class CW {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool r);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int s);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  // Largest visible top-level window of a process (MainWindowHandle can be a tooltip).
  public static IntPtr Largest(uint pid) {
    IntPtr best = IntPtr.Zero; long area = 0;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid && IsWindowVisible(h)) { RECT r; GetWindowRect(h, out r);
        long a = (long)(r.R - r.L) * (r.B - r.T); if (a > area) { area = a; best = h; } }
      return true; }, IntPtr.Zero);
    return best;
  }
}
'@
[CW]::SetProcessDPIAware() | Out-Null
$h = [CW]::Largest([uint32]$ProcessId)
if ($Width -lt 0) {
    [CW]::ShowWindow($h, 3) | Out-Null # maximize
    Start-Sleep -Milliseconds 1500
} elseif ($Width -gt 0) {
    [CW]::ShowWindow($h, 9) | Out-Null # restore
    [CW]::MoveWindow($h, 40, 40, $Width, $Height, $true) | Out-Null
    Start-Sleep -Milliseconds 1500
}
[CW]::SetForegroundWindow($h) | Out-Null
$r = New-Object CW+RECT
[CW]::DwmGetWindowAttribute($h, 9, [ref]$r, 16) | Out-Null # DWMWA_EXTENDED_FRAME_BOUNDS
[CW]::SetCursorPos([int](($r.L + $r.R) / 2), $r.T + 12) | Out-Null # park the mouse on the title bar (no tooltips)
Start-Sleep -Milliseconds 1500
$w = $r.R - $r.L; $hh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"$Out ${w}x${hh}"
