param(
    [int]$X1 = 400, [int]$Y1 = 195,
    [int]$X2 = 400, [int]$Y2 = 240,
    [string]$Out = "D:\Temp\pb_drag_mid.png",
    [int]$Steps = 8
)
# Press on a card header, move part of the way, capture the window while the
# button is still down, then move back and release without changing the chain.
# The mid-drag frame is the only way to see the dragged-card dimming: it is gone
# again by the time any normal capture runs.

$sig = @'
using System;
using System.Runtime.InteropServices;
public static class M {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p);
  [StructLayout(LayoutKind.Sequential)] public struct P { public int X, Y; }
  public const uint DOWN = 0x0002, UP = 0x0004;
}
'@
Add-Type -TypeDefinition $sig
Add-Type -AssemblyName System.Drawing

$old = New-Object M+P
[void] [M]::GetCursorPos([ref] $old)

[void] [M]::SetCursorPos($X1, $Y1)
Start-Sleep -Milliseconds 150
[M]::mouse_event([M]::DOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 150

for ($s = 1; $s -le $Steps; ++$s) {
    $t = $s / $Steps
    [void] [M]::SetCursorPos([int] ($X1 + ($X2 - $X1) * $t), [int] ($Y1 + ($Y2 - $Y1) * $t))
    Start-Sleep -Milliseconds 30
}

Start-Sleep -Milliseconds 300

# Locate the PedalBoard window by title and copy it while the drag is live.
$sig2 = @'
using System;
using System.Runtime.InteropServices;
public static class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, IntPtr t, int max);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rr, B; }
}
'@
Add-Type -TypeDefinition $sig2

$p = Get-Process -Name PedalBoard -ErrorAction SilentlyContinue | Select-Object -First 1
$script:wantPid = [uint32]$p.Id
$script:found = [IntPtr]::Zero
$cb = [W+EnumProc] {
  param($h, $l)
  if (-not [W]::IsWindowVisible($h)) { return $true }
  $pid2 = 0
  [void][W]::GetWindowThreadProcessId($h, [ref]$pid2)
  if ($pid2 -ne $script:wantPid) { return $true }
  $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal(1024)
  try {
    if ([W]::GetWindowTextW($h, $buf, 512) -gt 0) {
      if (([Runtime.InteropServices.Marshal]::PtrToStringUni($buf)).StartsWith("PedalBoard")) { $script:found = $h }
    }
  } finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
  return $true
}
[void][W]::EnumWindows($cb, [IntPtr]::Zero)

if ($script:found -eq [IntPtr]::Zero) { Write-Output "NO WINDOW"; exit 1 }

$r = New-Object W+R
[void][W]::GetWindowRect($script:found, [ref]$r)
$w = $r.Rr - $r.L
$h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size($w, $h)))
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

# Back to where the gesture started, then release: a drop on the original gap
# leaves the chain untouched.
[void] [M]::SetCursorPos($X1, $Y1)
Start-Sleep -Milliseconds 150
[M]::mouse_event([M]::UP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 100
[void] [M]::SetCursorPos($old.X, $old.Y)

Write-Output "saved=$Out"
