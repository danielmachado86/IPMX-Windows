[CmdletBinding()]
param(
    [Parameter()]
    [string]$BuildDirectory = "out/build/windows-msvc"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$build = (Resolve-Path -LiteralPath (Join-Path $root $BuildDirectory)).Path
$sender = (Resolve-Path -LiteralPath (Join-Path $build "bin\ipmx-sender.exe")).Path
$data = Join-Path $root "tests\IPMXConformanceTests\data"
$staging = Join-Path $root "out\golden-staging"
$sdp = Join-Path $staging "ipmx_golden.sdp.tmp"
$h264 = Join-Path $staging "ipmx_golden.h264"
$pcap = Join-Path $staging "ipmx_golden.pcap"
New-Item -ItemType Directory -Path $staging -Force | Out-Null

try {
    & $sender `
        --source test `
        --width 1280 `
        --height 720 `
        --fps 60 `
        --profile high `
        --bitrate-kbps 4000 `
        --max-ip-bitrate-kbps 4400 `
        --maxudp 1200 `
        --duration-seconds 1 `
        --sdp $sdp `
        --dump-h264 $h264 `
        --dump-pcap $pcap
    if ($LASTEXITCODE -ne 0) {
        throw "Golden generation failed with exit code $LASTEXITCODE."
    }
    Copy-Item -LiteralPath $h264 -Destination (Join-Path $data "ipmx_golden.h264") -Force
    Copy-Item -LiteralPath $pcap -Destination (Join-Path $data "ipmx_golden.pcap") -Force
}
finally {
    Remove-Item -LiteralPath $sdp -ErrorAction SilentlyContinue
}

Write-Host "Regenerated IPMX H.264 and PCAP conformance goldens in '$data'."
