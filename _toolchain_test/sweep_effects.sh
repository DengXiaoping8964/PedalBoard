#!/usr/bin/env bash
# Sweeps the chain through every effect at one parameter extreme, launching the real
# app each time and checking it survived.
#
# Why a settings file rather than the add-menu: the chain holds 16 slots and there are
# 22 effects, so a full sweep needs overlapping windows anyway, and driving it through
# a file exercises the load path (parse, clamp, prepare, publish) that the menu does
# not. The menu path is tested separately.
#
# usage:  bash _toolchain_test/sweep_effects.sh [max|min|default]
set -u

MODE="${1:-max}"
ROOT="D:/PedalBoard"
DATA="$ROOT/build/PedalBoard_artefacts/RelWithDebInfo/PedalBoardData"
SETTINGS="$DATA/PedalBoard.settings"
EXE='D:\PedalBoard\build\PedalBoard_artefacts\RelWithDebInfo\PedalBoard.exe'

if [[ ! -f "$SETTINGS" ]]; then
  echo "no settings file at $SETTINGS - launch the app once first"
  exit 1
fi

cp "$SETTINGS" "$DATA/settings.sweep.bak"

launch_and_check() {
  local tag="$1"
  local shot="/d/Temp/pb_sweep_${tag}.png"

  powershell.exe -NoProfile -Command "
    \$p = Start-Process -FilePath '$EXE' -WorkingDirectory 'D:\PedalBoard\build\PedalBoard_artefacts\RelWithDebInfo' -PassThru
    Start-Sleep -Seconds 12
    if (\$p.HasExited) { Write-Output 'RESULT=EXITED code=' + \$p.ExitCode }
    else {
      \$p.Refresh()
      Write-Output ('RESULT=ALIVE pid=' + \$p.Id + ' threads=' + \$p.Threads.Count + ' wsMB=' + [math]::Round(\$p.WorkingSet64/1MB,1) + ' cpuS=' + [math]::Round(\$p.CPU,1))
    }
  " 2>&1 | grep -E "RESULT="

  # Captured while the process is still up: the screenshot is what shows whether a
  # card rendered, not just whether the process survived.
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$ROOT/_toolchain_test/screenshot.ps1" 2>&1 | tail -2

  mv /d/Temp/pb_shot.png "$shot" 2>/dev/null && echo "  screenshot: $shot"

  powershell.exe -NoProfile -Command "
    Stop-Process -Name PedalBoard -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    \$err = Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='Application Error'; StartTime=(Get-Date).AddSeconds(-45)} -ErrorAction SilentlyContinue
    if (\$err) { foreach (\$e in \$err) { \$m = \$e.Message -replace \"\`r\`n\", ' | '; Write-Output ('WER: ' + \$m.Substring(0, [Math]::Min(200, \$m.Length))) } }
    else { Write-Output 'WER=none' }
  " 2>&1 | grep -E "WER"
}

for start in 0 6; do
  out="/d/Temp/pb_sweep_${MODE}_${start}"
  node "$ROOT/_toolchain_test/make_chain_settings.mjs" --mode "$MODE" --start "$start" --count 16 --out "$out" > /dev/null \
    || { echo "generator failed for start=$start"; exit 1; }

  cp "$out" "$SETTINGS"
  echo "--- mode=$MODE chain=$start..$((start + 15))"
  launch_and_check "$MODE-$start"
done

cp "$DATA/settings.sweep.bak" "$SETTINGS"
rm -f "$DATA/settings.sweep.bak"
echo "restored the original settings"
