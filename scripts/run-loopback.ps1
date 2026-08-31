[CmdletBinding()]
param(
    [Parameter()]
    [string]$BuildDirectory = "out/build/windows-msvc",

    [Parameter()]
    [ValidateSet("test", "screen")]
    [string]$Source = "test",

    [Parameter()]
    [ValidateRange(0, 86400)]
    [int]$DurationSeconds = 0
)

$ErrorActionPreference = "Stop"
$resolvedBuild = (Resolve-Path -LiteralPath $BuildDirectory).Path
$senderPath = (Resolve-Path -LiteralPath (Join-Path $resolvedBuild "bin\ipmx-sender.exe")).Path
$receiverPath = (Resolve-Path -LiteralPath (Join-Path $resolvedBuild "bin\ipmx-receiver.exe")).Path
$sessionDirectory = Join-Path $resolvedBuild "loopback"
New-Item -ItemType Directory -Path $sessionDirectory -Force | Out-Null
$sdpPath = Join-Path $sessionDirectory "phase0.sdp"

$senderArguments = @("--source", $Source, "--sdp", $sdpPath)
if ($DurationSeconds -gt 0) {
    $senderArguments += @("--duration-seconds", $DurationSeconds)
}
$sender = Start-Process -FilePath $senderPath -ArgumentList $senderArguments -PassThru

$deadline = [DateTime]::UtcNow.AddSeconds(15)
while (-not (Test-Path -LiteralPath $sdpPath) -and [DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 100
}
if (-not (Test-Path -LiteralPath $sdpPath)) {
    Stop-Process -Id $sender.Id -ErrorAction SilentlyContinue
    throw "The sender did not create an SDP file within 15 seconds."
}

$receiverArguments = @("--sdp", $sdpPath)
if ($DurationSeconds -gt 0) {
    $receiverArguments += @("--duration-seconds", ($DurationSeconds + 2))
}
$receiver = Start-Process -FilePath $receiverPath -ArgumentList $receiverArguments -PassThru

[pscustomobject]@{
    SenderProcessId = $sender.Id
    ReceiverProcessId = $receiver.Id
    SdpPath = $sdpPath
}
