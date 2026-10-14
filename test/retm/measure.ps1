param(
  [Parameter(Mandatory=$true)][string]$Baseline,
  [Parameter(Mandatory=$true)][string]$Candidate,
  [Parameter(Mandatory=$true)][string]$BaselineModule,
  [Parameter(Mandatory=$true)][string]$CandidateModule,
  [Parameter(Mandatory=$true)][string]$Output,
  [ValidateSet(15, 30, 60)][int]$Pairs = 15,
  [ValidateSet("ordinary", "fixed", "vararg", "bench2")]
  [string[]]$Cases = @("ordinary", "fixed", "vararg", "bench2"),
  [ValidateSet("default", "arenagc")][string]$Variant = "default",
  [int]$Iterations = 5000000,
  [long]$Affinity = 4,
  [int]$WarmupIterations = 1000000,
  [switch]$Linux,
  [string]$LinuxBench,
  [int]$LinuxCpu = 2,
  [int]$CooldownMs = 0,
  [ValidateRange(1, 1000)][int]$Blocks = 1
)
$ErrorActionPreference = "Stop"
if (@($Cases | Select-Object -Unique).Count -ne $Cases.Count) {
  throw "Cases must be unique."
}
$critical = @{15=2.1447866879; 30=2.0452296421; 60=2.0009953770}[$Pairs]
$rows = @()
$blockRows = @()
$summary = @()
function Start-Benchmark([string]$Which, [string]$Case, [int]$Count, [bool]$Worker) {
  $exe = if ($Which -eq "baseline") { $Baseline } else { $Candidate }
  $module = if ($Which -eq "baseline") { $BaselineModule } else { $CandidateModule }
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $workerArg = if ($Worker) { " worker" } else { "" }
  if ($Linux) {
    if (!$LinuxBench) { throw "LinuxBench must be an absolute WSL path." }
    $libraryDir = [IO.Path]::GetDirectoryName($exe)
    $info.FileName = "wsl.exe"
    $info.Arguments = "-d Ubuntu --exec env `"LUA_CPATH=$module/?.so;;`" " +
      "`"LD_LIBRARY_PATH=$libraryDir`" " +
      "taskset -c $LinuxCpu $exe `"$LinuxBench`" $Case $Count $WarmupIterations$workerArg"
  } else {
    $info.FileName = $exe
    $info.Arguments = "`"$PSScriptRoot/bench.lua`" $Case $Count $WarmupIterations$workerArg"
  }
  $info.UseShellExecute = $false
  $info.CreateNoWindow = $true
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $info.RedirectStandardInput = $Worker
  if (!$Linux) { $info.EnvironmentVariables["LUA_CPATH"] = "$module/?.dll;;" }
  $process = [Diagnostics.Process]::Start($info)
  if ($Linux -and $Worker) { $process.StandardInput.NewLine = "`n" }
  if (!$Linux) {
    $process.ProcessorAffinity = [IntPtr]$Affinity
    $process.PriorityClass = [Diagnostics.ProcessPriorityClass]::AboveNormal
  }
  return $process
}
function Read-Block($Process, [string]$Which, [string]$Case) {
  $result = $Process.StandardOutput.ReadLine()
  $elapsed = $Process.StandardOutput.ReadLine()
  if ($null -eq $elapsed -or $result -notmatch '^r=') {
    throw "$Which/$Case worker failed: $($Process.StandardError.ReadToEnd())"
  }
  $time = [double]::Parse($elapsed, [Globalization.CultureInfo]::InvariantCulture)
  if ($time -le 0) { throw "$Which/$Case elapsed time is zero; increase Iterations." }
  return @{
    result=$result
    time=$time
  }
}
foreach ($case in $Cases) {
  $count = if ($case -eq "bench2") { 20000000 } else { $Iterations }
  for ($pair = 1; $pair -le $Pairs; $pair++) {
    $times = @{}
    $order = if ($pair % 2) { @("baseline", "candidate") } else { @("candidate", "baseline") }
    if ($Blocks -gt 1) {
      $workers = @{}
      $times.baseline = $times.candidate = 0.0
      try {
        foreach ($which in $order) {
          $workers[$which] = Start-Benchmark $which $case $count $true
          if ($workers[$which].StandardOutput.ReadLine() -ne "ready") {
            throw "$which/$case warmup failed"
          }
        }
        for ($block = 1; $block -le $Blocks; $block++) {
          $blockTimes = @{}
          $blockOrder = if (($pair+$block) % 2) {
            @("candidate", "baseline")
          } else { @("baseline", "candidate") }
          foreach ($which in $blockOrder) {
            $workers[$which].StandardInput.WriteLine($count)
            $sample = Read-Block $workers[$which] $which $case
            $times[$which] += $sample.time
            $times["${which}Result"] = $sample.result
            $blockTimes[$which] = $sample.time
          }
          if ($times.baselineResult -ne $times.candidateResult) { throw "result mismatch" }
          $blockRows += [pscustomobject]@{
            case=$case; pair=$pair; block=$block; order=($blockOrder -join ",")
            iterations=$count; variant=$Variant
            baseline=$blockTimes.baseline; candidate=$blockTimes.candidate
            ratio=$blockTimes.candidate/$blockTimes.baseline
          }
          if ($CooldownMs) { Start-Sleep -Milliseconds $CooldownMs }
        }
      } finally {
        $failedExits = @()
        foreach ($process in $workers.Values) {
          if (!$process.HasExited) {
            $process.StandardInput.WriteLine("quit")
            if (!$process.WaitForExit(5000)) {
              $process.Kill()
              $process.WaitForExit()
            }
          }
          if ($process.ExitCode) {
            $failedExits += "exit $($process.ExitCode): $($process.StandardError.ReadToEnd())"
          }
          $process.Dispose()
        }
        if ($failedExits.Count) { throw ($failedExits -join "`n") }
      }
    } else {
      foreach ($which in $order) {
        $process = Start-Benchmark $which $case $count $false
        $text = $process.StandardOutput.ReadToEnd()
        $errors = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        if ($process.ExitCode) { throw "$which/$case exited $($process.ExitCode): $errors" }
        $result = @($text.Trim() -split "\r?\n")
        $times[$which] = [double]::Parse($result[-1], [Globalization.CultureInfo]::InvariantCulture)
        $times["${which}Result"] = $result[-2]
        $process.Dispose()
        if ($CooldownMs) { Start-Sleep -Milliseconds $CooldownMs }
      }
    }
    if ($times.baselineResult -ne $times.candidateResult) { throw "result mismatch" }
    if ($times.baseline -le 0 -or $times.candidate -le 0) {
      throw "$case elapsed time is zero; increase Iterations."
    }
    $rows += [pscustomobject]@{
      case=$case; pair=$pair; order=($order -join ","); iterations=$count; blocks=$Blocks
      warmup=$WarmupIterations; affinity=$(if ($Linux) { $LinuxCpu } else { $Affinity })
      platform=$(if ($Linux) { "linux" } else { "windows" })
      variant=$Variant
      baselineExe=$Baseline; candidateExe=$Candidate
      baselineModule=$BaselineModule; candidateModule=$CandidateModule
      cooldownMs=$CooldownMs
      result=$times.baselineResult
      baseline=$times.baseline; candidate=$times.candidate
      ratio=$times.candidate/$times.baseline
    }
  }
  $logs = @($rows | Where-Object case -EQ $case | ForEach-Object { [Math]::Log($_.ratio) })
  $mean = ($logs | Measure-Object -Average).Average
  $variance = 0.0
  foreach ($x in $logs) { $variance += ($x-$mean)*($x-$mean) }
  $half = $critical*[Math]::Sqrt($variance/($Pairs-1)/$Pairs)
  $upper = [Math]::Exp($mean+$half)
  $limit = if ($case -eq "ordinary") { 1.03 } else { 1.0 }
  $summary += [pscustomobject]@{
    platform=$(if ($Linux) { "linux" } else { "windows" })
    variant=$Variant; case=$case; pairs=$Pairs; ratio=[Math]::Exp($mean)
    lower95=[Math]::Exp($mean-$half); upper95=$upper
    limit=$limit; pass=$(if ($case -eq "ordinary") { $upper -le $limit } else { $upper -lt $limit })
  }
  $summary[-1] | Format-List
}
$rows | Export-Csv -NoTypeInformation -Path "$Output.csv"
if ($Blocks -gt 1) { $blockRows | Export-Csv -NoTypeInformation -Path "$Output-blocks.csv" }
$summary | ConvertTo-Json | Set-Content -Encoding utf8 -Path "$Output.json"
