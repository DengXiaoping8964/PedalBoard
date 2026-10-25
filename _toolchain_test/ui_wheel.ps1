param([int]$X = 700, [int]$Y = 450, [int]$Ticks = -10)

$sig = @'
using System;
using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, int d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p);
  [StructLayout(LayoutKind.Sequential)] public struct P { public int X, Y; }
  public const uint WHEEL = 0x0800;
}
'@
Add-Type -TypeDefinition $sig

$old = New-Object W+P
[void][W]::GetCursorPos([ref]$old)

[void][W]::SetCursorPos($X, $Y)
Start-Sleep -Milliseconds 150
[W]::mouse_event([W]::WHEEL, 0, 0, (120 * $Ticks), [IntPtr]::Zero)
Start-Sleep -Milliseconds 400

[void][W]::SetCursorPos($old.X, $old.Y)
"wheel $Ticks at $X,$Y"
