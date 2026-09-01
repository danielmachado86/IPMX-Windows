[CmdletBinding()]
param(
    [Parameter()]
    [string]$BuildDirectory = "out/build/windows-msvc",

    [Parameter()]
    [ValidateRange(10, 86400)]
    [int]$DurationSeconds = 90,

    [Parameter()]
    [ValidateSet("test", "screen")]
    [string]$Source = "test",

    [Parameter()]
    [ValidateRange(1, 1024)]
    [double]$MaximumGrowthMiB = 5.0,

    [Parameter()]
    [ValidateRange(0, 600)]
    [int]$WarmupSeconds = 60,

    [Parameter()]
    [ValidateRange(1, 10000)]
    [int]$MaximumLatencyMs = 250
)

$ErrorActionPreference = "Stop"
$resolvedBuild = (Resolve-Path -LiteralPath $BuildDirectory).Path
$senderPath = (Resolve-Path -LiteralPath (Join-Path $resolvedBuild "bin\ipmx-sender.exe")).Path
$receiverPath = (Resolve-Path -LiteralPath (Join-Path $resolvedBuild "bin\ipmx-receiver.exe")).Path
if (-not $senderPath.StartsWith($resolvedBuild, [System.StringComparison]::OrdinalIgnoreCase) -or
    -not $receiverPath.StartsWith($resolvedBuild, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Resolved executable escaped the requested build directory."
}

$runId = (Get-Date -Format "yyyyMMdd-HHmmss") + "-$Source-" + [guid]::NewGuid().ToString("N").Substring(0, 8)
$outputDirectory = Join-Path (Join-Path $resolvedBuild "stability") $runId
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$receiverLog = Join-Path $outputDirectory "receiver.log"
$receiverError = Join-Path $outputDirectory "receiver.err.log"
$senderLog = Join-Path $outputDirectory "sender.log"
$senderError = Join-Path $outputDirectory "sender.err.log"
$samplesPath = Join-Path $outputDirectory "memory_samples.csv"
$sdpPath = Join-Path $outputDirectory ("ipmx-" + [guid]::NewGuid().ToString("N") + ".sdp")

$receiver = $null
$sender = $null
try {
    $sender = Start-Process -FilePath $senderPath -ArgumentList @(
        "--source", $Source,
        "--duration-seconds", $DurationSeconds,
        "--sdp", $sdpPath,
        "--require-timing-compliance"
    ) -RedirectStandardOutput $senderLog -RedirectStandardError $senderError `
        -WindowStyle Hidden -PassThru
    $sdpDeadline = [datetime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path -LiteralPath $sdpPath) -and [datetime]::UtcNow -lt $sdpDeadline) {
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path -LiteralPath $sdpPath)) {
        throw "Sender did not create an SDP file within 15 seconds."
    }
    $receiverArguments = @(
        "--sdp", $sdpPath,
        "--duration-seconds", ($DurationSeconds + 5),
        "--max-latency-ms", $MaximumLatencyMs,
        "--require-zero-loss"
    )
    $receiver = Start-Process -FilePath $receiverPath -ArgumentList $receiverArguments `
        -RedirectStandardOutput $receiverLog -RedirectStandardError $receiverError `
        -WindowStyle Hidden -PassThru

    $samples = [System.Collections.Generic.List[object]]::new()
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    while (-not $sender.HasExited -and $stopwatch.Elapsed.TotalSeconds -lt ($DurationSeconds + 15)) {
        $sender.Refresh()
        $receiver.Refresh()
        if (-not $sender.HasExited -and -not $receiver.HasExited) {
            $samples.Add([pscustomobject]@{
                    Second                = [math]::Round($stopwatch.Elapsed.TotalSeconds, 1)
                    SenderWorkingSetMiB   = [math]::Round($sender.WorkingSet64 / 1MB, 3)
                    ReceiverWorkingSetMiB = [math]::Round($receiver.WorkingSet64 / 1MB, 3)
                })
        }
        Start-Sleep -Seconds 1
    }
    $sender.WaitForExit()
    $receiver.WaitForExit()
    $samples | Export-Csv -LiteralPath $samplesPath -NoTypeInformation

    $effectiveWarmup = [math]::Min($WarmupSeconds, [math]::Floor($DurationSeconds / 5))
    $steadySamples = @($samples | Where-Object { $_.Second -ge $effectiveWarmup })
    $senderGrowth = [double]::NaN
    $receiverGrowth = [double]::NaN
    $memoryPassed = $false
    if ($steadySamples.Count -ge 20) {
        $window = [math]::Min(60, [math]::Floor($steadySamples.Count / 3))
        $first = $steadySamples | Select-Object -First $window
        $last = $steadySamples | Select-Object -Last $window
        $senderGrowth = ($last | Measure-Object SenderWorkingSetMiB -Average).Average -
        ($first | Measure-Object SenderWorkingSetMiB -Average).Average
        $receiverGrowth = ($last | Measure-Object ReceiverWorkingSetMiB -Average).Average -
        ($first | Measure-Object ReceiverWorkingSetMiB -Average).Average
        $memoryPassed = $senderGrowth -le $MaximumGrowthMiB -and
        $receiverGrowth -le $MaximumGrowthMiB
    }
    $receiverText = Get-Content -LiteralPath $receiverLog -Raw
    $senderText = Get-Content -LiteralPath $senderLog -Raw
    $transportPassed = $receiver.ExitCode -eq 0 -and $receiverText.Contains("Receiver stopped: PASS")
    $timingPassed = $sender.ExitCode -eq 0 -and $senderText.Contains("timing=PASS")

    [pscustomobject]@{
        DurationSeconds   = $DurationSeconds
        WarmupSeconds     = $effectiveWarmup
        Samples           = $samples.Count
        SenderExitCode    = $sender.ExitCode
        ReceiverExitCode  = $receiver.ExitCode
        SenderGrowthMiB   = [math]::Round($senderGrowth, 3)
        ReceiverGrowthMiB = [math]::Round($receiverGrowth, 3)
        TransportPassed   = $transportPassed
        TimingPassed      = $timingPassed
        MemoryPassed      = $memoryPassed
        Overall           = $transportPassed -and $timingPassed -and $memoryPassed
        OutputDirectory   = $outputDirectory
    } | Format-List

    if (-not ($transportPassed -and $timingPassed -and $memoryPassed)) {
        exit 2
    }
}
finally {
    if ($sender -and -not $sender.HasExited) {
        Stop-Process -Id $sender.Id -ErrorAction SilentlyContinue
    }
    if ($receiver -and -not $receiver.HasExited) {
        Stop-Process -Id $receiver.Id -ErrorAction SilentlyContinue
    }
}
