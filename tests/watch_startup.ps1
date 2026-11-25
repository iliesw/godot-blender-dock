# Launch the sandbox editor, sample Blender's window visibility every ~30 ms, then close Godot normally.
param([string]$Godot, [string]$Project, [string]$Scene)
. "$PSScriptRoot\winrect_lib.ps1"
$env:BLENDER_DOCK_AUTOLAUNCH = '1'; $env:BLENDER_DOCK_PORT = '6790'
Remove-Item Env:BLENDER_DOCK_PERFTEST, Env:BLENDER_DOCK_SELFTEST -ErrorAction SilentlyContinue
$g = Start-Process -FilePath $Godot -ArgumentList '--editor', '--path', $Project, $Scene -PassThru
$t0 = Get-Date; $bpid = 0; $last = ''
while (((Get-Date) - $t0).TotalSeconds -lt 25) {
  if (-not $bpid) { $b = Get-CimInstance Win32_Process -Filter "Name='blender.exe'" | Where-Object { $_.CommandLine -match 'godot-port 6790' }; if ($b) { $bpid = $b.ProcessId } }
  if ($bpid) {
    $state = ([R]::List([uint32]$bpid) | Where-Object { $_ -like 'GHOST*' }) -join ' | '
    if ($state -ne $last) { '{0,6:N2}s  {1}' -f ((Get-Date) - $t0).TotalSeconds, $state; $last = $state }
  }
  Start-Sleep -Milliseconds 30
}
"--- closing Godot normally"
$g.CloseMainWindow() | Out-Null
$t1 = Get-Date
while (((Get-Date) - $t1).TotalSeconds -lt 15 -and (Get-Process -Id $bpid -ErrorAction SilentlyContinue)) { Start-Sleep -Milliseconds 200 }
'Godot exited: {0}   Blender exited: {1} after {2:N1}s' -f ($g.HasExited), (-not (Get-Process -Id $bpid -ErrorAction SilentlyContinue)), ((Get-Date) - $t1).TotalSeconds
