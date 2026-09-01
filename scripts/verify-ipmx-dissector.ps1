[CmdletBinding()]
param(
    [Parameter()]
    [string]$PcapPath = "tests/IPMXConformanceTests/data/ipmx_golden.pcap",

    [Parameter(Mandatory)]
    [string]$DissectorPath,

    [Parameter()]
    [string]$TSharkPath
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$pcap = (Resolve-Path -LiteralPath (Join-Path $root $PcapPath)).Path
$dissector = (Resolve-Path -LiteralPath $DissectorPath).Path

if (-not $TSharkPath) {
    $command = Get-Command tshark.exe -ErrorAction SilentlyContinue
    if ($command) {
        $TSharkPath = $command.Source
    } else {
        $installed = "C:\Program Files\Wireshark\tshark.exe"
        if (Test-Path -LiteralPath $installed) {
            $TSharkPath = $installed
        } else {
            throw "tshark was not found. Install Wireshark or pass -TSharkPath."
        }
    }
}

$rows = & $TSharkPath `
    -r $pcap `
    -X "lua_script:$dissector" `
    -d "udp.port==5005,rtcp" `
    -Y "udp.dstport == 5005" `
    -T fields `
    -E "separator=|" `
    -E "occurrence=a" `
    -e frame.number `
    -e ipmx_rtcp_info.version `
    -e ipmx_rtcp_info.media_info_type `
    -e ipmx_rtcp_info.block_length.error
if ($LASTEXITCODE -ne 0) {
    throw "tshark/dissector verification failed with exit code $LASTEXITCODE."
}

$reportCount = 0
foreach ($row in $rows) {
    $columns = $row -split '\|', 4
    if ($columns.Count -lt 3 -or -not $columns[1]) {
        throw "The IPMX dissector did not decode frame '$($columns[0])'."
    }
    if ($columns.Count -ge 4 -and $columns[3]) {
        throw "The IPMX dissector reported an invalid block length in frame '$($columns[0])'."
    }
    $types = $columns[2] -split ','
    if ($types -notcontains '5' -or $types -notcontains '10') {
        throw "Frame '$($columns[0])' does not contain Media Info Blocks 0x0005 and 0x000A."
    }
    ++$reportCount
}

if ($reportCount -eq 0) {
    throw "No RTCP Sender Reports were found on UDP port 5005."
}

Write-Host "IPMX dissector verification passed for $reportCount RTCP Sender Reports."
