param([int]$X = 0, [int]$Y = 0)
Add-Type -AssemblyName System.Windows.Forms

# Same click as ui_click.ps1, but reporting whether SendInput actually accepted the
# events: a click that silently fails looks exactly like a control that does nothing.
$sig = @'
using System;
using System.Runtime.InteropServices;
public static class M {
  [StructLayout(LayoutKind.Sequential)]
  public struct INPUT { public uint type; public MOUSEINPUT mi; }
  [StructLayout(LayoutKind.Sequential)]
  public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData;
                             public uint dwFlags; public uint time; IntPtr dwExtra; }
  [DllImport("user32.dll", SetLastError = true)]
  public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern int GetLastError();
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern IntPtr GetDesktopWindow();
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p);
  [StructLayout(LayoutKind.Sequential)] public struct P { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public const uint ABSOLUTE = 32768, VIRTUALDESK = 4096, MOVE = 1, DOWN = 2, UP = 4;
}
'@
Add-Type -TypeDefinition $sig

$vs   = [System.Windows.Forms.SystemInformation]::VirtualScreen
$size = [Runtime.InteropServices.Marshal]::SizeOf([type]"M+INPUT")

$hit = New-Object M+POINT
$hit.X = $X
$hit.Y = $Y
$hwnd = [M]::WindowFromPoint($hit)
$hitPid = 0
[void] [M]::GetWindowThreadProcessId($hwnd, [ref] $hitPid)

$old = New-Object M+P
[void] [M]::GetCursorPos([ref] $old)

function Send-Drag([int]$x, [int]$y, [uint32]$flags) {
    $i = New-Object M+INPUT
    $i.type = 0
    $i.mi.dx = [int] (65536.0 * ($x - $vs.X) / $vs.Width)
    $i.mi.dy = [int] (65536.0 * ($y - $vs.Y) / $vs.Height)
    $i.mi.dwFlags = $flags
    $n = [M]::SendInput(1, [M+INPUT[]] @($i), $size)
    if ($n -eq 0) { Write-Output ("  SendInput failed, err={0}" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error()) }
}

Write-Output ("point {0},{1} -> hwnd={2} pid={3} (pedalboard pid={4})" -f `
    $X, $Y, $hwnd, $hitPid, (Get-Process -Name PedalBoard -ErrorAction SilentlyContinue | Select-Object -First 1).Id)

Send-Drag $X $Y ([M]::ABSOLUTE -bor [M]::VIRTUALDESK -bor [M]::MOVE)
Start-Sleep -Milliseconds 80
Send-Drag $X $Y ([M]::ABSOLUTE -bor [M]::VIRTUALDESK -bor [M]::DOWN)
Start-Sleep -Milliseconds 80
Send-Drag $X $Y ([M]::ABSOLUTE -bor [M]::VIRTUALDESK -bor [M]::UP)

Start-Sleep -Milliseconds 150
$cur = New-Object M+P
[void] [M]::GetCursorPos([ref] $cur)
[void] [M]::SetCursorPos($old.X, $old.Y)
Write-Output ("sent; cursor observed at {0},{1}" -f $cur.X, $cur.Y)
