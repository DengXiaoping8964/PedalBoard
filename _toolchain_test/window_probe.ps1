Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Collections.Generic;

public static class WinEnum
{
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int  GetWindowTextW(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int  GetWindowTextLengthW(IntPtr hWnd);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] static extern int  GetWindowLongW(IntPtr hWnd, int idx);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hWnd, out RECT r);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    public static List<string> ForProcess(uint target)
    {
        var found = new List<string>();
        EnumWindows((h, l) =>
        {
            uint pid;
            GetWindowThreadProcessId(h, out pid);
            if (pid != target) return true;

            var sb = new StringBuilder(GetWindowTextLengthW(h) + 1);
            GetWindowTextW(h, sb, sb.Capacity);
            var vis = IsWindowVisible(h);
            var style = GetWindowLongW(h, -16);        // GWL_STYLE
            var ex = GetWindowLongW(h, -20);           // GWL_EXSTYLE
            RECT r;
            GetWindowRect(h, out r);
            found.Add(String.Format("hwnd=0x{0:X} vis={1} title=[{2}] style=0x{3:X} exstyle=0x{4:X} rect=({5},{6})-({7},{8}) {9}x{10}",
                (ulong)h.ToInt64(), vis, sb.ToString(), (uint)style, (uint)ex,
                r.Left, r.Top, r.Right, r.Bottom, r.Right - r.Left, r.Bottom - r.Top));
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

foreach ($proc in (Get-Process -Name PedalBoard -ErrorAction SilentlyContinue))
{
    "PID " + $proc.Id + "  threads=" + $proc.Threads.Count + "  WS_MB=" + [math]::Round($proc.WorkingSet64/1MB,1)
    $wins = [WinEnum]::ForProcess([uint32]$proc.Id)
    if ($wins.Count -eq 0) { "  (no top-level windows at all)" }
    foreach ($w in $wins) { "  " + $w }
}
