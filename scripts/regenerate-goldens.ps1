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
$sdp = Join-Path $data "ipmx_golden.sdp.tmp"

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
        --dump-h264 (Join-Path $data "ipmx_golden.h264") `
        --dump-pcap (Join-Path $data "ipmx_golden.pcap")
    if ($LASTEXITCODE -ne 0) {
        throw "Golden generation failed with exit code $LASTEXITCODE."
    }
}
finally {
    Remove-Item -LiteralPath $sdp -ErrorAction SilentlyContinue
}

Write-Host "Regenerated IPMX H.264 and PCAP conformance goldens in '$data'."
