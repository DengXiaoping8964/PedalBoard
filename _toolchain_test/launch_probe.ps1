$ErrorActionPreference = 'Stop'
$exe = 'D:\PedalBoard\build\PedalBoard_artefacts\Release\PedalBoard.exe'
$dataDir = 'D:\PedalBoard\build\PedalBoard_artefacts\Release\PedalBoardData'

if (-not (Test-Path $exe)) { 'MISSING EXE'; exit 1 }

$p = Start-Process -FilePath $exe -PassThru
'Started pid = ' + $p.Id

for ($i = 1; $i -le 10; $i++)
{
    Start-Sleep -Seconds 2
    $p.Refresh()

    if ($p.HasExited)
    {
        'EXITED EARLY at ~' + ($i * 2) + 's, exit code = ' + $p.ExitCode
        exit 2
    }

    if (-not [string]::IsNullOrWhiteSpace($p.MainWindowTitle))
    {
        'WINDOW at ~' + ($i * 2) + 's'
        '  title      = ' + $p.MainWindowTitle
        '  handle     = ' + $p.MainWindowHandle
        '  WS(MB)     = ' + [math]::Round($p.WorkingSet64 / 1MB, 1)
        '  threads    = ' + $p.Threads.Count
        break
    }
}

$p.Refresh()
if ([string]::IsNullOrWhiteSpace($p.MainWindowTitle))
{
    'NO WINDOW after 20s (process still alive = ' + (-not $p.HasExited) + ')'
}

''
'--- alive check: watch 15 more seconds for a late crash ---'
$before = $p.HasExited
Start-Sleep -Seconds 15
$p.Refresh()
'exited during the extra 15s? ' + $p.HasExited
'WS(MB) now = ' + [math]::Round($p.WorkingSet64 / 1MB, 1)

''
'--- files the app created on D: ---'
if (Test-Path $dataDir) { Get-ChildItem $dataDir | ForEach-Object { $_.Name + '  ' + $_.Length + ' bytes' } }
else { 'no PedalBoardData directory yet' }

''
'--- IRs copied next to the exe? ---'
$irDir = 'D:\PedalBoard\build\PedalBoard_artefacts\Release\IRs'
if (Test-Path $irDir) { (Get-ChildItem $irDir).Name } else { 'no IRs folder' }

'PID for the caller to reuse: ' + $p.Id
