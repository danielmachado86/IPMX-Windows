[CmdletBinding()]
param(
    [Parameter()]
    [string]$BuildDirectory = "out/build/windows-msvc",

    [Parameter()]
    [ValidateRange(10, 86400)]
    [int]$DurationSeconds = 1800,

    [Parameter()]
    [ValidateSet("test", "screen")]
    [string]$Source = "test",

    [Parameter()]
    [ValidateRange(1, 1024)]
    [double]$MaximumGrowthMiB = 5.0
)

$ErrorActionPreference = "Stop"
$resolvedBuild = (Resolve-Path -LiteralPath $BuildDirectory).Path
$senderPath = (Resolve-Path -LiteralPath (Join-Path $resolvedBuild "bin\ipmx-sender.exe")).Path
$receiverPath = (Resolve-Path -LiteralPath (Join-Path $resolvedBuild "bin\ipmx-receiver.exe")).Path
if (-not $senderPath.StartsWith($resolvedBuild, [System.StringComparison]::OrdinalIgnoreCase) -or
    -not $receiverPath.StartsWith($resolvedBuild, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Resolved executable escaped the requested build directory."
}

$outputDirectory = Join-Path $resolvedBuild "stability"
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$receiverLog = Join-Path $outputDirectory "receiver.log"
$receiverError = Join-Path $outputDirectory "receiver.err.log"
$senderLog = Join-Path $outputDirectory "sender.log"
$senderError = Join-Path $outputDirectory "sender.err.log"
$samplesPath = Join-Path $outputDirectory "memory_samples.csv"
$sdpPath = Join-Path $outputDirectory ("phase0-" + [guid]::NewGuid().ToString("N") + ".sdp")

$receiver = $null
$sender = $null
try {
    $sender = Start-Process -FilePath $senderPath -ArgumentList @(
        "--source", $Source,
        "--duration-seconds", $DurationSeconds,
        "--sdp", $sdpPath
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
                Second = [math]::Round($stopwatch.Elapsed.TotalSeconds, 1)
                SenderWorkingSetMiB = [math]::Round($sender.WorkingSet64 / 1MB, 3)
                ReceiverWorkingSetMiB = [math]::Round($receiver.WorkingSet64 / 1MB, 3)
            })
        }
        Start-Sleep -Seconds 1
    }
    $sender.WaitForExit()
    $receiver.WaitForExit()
    $samples | Export-Csv -LiteralPath $samplesPath -NoTypeInformation

    if ($samples.Count -lt 20) {
        throw "Not enough memory samples were collected."
    }
    $window = [math]::Min(60, [math]::Floor($samples.Count / 3))
    $first = $samples | Select-Object -First $window
    $last = $samples | Select-Object -Last $window
    $senderGrowth = ($last | Measure-Object SenderWorkingSetMiB -Average).Average -
                    ($first | Measure-Object SenderWorkingSetMiB -Average).Average
    $receiverGrowth = ($last | Measure-Object ReceiverWorkingSetMiB -Average).Average -
                      ($first | Measure-Object ReceiverWorkingSetMiB -Average).Average
    $receiverText = Get-Content -LiteralPath $receiverLog -Raw
    $transportPassed = $receiver.ExitCode -eq 0 -and $receiverText.Contains("Receiver stopped: PASS")
    $memoryPassed = $senderGrowth -le $MaximumGrowthMiB -and $receiverGrowth -le $MaximumGrowthMiB

    [pscustomobject]@{
        DurationSeconds = $DurationSeconds
        Samples = $samples.Count
        SenderExitCode = $sender.ExitCode
        ReceiverExitCode = $receiver.ExitCode
        SenderGrowthMiB = [math]::Round($senderGrowth, 3)
        ReceiverGrowthMiB = [math]::Round($receiverGrowth, 3)
        TransportPassed = $transportPassed
        MemoryPassed = $memoryPassed
        Overall = $transportPassed -and $memoryPassed
    } | Format-List

    if (-not ($transportPassed -and $memoryPassed)) {
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
