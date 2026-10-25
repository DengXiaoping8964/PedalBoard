Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$sig = @'
using System;
using System.Runtime.InteropServices;
public static class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, IntPtr t, int max);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rr, B; }
}
'@
Add-Type -TypeDefinition $sig

$script:wantPid = 0
$script:found = [IntPtr]::Zero
$callback = [W+EnumProc] {
  param($h, $l)
  if (-not [W]::IsWindowVisible($h)) { return $true }
  $pid2 = 0
  [void][W]::GetWindowThreadProcessId($h, [ref]$pid2)
  if ($pid2 -ne $script:wantPid) { return $true }
  $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal(1024)
  try {
    $n = [W]::GetWindowTextW($h, $buf, 512)
    if ($n -gt 0) {
      $t = [Runtime.InteropServices.Marshal]::PtrToStringUni($buf)
      $r = New-Object W+R
      [void][W]::GetWindowRect($h, [ref]$r)
      # A minimised window reports a 160x28 rect at -32000, so it has to be
      # recognised by IsIconic rather than by its size.
      if ($t.StartsWith("PedalBoard") -and (-not [W]::IsIconic($h))) { $script:found = $h }
    }
  } finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
  return $true
}

$p = Get-Process -Name PedalBoard -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -eq $p) { Write-Output "NO PROCESS"; exit 1 }
$script:wantPid = [uint32]$p.Id

[void][W]::EnumWindows($callback, [IntPtr]::Zero)

if ($script:found -eq [IntPtr]::Zero) {
    # Nothing non-minimised: restore the first one and look again.
    $restore = [W+EnumProc] {
      param($h, $l)
      $pid2 = 0
      [void][W]::GetWindowThreadProcessId($h, [ref]$pid2)
      if ($pid2 -ne $script:wantPid) { return $true }
      $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal(1024)
      try {
        if ([W]::GetWindowTextW($h, $buf, 512) -gt 0) {
          if (([Runtime.InteropServices.Marshal]::PtrToStringUni($buf)).StartsWith("PedalBoard")) {
            [void][W]::ShowWindow($h, 9)   # SW_RESTORE
            Write-Output "restored a minimised window first"
          }
        }
      } finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($buf) }
      return $true
    }
    [void][W]::EnumWindows($restore, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 600
    [void][W]::EnumWindows($callback, [IntPtr]::Zero)
}

$h = $script:found
if ($h -eq [IntPtr]::Zero) { Write-Output "NO HWND for pid $($p.Id)"; exit 1 }

# Bring it forward so nothing is painted over the capture.
[void][W]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 500
$r = New-Object W+R
[void][W]::GetWindowRect($h, [ref]$r)
$w = $r.Rr - $r.L
$hh = $r.B - $r.T
Write-Output "hwnd=$h rect=$($r.L),$($r.T)-$($r.Rr),$($r.B) size=${w}x${hh}"

$bmp = New-Object System.Drawing.Bitmap($w, $hh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size($w, $hh)))
$out = "D:\Temp\pb_shot.png"
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

# how much of the shot is non-background (proof it painted something)
$bmp2 = New-Object System.Drawing.Bitmap($out)
$distinct = @{}
$rand = New-Object System.Random
for ($y = 0; $y -lt $bmp2.Height; $y += 7) {
  for ($x = 0; $x -lt $bmp2.Width; $x += 7) {
    $c = $bmp2.GetPixel($x, $y)
    $key = "{0},{1},{2}" -f $c.R, $c.G, $c.B
    if (-not $distinct.ContainsKey($key)) { $distinct[$key] = 0 }
    $distinct[$key] = $distinct[$key] + 1
  }
}
$bmp2.Dispose()
$samples = ($distinct.Values | Measure-Object -Sum).Sum
Write-Output ("distinct_colours={0} samples={1} top5={2}" -f $distinct.Count, $samples,
  (($distinct.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 5 | ForEach-Object { "$($_.Key):$($_.Value)" }) -join ' '))
Write-Output "saved=$out"
