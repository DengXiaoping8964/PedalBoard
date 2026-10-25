$ErrorActionPreference = 'SilentlyContinue'
'=== audio endpoints (Get-CimInstance Win32_SoundDevice) ==='
Get-CimInstance Win32_SoundDevice | ForEach-Object { '{0}  |  status={1}' -f $_.Name, $_.Status }

''
'=== PnP audio class devices ==='
Get-CimInstance Win32_PnPEntity -Filter "Class='MEDIA'" | ForEach-Object { $_.Name }

''
'=== MMDevices via registry (WAVE) ==='
$wave = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio'
if (Test-Path $wave) { (Get-ItemProperty $wave).DisabledAudioEndpoints }

''
'=== render/capture devices as Windows sees them ==='
Add-Type -TypeDefinition '
using System;
using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential)]
public struct MM_DEVICE { public uint count; }
' 2>&1 | Out-Null

'MMBlob devices:'
Get-ChildItem 'HKCU:\Software\Microsoft\Multimedia\Audio' 2>&1 | Select-Object -ExpandProperty PSChildName

''
'=== default endpoint markers ==='
Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render' 2>&1 | ForEach-Object { $_.PSChildName }
'render subkeys:'
Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render' 2>&1 | ForEach-Object { $_.PSChildName }
'capture subkeys:'
Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture' 2>&1 | ForEach-Object { $_.PSChildName }
