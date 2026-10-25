Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class WinStation
{
    [DllImport("user32")]
    public static extern IntPtr GetProcessWindowStation();

    [DllImport("user32", CharSet=CharSet.Unicode)]
    public static extern bool GetUserObjectInformationW(IntPtr hObj, int nIndex,
                                                        IntPtr pvInfo, uint nLength,
                                                        out uint lpnLengthNeeded);

    // UOI_NAME == 2
    public static string CurrentName()
    {
        IntPtr h = GetProcessWindowStation();
        uint need;
        GetUserObjectInformationW(h, 2, IntPtr.Zero, 0, out need);
        if (need == 0) return "<unknown>";
        IntPtr buf = Marshal.AllocHGlobal((int) need);
        GetUserObjectInformationW(h, 2, buf, need, out need);
        string s = Marshal.PtrToStringUni(buf);
        Marshal.FreeHGlobal(buf);
        return s;
    }
}
'@

'window station of this shell = [' + [WinStation]::CurrentName() + ']'
'console session id           = ' + (Get-Process -Id $PID).SessionId
