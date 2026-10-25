$ErrorActionPreference = 'Stop'
$exe = 'D:\PedalBoard\build\PedalBoard_artefacts\RelWithDebInfo\PedalBoard.exe'

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class CrashResolver
{
    [DllImport("dbghelp.dll", SetLastError=true, CharSet=CharSet.Ansi)]
    static extern bool SymInitialize(IntPtr h, string f, bool inv);
    [DllImport("dbghelp.dll", SetLastError=true)]
    static extern uint SymSetOptions(uint o);
    [DllImport("dbghelp.dll", SetLastError=true, CharSet=CharSet.Ansi)]
    static extern ulong SymLoadModuleEx(IntPtr h, IntPtr file, string image, string name,
                                        ulong baseOfDll, int dllSize, IntPtr data, uint flags);

    [StructLayout(LayoutKind.Sequential)]
    struct SYMBOL_INFO
    {
        public uint SizeOfStruct;
        public uint TypeIndex;
        public ulong Reserved1;
        public ulong Reserved2;
        public uint Index;
        public uint Size;
        public ulong ModBase;
        public uint Flags;
        public ulong Value;
        public ulong Tag;
        public uint NameLen;
        public uint MaxNameLen;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=1024)]
        public string Name;
    }

    [DllImport("dbghelp.dll", SetLastError=true)]
    static extern bool SymFromAddr(IntPtr h, ulong addr, out ulong disp, ref SYMBOL_INFO si);

    [StructLayout(LayoutKind.Sequential)]
    struct IMAGEHLP_LINE64
    {
        public uint SizeOfStruct;
        public IntPtr Key;
        public uint LineNumber;
        [MarshalAs(UnmanagedType.LPStr)] public string FileName;
        public ulong Address;
    }

    [DllImport("dbghelp.dll", SetLastError=true)]
    static extern bool SymGetLineFromAddr64(IntPtr h, ulong addr, out uint disp, ref IMAGEHLP_LINE64 li);

    public static string Resolve(string modulePath, ulong rva)
    {
        var sb = new StringBuilder();
        IntPtr h = ProcessHandle();

        SymSetOptions(0x00000400);            // SYMOPT_UNDNAME
        if (! SymInitialize(h, null, false))
        {
            int e = Marshal.GetLastWin32Error();
            sb.AppendLine("SymInitialize failed, err " + e);
        }

        ulong baseAddr = SymLoadModuleEx(h, IntPtr.Zero, modulePath, null, 0, 0, IntPtr.Zero, 0);
        if (baseAddr == 0)
        {
            sb.AppendLine("SymLoadModuleEx failed, err " + Marshal.GetLastWin32Error());
            return sb.ToString();
        }

        ulong addr = baseAddr + rva;
        sb.AppendLine("module base = 0x" + baseAddr.ToString("X"));
        sb.AppendLine("fault addr  = 0x" + addr.ToString("X") + "   (rva 0x" + rva.ToString("X") + ")");

        var si = new SYMBOL_INFO();
        si.SizeOfStruct = (uint)(Marshal.SizeOf(typeof(SYMBOL_INFO)) - 1024);
        si.MaxNameLen = 1024;

        ulong disp;
        if (SymFromAddr(h, addr, out disp, ref si))
            sb.AppendLine("function    = " + si.Name + " + 0x" + disp.ToString("X"));
        else
            sb.AppendLine("function    = <unresolved, err " + Marshal.GetLastWin32Error() + ">");

        var li = new IMAGEHLP_LINE64();
        li.SizeOfStruct = (uint)Marshal.SizeOf(typeof(IMAGEHLP_LINE64));

        uint ldisp;
        if (SymGetLineFromAddr64(h, addr, out ldisp, ref li))
            sb.AppendLine("source line = " + li.FileName + ":" + li.LineNumber);
        else
            sb.AppendLine("source line = <unresolved, err " + Marshal.GetLastWin32Error() + ">");

        return sb.ToString();
    }

    static IntPtr ProcessHandle()
    {
        return System.Diagnostics.Process.GetCurrentProcess().Handle;
    }
}
'@

$t0 = Get-Date
$p = Start-Process -FilePath $exe -PassThru
if (-not $p.WaitForExit(25000)) { 'STILL RUNNING after 25 s - no crash'; exit 0 }

$code64 = ([int64]$p.ExitCode) -band 0xFFFFFFFF
'exit code    = {0}  (0x{1:X8})' -f $p.ExitCode, $code64
'alive for    = ' + [math]::Round(((Get-Date) - $t0).TotalSeconds, 1) + ' s'

Start-Sleep -Seconds 4

$ev = Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='Application Error';
                                      StartTime=$t0.AddSeconds(-5)} -ErrorAction SilentlyContinue |
      Where-Object { $_.Message -match 'PedalBoard' } | Select-Object -First 1

if ($null -eq $ev) { 'no Application Error event'; exit 1 }

if ($ev.Message -match '(?m)0x([0-9a-fA-F]{16})')
{
    $rva = [Convert]::ToUInt64($Matches[1], 16)
    'fault offset = 0x{0:X}' -f $rva
    ''
    [CrashResolver]::Resolve($exe, $rva)
}
else { 'could not read the fault offset' }
