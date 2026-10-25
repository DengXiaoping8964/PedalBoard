$sig = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder t, int max);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rr, B; }
}
'@
Add-Type -TypeDefinition $sig

$matches2 = @()
$cb = [W+EnumProc] {
  param($h, $l)
  $sb = New-Object System.Text.StringBuilder 512
  [void][W]::GetWindowTextW($h, $sb, 512)
  $t = $sb.ToString()
  if ($t -like "*PedalBoard*") {
    $pid2 = 0
    [void][W]::GetWindowThreadProcessId($h, [ref]$pid2)
    $r = New-Object W+R
    [void][W]::GetWindowRect($h, [ref]$r)
    $script:matches2 += [pscustomobject]@{
      hwnd = $h; pid = $pid2; vis = [W]::IsWindowVisible($h); min = [W]::IsIconic($h)
      rect = "$($r.L),$($r.T)-$($r.Rr),$($r.B)"; title = $t.Substring(0, [Math]::Min(60, $t.Length))
    }
  }
  return $true
}
[void][W]::EnumWindows($cb, [IntPtr]::Zero)

Write-Output "--- processes named PedalBoard ---"
Get-Process -Name PedalBoard -ErrorAction SilentlyContinue | ForEach-Object {
  Write-Output ("  pid={0} threads={1} wsMB={2:N1} cpuS={3:N1} start={4:HH:mm:ss} responding={5}" -f `
    $_.Id, $_.Threads.Count, ($_.WorkingSet64/1MB), $_.CPU, $_.StartTime, $_.Responding)
}

Write-Output "--- every window whose title mentions PedalBoard ---"
if ($matches2.Count -eq 0) { Write-Output "  (none)" }
$matches2 | ForEach-Object { Write-Output ("  hwnd={0} pid={1} vis={2} min={3} rect={4} title=[{5}]" -f `
  $_.hwnd, $_.pid, $_.vis, $_.min, $_.rect, $_.title) }
