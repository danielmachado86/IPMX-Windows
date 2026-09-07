[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out/build/windows-msvc',
    [ValidateRange(5,300)][int]$DurationSeconds = 20,
    [string]$InterfaceAddress = '0.0.0.0',
    [string]$OutputDirectory = ('out/remediation/' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bin = Join-Path $root "$BuildDirectory/bin"
$out = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
New-Item -ItemType Directory -Force -Path $out | Out-Null
$results = @()
foreach ($case in @('test','stress','stress-load')) {
    $load = $null
    $sender = $null
    $record = [ordered]@{ Case=$case; DurationSeconds=$DurationSeconds; CaptureComplete=$false; Conformance=$false; LatencyValid=$false; FinalAcceptance=$false }
    try {
        if ($case -eq 'stress-load') {
            $load = Start-Process (Join-Path $bin 'ipmx-load.exe') -ArgumentList @('--cpu-workers',4,'--cpu-duty-percent',80,'--gpu','--duration-seconds',($DurationSeconds+5)) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/$case-load.log" -RedirectStandardError "$out/$case-load.err"
            Start-Sleep -Seconds 1
            if ($load.HasExited) { throw 'Load exited before sender startup' }
        }
        $source = if ($case -eq 'test') {'test'} else {'stress'}
        $sender = Start-Process (Join-Path $bin 'ipmx-sender.exe') -ArgumentList @('--source',$source,'--interface',$InterfaceAddress,'--duration-seconds',$DurationSeconds,'--sdp',"`"$out/$case.sdp`"",'--production-csv',"`"$out/$case.csv`"",'--metrics-json',"`"$out/$case.json`"",'--dump-h264',"`"$out/$case.h264`"",'--dump-pcap',"`"$out/$case.pcap`"",'--require-timing-compliance') -WindowStyle Hidden -PassThru -RedirectStandardOutput "$out/$case.log" -RedirectStandardError "$out/$case.err"
        $sender.WaitForExit()
        $record.SenderExitCode = $sender.ExitCode
        $record.CaptureComplete = $sender.ExitCode -in @(0,2)
        $record.ProductionTimingPassed = $sender.ExitCode -eq 0
        if ($load) { $load.WaitForExit(); $record.LoadExitCode = $load.ExitCode }
    } catch { $record.Error = $_.Exception.Message }
    finally {
        foreach ($process in @($sender,$load)) {
            if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id }
        }
        $record.Hashes = @(Get-ChildItem -LiteralPath $out -File | Where-Object Name -Like "$case.*" | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash)
        $record.SenderSha256 = (Get-FileHash -LiteralPath (Join-Path $bin 'ipmx-sender.exe') -Algorithm SHA256).Hash
        $record.CpuWorkers = if ($case -eq 'stress-load') {4} else {0}
        $record.CpuDutyPercent = 80
        $record.GpuLoad = $case -eq 'stress-load'
        $record.CaptureKind = 'internal-diagnostic'
        $results += [pscustomobject]$record
        $results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$out/result.json"
    }
}
$results
