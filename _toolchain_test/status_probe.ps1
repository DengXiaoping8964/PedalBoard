$p = Get-Process -Name PedalBoard -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -eq $p) {
  Write-Output "PROCESS: not running"
} else {
  Write-Output ("PROCESS: pid={0} threads={1} wsMB={2:N1} cpuS={3:N1} start={4:HH:mm:ss}" -f `
    $p.Id, $p.Threads.Count, ($p.WorkingSet64 / 1MB), $p.CPU, $p.StartTime)
}

$since = (Get-Date).AddMinutes(-20)
foreach ($log in @('Application Error', '.NET Runtime', 'Windows Error Reporting')) {
  $ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = $log; StartTime = $since } -ErrorAction SilentlyContinue
  if ($null -ne $ev) {
    foreach ($e in $ev | Select-Object -First 3) {
      $m = $e.Message -replace "`r`n", ' | '
      Write-Output ("EVENT[{0}] {1:HH:mm:ss} {2}" -f $log, $e.TimeCreated, $m.Substring(0, [Math]::Min(240, $m.Length)))
    }
  }
}
Write-Output "event scan done"
