$sig = @'
using System;
using System.Runtime.InteropServices;
public static class T {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p);
  [StructLayout(LayoutKind.Sequential)] public struct P { public int X, Y; }
  [DllImport("user32.dll")] public static extern IntPtr GetProcessWindowStation();
  [DllImport("user32.dll")] public static extern IntPtr GetThreadDesktop(uint id);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
}
'@
Add-Type -TypeDefinition $sig

$p = New-Object T+P
[void] [T]::GetCursorPos([ref] $p)
Write-Output ("before: {0},{1}  station={2} desktop={3} thread={4}" -f `
    $p.X, $p.Y, [T]::GetProcessWindowStation(), [T]::GetThreadDesktop([T]::GetCurrentThreadId()), [T]::GetCurrentThreadId())

[void] [T]::SetCursorPos(665, 712)
Start-Sleep -Milliseconds 200
$q = New-Object T+P
[void] [T]::GetCursorPos([ref] $q)
Write-Output ("after SetCursorPos(665,712): {0},{1}" -f $q.X, $q.Y)

[void] [T]::SetCursorPos($p.X, $p.Y)
