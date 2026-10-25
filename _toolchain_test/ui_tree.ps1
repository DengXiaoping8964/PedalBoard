Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty,
            (Get-Process -Name PedalBoard | Select-Object -First 1).Id)

$windows = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
"windows: $($windows.Count)"

foreach ($w in $windows) {
    $r = $w.Current.BoundingRectangle
    "  '$($w.Current.Name)' $($r.Left),$($r.Top) $($r.Width)x$($r.Height)"
    $all = $w.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                      [System.Windows.Automation.Condition]::TrueCondition)
    "  descendants: $($all.Count)"
    $i = 0
    foreach ($e in $all) {
        if ($i -ge 60) { break }
        $b = $e.Current.BoundingRectangle
        "    {0,-12} [{1}] {2},{3} {4}x{5}  '{6}'" -f $e.Current.ControlType.ProgrammaticName,
            $e.Current.ClassName, $b.Left, $b.Top, $b.Width, $b.Height, $e.Current.Name
        ++$i
    }
}
