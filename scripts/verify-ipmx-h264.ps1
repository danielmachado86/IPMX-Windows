[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$PcapPath,

    [Parameter(Mandatory)]
    [string]$SdpPath,

    [Parameter()]
    [string]$ConfigPath,

    [Parameter()]
    [string]$TestingRoot = "out/tools/ipmx-testing-official",

    [Parameter()]
    [string]$PythonPath,

    [Parameter()]
    [string]$ReportPath
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$pcap = (Resolve-Path -LiteralPath $PcapPath).Path
$sdp = (Resolve-Path -LiteralPath $SdpPath).Path
$testing = (Resolve-Path -LiteralPath (Join-Path $root $TestingRoot)).Path
$validator = Join-Path $testing "ipmx/streams/ipmx-streams-tools/ipmx_h264_validate_pcap.py"
if (-not (Test-Path -LiteralPath $validator)) {
    throw "Official validator not found. Run scripts/fetch-ipmx-testing.ps1 first."
}

if (-not $PythonPath) {
    $python = Get-Command python.exe -ErrorAction SilentlyContinue
    if (-not $python) {
        throw "python.exe was not found; pass -PythonPath."
    }
    $PythonPath = $python.Source
}
$pythonResolved = (Resolve-Path -LiteralPath $PythonPath).Path

$arguments = @(
    $validator,
    "--frames", "2147483647",
    "--cmax",
    "--hrd",
    "--hrd-sim",
    "--hrd-timing",
    "--sdp", $sdp,
    "--full-report"
)
if ($ConfigPath) {
    $config = (Resolve-Path -LiteralPath $ConfigPath).Path
    $arguments += @("--cfg", $config)
}
$arguments += $pcap

$output = & $pythonResolved @arguments 2>&1
$exitCode = $LASTEXITCODE
$output | Write-Output
if ($ReportPath) {
    $report = [System.IO.Path]::GetFullPath($ReportPath)
    $output | Set-Content -LiteralPath $report -Encoding utf8
}
$reportedFailures = @($output | Where-Object { $_ -match "^FAIL " })
$required = @('TR-10-1-8.1-CMAX', 'H264-HRD-01', 'H264-HRD-09', 'H264-HRD-10',
    'H264-HRD-SIM-01', 'H264-HRD-SIM-02', 'H264-HRD-TIME-EQC3', 'H264-HRD-TIME-01')
foreach ($id in $required) {
    if (-not ($output -match ('^PASS ' + [regex]::Escape($id) + ':'))) {
        throw "Required validation $id did not PASS. Check FFmpeg availability and capture completeness."
    }
}
if ($exitCode -ne 0 -or $reportedFailures.Count -ne 0) {
    throw "Official IPMX H.264 validation failed: exit code $exitCode, reported failures $($reportedFailures.Count)."
}
