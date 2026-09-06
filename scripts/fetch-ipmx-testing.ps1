[CmdletBinding()]
param(
    [Parameter()]
    [string]$Destination = "out/tools/ipmx-testing-official",

    [Parameter()]
    [string]$Repository = "https://github.com/alabou/nmos-testing.git",

    [Parameter()]
    [string]$Branch = "testing-IPMX"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$destinationPath = [System.IO.Path]::GetFullPath((Join-Path $root $Destination))
if (-not $destinationPath.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Testing package destination escaped the workspace."
}

$git = Get-Command git.exe -ErrorAction SilentlyContinue
if (-not $git) {
    throw "git.exe was not found."
}

if (Test-Path -LiteralPath $destinationPath) {
    $validator = Join-Path $destinationPath "ipmx/streams/ipmx-streams-tools/ipmx_h264_validate_pcap.py"
    if (-not (Test-Path -LiteralPath $validator)) {
        throw "Destination exists but does not contain the official H.264 validator."
    }
    Write-Output $destinationPath
    return
}

New-Item -ItemType Directory -Path (Split-Path -Parent $destinationPath) -Force | Out-Null
& $git.Source clone --depth 1 --filter=blob:none --sparse --branch $Branch $Repository $destinationPath
if ($LASTEXITCODE -ne 0) {
    throw "Could not clone the official IPMX testing package."
}
& $git.Source -C $destinationPath sparse-checkout set ipmx cfg docs
if ($LASTEXITCODE -ne 0) {
    throw "Could not configure the IPMX testing sparse checkout."
}
Write-Output $destinationPath
