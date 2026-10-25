# Launches the app with the current settings file, captures the window twice, and
# leaves both images for comparison. The point is the drop counters in the status
# bar: a number that only moves during start-up is a device-ramp artefact, a number
# that keeps climbing is a real-time budget problem, and the two look identical in a
# single screenshot.
param(
    [int]$FirstDelaySeconds = 10,
    [int]$GapSeconds = 20,
    [string]$Tag = "drops"
)

$dir = 'D:\PedalBoard\build\PedalBoard_artefacts\RelWithDebInfo'
$exe = Join-Path $dir 'PedalBoard.exe'
$shot = 'D:\PedalBoard\_toolchain_test\screenshot.ps1'

Start-Process -FilePath $exe -WorkingDirectory $dir | Out-Null
Start-Sleep -Seconds $FirstDelaySeconds

powershell.exe -NoProfile -ExecutionPolicy Bypass -File $shot | Out-Null
Copy-Item 'D:\Temp\pb_shot.png' "D:\Temp\pb_$Tag`_t1.png" -Force

Start-Sleep -Seconds $GapSeconds

$p = Get-Process -Name PedalBoard -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -eq $p) { Write-Output 'PROCESS EXITED between samples'; exit 1 }
$p.Refresh()
Write-Output ('alive: pid={0} threads={1} wsMB={2:N1} cpuS={3:N1}' -f `
    $p.Id, $p.Threads.Count, ($p.WorkingSet64 / 1MB), $p.CPU)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File $shot | Out-Null
Copy-Item 'D:\Temp\pb_shot.png' "D:\Temp\pb_$Tag`_t2.png" -Force

Write-Output "captured D:\Temp\pb_${Tag}_t1.png and _t2.png (gap ${GapSeconds}s)"
