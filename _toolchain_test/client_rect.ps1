Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class CW
{
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
'@

"screen: " + [CW]::GetSystemMetrics(0) + "x" + [CW]::GetSystemMetrics(1)
"work area: " + [CW]::GetSystemMetrics(16) + "x" + [CW]::GetSystemMetrics(17)

$target = (Get-Process -Name PedalBoard | Select-Object -First 1).Id
$script:hwnd = [IntPtr]::Zero
[CW]::EnumWindows({ param($h,$l)
    $p = 0
    [void][CW]::GetWindowThreadProcessId($h, [ref]$p)
    if ($p -eq $target -and [CW]::IsWindowVisible($h)) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][CW]::GetWindowTextW($h, $sb, 256)
        if ($sb.ToString().StartsWith('PedalBoard')) { $script:hwnd = $h }
    }
    $true
}, [IntPtr]::Zero) | Out-Null

$r = New-Object CW+RECT
$c = New-Object CW+RECT
$p = New-Object CW+POINT
[CW]::GetWindowRect($script:hwnd, [ref]$r) | Out-Null
[CW]::GetClientRect($script:hwnd, [ref]$c) | Out-Null
[CW]::ClientToScreen($script:hwnd, [ref]$p) | Out-Null
"window rect  : ({0},{1})-({2},{3}) {4}x{5}" -f $r.L,$r.T,$r.R,$r.B,($r.R-$r.L),($r.B-$r.T)
"client size  : {0}x{1} at screen ({2},{3})" -f $c.R,$c.B,$p.X,$p.Y
"zoomed       : " + [CW]::IsZoomed($script:hwnd)
