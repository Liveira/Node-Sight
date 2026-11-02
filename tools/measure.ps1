<#
.SYNOPSIS
Measure Node-Sight and only its direct SSH child processes.
.DESCRIPTION
Samples memory once per second and measures CPU time across all logical cores.
Reports omit IPs, hostnames, key paths, passwords and PIDs. Working-set sums may
count shared pages twice; they do not represent unique physical memory.
.PARAMETER Seconds
Measured interval after the caller's warm-up; defaults to sixty seconds.
.PARAMETER AppProcessId
Explicit widget PID. Otherwise exactly one NodeSight process must be running.
.PARAMETER Label
Non-sensitive scenario name included in the report.
#>
param(
    [ValidateRange(5, 600)][int]$Seconds = 60,
    [int]$AppProcessId = 0,
    [string]$Label = 'default',
    [string]$OutputPath
)
$ErrorActionPreference = 'Stop'
if ($AppProcessId) { $app = Get-Process -Id $AppProcessId }
else {
    $candidates = @(Get-Process NodeSight -ErrorAction Stop)
    if ($candidates.Count -ne 1) { throw 'Specify -AppProcessId if multiple widget/diagnostic processes exist.' }
    $app = $candidates[0]
}
if ($app.ProcessName -ne 'NodeSight') { throw 'The selected process is not NodeSight.' }
# Freeze membership so a lost connection cannot silently lower the reported cost.
$children = @(Get-CimInstance Win32_Process -Filter "Name = 'ssh.exe'" | Where-Object ParentProcessId -eq $app.Id)
$ids = @($app.Id) + @($children | ForEach-Object ProcessId)
$baseline = @{}
$samples = @{}
foreach ($processIdValue in $ids) {
    $process = Get-Process -Id $processIdValue
    $baseline[$processIdValue] = $process.CPU
    $samples[$processIdValue] = [System.Collections.Generic.List[object]]::new()
}
$cores = [Environment]::ProcessorCount
$clock = [Diagnostics.Stopwatch]::StartNew()
# Sleep between read-only samples; the widget's normal event loop keeps running.
for ($i = 0; $i -lt $Seconds; $i++) {
    Start-Sleep -Seconds 1
    foreach ($processIdValue in $ids) {
        $process = Get-Process -Id $processIdValue -ErrorAction Stop
        $samples[$processIdValue].Add([pscustomobject]@{
            WorkingSetMiB = $process.WorkingSet64 / 1MB
            PrivateMiB = $process.PrivateMemorySize64 / 1MB
        })
    }
}
$clock.Stop()
# Record only non-sensitive preferences, never serialize the full connection INI.
$preferences = [ordered]@{}
$settingsPath = Join-Path $env:LOCALAPPDATA 'NodeSight\settings.ini'
if (Test-Path -LiteralPath $settingsPath) {
    foreach ($line in [IO.File]::ReadAllLines($settingsPath)) {
        if ($line -match '^(interval|size|local|opacity|pinned)=(\d+)$') {
            $preferences[$Matches[1]] = [int]$Matches[2]
        }
    }
}
# CPU uses cumulative execution-time deltas normalized to total machine capacity.
$rows = foreach ($processIdValue in $ids) {
    $process = Get-Process -Id $processIdValue
    $points = $samples[$processIdValue]
    [pscustomobject]@{
        Process = $process.ProcessName
        AverageWorkingSetMiB = [Math]::Round(($points.WorkingSetMiB | Measure-Object -Average).Average, 3)
        PeakWorkingSetMiB = [Math]::Round(($points.WorkingSetMiB | Measure-Object -Maximum).Maximum, 3)
        AveragePrivateMiB = [Math]::Round(($points.PrivateMiB | Measure-Object -Average).Average, 3)
        CPUPercentTotalMachine = [Math]::Round(100 * ($process.CPU - $baseline[$processIdValue]) / $clock.Elapsed.TotalSeconds / $cores, 5)
    }
}
$report = [pscustomobject]@{
    Scenario = $Label
    CapturedAtUtc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    MeasuredSeconds = [Math]::Round($clock.Elapsed.TotalSeconds, 3)
    SampleCount = $Seconds
    LogicalProcessors = $cores
    SSHProcessCount = $children.Count
    Preferences = $preferences
    Processes = @($rows)
    CombinedAverageWorkingSetMiB = [Math]::Round(($rows.AverageWorkingSetMiB | Measure-Object -Sum).Sum, 3)
    CombinedAveragePrivateMiB = [Math]::Round(($rows.AveragePrivateMiB | Measure-Object -Sum).Sum, 3)
    CombinedCPUPercentTotalMachine = [Math]::Round(($rows.CPUPercentTotalMachine | Measure-Object -Sum).Sum, 5)
}
$json = $report | ConvertTo-Json -Depth 6
if ($OutputPath) { [IO.File]::WriteAllText($OutputPath, $json + [Environment]::NewLine) }
Write-Output $json
