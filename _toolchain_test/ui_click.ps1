param([int]$X = 0, [int]$Y = 0, [switch]$NoFocus)
# Clicks at screen coordinates X,Y, then puts the pointer back.
#
# This uses SetCursorPos + mouse_event rather than SendInput with
# MOUSEEVENTF_ABSOLUTE: measured on this machine, SendInput reported the events as
# accepted yet the cursor never moved (a remote-desktop agent is running here and
# appears to filter injected absolute input), while SetCursorPos moves it exactly.
# The pointer therefore visibly jumps for a moment; the old position is restored
# before the script exits.

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

# Input goes to whatever window is under the cursor, so the app has to be the
# foreground window first: a click delivered to the IDE that happens to sit over
# the same pixels looks exactly like a click the app ignored.
#
# -NoFocus skips that step. It is required for clicks inside an open combo popup:
# focusing the main window first makes JUCE dismiss the popup, so the click lands
# on whatever card was underneath it instead of on the menu item.
$p = Get-Process -Name PedalBoard -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -ne $p -and -not $NoFocus) {
    [void] [M]::SetForegroundWindow([IntPtr] $p.MainWindowHandle)
    Start-Sleep -Milliseconds 300
}

$old = New-Object M+P
[void] [M]::GetCursorPos([ref] $old)

[void] [M]::SetCursorPos($X, $Y)
Start-Sleep -Milliseconds 120
[M]::mouse_event([M]::DOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 80
[M]::mouse_event([M]::UP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 80
[void] [M]::SetCursorPos($old.X, $old.Y)

Write-Output "clicked screen $X,$Y"
