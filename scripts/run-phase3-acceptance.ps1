[CmdletBinding()]
param(
    [Parameter()]
    [string]$BuildDirectory = "out/build/windows-msvc",

    [Parameter()]
    [ValidateRange(5, 86400)]
    [int]$DurationSeconds = 300,

    [Parameter()]
    [ValidateRange(0, 1024)]
    [int]$CpuWorkers = [math]::Max(1, [Environment]::ProcessorCount / 2),

    [Parameter()]
    [ValidateRange(1, 100)]
    [int]$CpuDutyPercent = 80,

    [Parameter()]
    [switch]$DisableGpuLoad,

    [Parameter()]
    [string]$TestingRoot = "out/tools/ipmx-testing-official",

    [Parameter()]
    [string]$PythonPath,

    [Parameter()]
    [string]$ExternalPcapPath,

    [Parameter()]
    [string]$ExternalSdpPath,

    [Parameter()]
    [string]$CaptureProvenance,

    [Parameter()]
    [ValidateRange(1, 10000)]
    [int]$MaximumLatencyMs = 250,

    [Parameter()]
    [switch]$RequireExternalCapture
)

$ErrorActionPreference = "Stop"
if ($ExternalPcapPath) {
    if (-not $ExternalSdpPath -or -not $CaptureProvenance) {
        throw "External validation requires the matching -ExternalSdpPath and -CaptureProvenance (capture host/interface, timestamp method and session)."
    }
    $externalArguments = @{
        PcapPath = $ExternalPcapPath
        SdpPath = $ExternalSdpPath
        TestingRoot = $TestingRoot
    }
    if ($PythonPath) { $externalArguments.PythonPath = $PythonPath }
    & (Join-Path $PSScriptRoot 'verify-ipmx-h264.ps1') @externalArguments
    [pscustomobject]@{
        CaptureValidationPassed = $true
        Provenance = $CaptureProvenance
        PcapSha256 = (Get-FileHash -LiteralPath $ExternalPcapPath -Algorithm SHA256).Hash
        SdpSha256 = (Get-FileHash -LiteralPath $ExternalSdpPath -Algorithm SHA256).Hash
        Note = 'Provenance is operator supplied; no new sender session was started.'
    }
    return
}
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$build = (Resolve-Path -LiteralPath (Join-Path $root $BuildDirectory)).Path
$senderPath = (Resolve-Path -LiteralPath (Join-Path $build "bin/ipmx-sender.exe")).Path
$receiverPath = (Resolve-Path -LiteralPath (Join-Path $build "bin/ipmx-receiver.exe")).Path
$loadPath = (Resolve-Path -LiteralPath (Join-Path $build "bin/ipmx-load.exe")).Path
$configPath = (Resolve-Path -LiteralPath (Join-Path $root "tests/IPMXConformanceTests/data/ipmx_h264_720p60.cfg")).Path

if ($RequireExternalCapture -and -not $ExternalPcapPath) {
    throw "Final acceptance requires -ExternalPcapPath from another machine or hardware timestamps."
}

$runId = (Get-Date -Format "yyyyMMdd-HHmmss") + "-" + [guid]::NewGuid().ToString("N").Substring(0, 8)
$outputDirectory = Join-Path (Join-Path $build "phase3") $runId
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$sdpPath = Join-Path $outputDirectory "phase3.sdp"
$pcapPath = Join-Path $outputDirectory "phase3-internal.pcap"
$metricsCsv = Join-Path $outputDirectory "traffic-metrics.csv"
$metricsJson = Join-Path $outputDirectory "traffic-metrics.json"
$validatorReport = Join-Path $outputDirectory "official-validator.log"
$senderLog = Join-Path $outputDirectory "sender.log"
$senderError = Join-Path $outputDirectory "sender.err.log"
$receiverLog = Join-Path $outputDirectory "receiver.log"
$receiverError = Join-Path $outputDirectory "receiver.err.log"
$loadLog = Join-Path $outputDirectory "load.log"
$loadError = Join-Path $outputDirectory "load.err.log"

$sender = $null
$receiver = $null
$load = $null
try {
    $loadArguments = @(
        "--duration-seconds", ($DurationSeconds + 10),
        "--cpu-workers", $CpuWorkers,
        "--cpu-duty-percent", $CpuDutyPercent
    )
    if (-not $DisableGpuLoad) {
        $loadArguments += "--gpu"
    }
    $load = Start-Process -FilePath $loadPath -ArgumentList $loadArguments `
        -RedirectStandardOutput $loadLog -RedirectStandardError $loadError `
        -WindowStyle Hidden -PassThru

    $senderArguments = @(
        "--source", "stress",
        "--duration-seconds", $DurationSeconds,
        "--sdp", $sdpPath,
        "--dump-pcap", $pcapPath,
        "--metrics-csv", $metricsCsv,
        "--metrics-json", $metricsJson,
        "--production-csv", (Join-Path $outputDirectory "production.csv"),
        "--require-timing-compliance"
    )
    $sender = Start-Process -FilePath $senderPath -ArgumentList $senderArguments `
        -RedirectStandardOutput $senderLog -RedirectStandardError $senderError `
        -WindowStyle Hidden -PassThru

    $sdpDeadline = [datetime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path -LiteralPath $sdpPath) -and [datetime]::UtcNow -lt $sdpDeadline) {
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path -LiteralPath $sdpPath)) {
        throw "Sender did not create its SDP within 15 seconds."
    }

    $receiver = Start-Process -FilePath $receiverPath -ArgumentList @(
        "--sdp", $sdpPath,
        "--duration-seconds", ($DurationSeconds + 5),
        "--max-latency-ms", $MaximumLatencyMs,
        "--local-clock", "--require-zero-loss"
    ) -RedirectStandardOutput $receiverLog -RedirectStandardError $receiverError `
        -WindowStyle Hidden -PassThru

    $sender.WaitForExit()
    $receiver.WaitForExit()
    $load.WaitForExit()

    $validationPcap = $pcapPath
    $captureKind = "internal-diagnostic"
    if ($ExternalPcapPath) {
        $validationPcap = (Resolve-Path -LiteralPath $ExternalPcapPath).Path
        $captureKind = "external-or-hardware"
    }
    $validatorArguments = @{
        PcapPath   = $validationPcap
        SdpPath    = $sdpPath
        ConfigPath = $configPath
        TestingRoot = $TestingRoot
        ReportPath = $validatorReport
    }
    if ($PythonPath) {
        $validatorArguments["PythonPath"] = $PythonPath
    }
    & (Join-Path $PSScriptRoot "verify-ipmx-h264.ps1") @validatorArguments

    if ($sender.ExitCode -ne 0 -or $receiver.ExitCode -ne 0 -or $load.ExitCode -ne 0) {
        throw "Acceptance failed: sender=$($sender.ExitCode), receiver=$($receiver.ExitCode), load=$($load.ExitCode). See logs and official report."
    }

    $result = [ordered]@{
        Overall           = $true
        FinalAcceptance   = $false
        MaximumLatencyMs  = $MaximumLatencyMs
        DurationSeconds   = $DurationSeconds
        CpuWorkers        = $CpuWorkers
        CpuDutyPercent    = $CpuDutyPercent
        GpuLoad           = -not $DisableGpuLoad
        CaptureKind       = $captureKind
        SenderExitCode    = $sender.ExitCode
        ReceiverExitCode  = $receiver.ExitCode
        LoadExitCode      = $load.ExitCode
        OutputDirectory   = $outputDirectory
    }
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputDirectory "result.json")
    [pscustomobject]$result | Format-List
}
catch {
    [ordered]@{
        Overall = $false
        FinalAcceptance = $false
        Error = $_.Exception.Message
        MaximumLatencyMs = $MaximumLatencyMs
        DurationSeconds = $DurationSeconds
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputDirectory 'result.json')
    throw
}
finally {
    foreach ($process in @($sender, $receiver, $load)) {
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        }
    }
    Get-ChildItem -LiteralPath $outputDirectory -File |
        Where-Object { $_.Name -ne 'hashes.json' } |
        Get-FileHash -Algorithm SHA256 | Select-Object Path, Hash |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputDirectory 'hashes.json')

}
