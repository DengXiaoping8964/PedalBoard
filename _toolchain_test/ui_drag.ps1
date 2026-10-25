param([int]$X1 = 0, [int]$Y1 = 0, [int]$X2 = 0, [int]$Y2 = 0, [int]$Steps = 12)
# Press at X1,Y1, move in $Steps increments to X2,Y2, release. Same SetCursorPos +
# mouse_event approach as ui_click.ps1 (SendInput's absolute events are dropped on
# this machine), with the intermediate moves spaced out so the target sees them as a
# drag rather than a jump.

$sig = @'
using System;
using System.Runtime.InteropServices;
public static class M {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct P { public int X, Y; }
  public const uint DOWN = 0x0002, UP = 0x0004;
}
'@
Add-Type -TypeDefinition $sig

# Same reason as ui_click.ps1: the press has to be delivered to the app, not to
# whatever window the IDE left on top of those coordinates.
$p = Get-Process -Name PedalBoard -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -ne $p) {
    [void] [M]::SetForegroundWindow([IntPtr] $p.MainWindowHandle)
    Start-Sleep -Milliseconds 300
}

$old = New-Object M+P
[void] [M]::GetCursorPos([ref] $old)

[void] [M]::SetCursorPos($X1, $Y1)
Start-Sleep -Milliseconds 150
[M]::mouse_event([M]::DOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 120

for ($s = 1; $s -le $Steps; ++$s) {
    $t = $s / $Steps
    $x = [int] ($X1 + ($X2 - $X1) * $t)
    $y = [int] ($Y1 + ($Y2 - $Y1) * $t)
    [void] [M]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 25
}

Start-Sleep -Milliseconds 100
[M]::mouse_event([M]::UP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 80
[void] [M]::SetCursorPos($old.X, $old.Y)

Write-Output "dragged ($X1,$Y1) -> ($X2,$Y2)"
