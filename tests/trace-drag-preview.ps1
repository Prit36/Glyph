param([ValidateRange(10, 60)][int]$Seconds = 20)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Windows requires Administrator rights for GPU/display tracing. Run this script from an Administrator PowerShell terminal.'
}
$preview = Join-Path $projectRoot 'dist/frame-pacing-preview/Glyph.exe'
if (!(Test-Path -LiteralPath $preview)) { throw 'Build the preview first: ./build.ps1 -SkipPackage -OutputDirectory dist/frame-pacing-preview' }
$wpr = (Get-Command wpr.exe -ErrorAction Stop).Source
$state = & $wpr -status 2>&1
if ($LASTEXITCODE -ne 0 -or ($state -join "`n") -notmatch 'WPR is not recording') {
    throw 'WPR may already be recording. This script will not change an existing recording.'
}
$captureRoot = Join-Path $projectRoot ('build/frame-pacing-trace-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
New-Item -ItemType Directory -Path $captureRoot | Out-Null
$timingPath = Join-Path $captureRoot 'paint-timing.csv'
$instances = @(Get-Process Glyph -ErrorAction SilentlyContinue)
if ($instances.Count -gt 1) { throw 'More than one Glyph instance is running; quit them before testing.' }
if ($instances.Count -eq 1) {
    $process = $instances[0]
    if ([IO.Path]::GetFullPath($process.Path) -ne [IO.Path]::GetFullPath($preview)) {
        throw 'Quit the released Glyph tray instance first. The trace must measure the frame-pacing preview.'
    }
    $commandLine = (Get-CimInstance Win32_Process -Filter ("ProcessId={0}" -f $process.Id)).CommandLine
    $traceArgument = [regex]::Match($commandLine, '--trace-drag\s+(?:"([^"]+)"|(\S+))')
    if (!$traceArgument.Success) { throw 'Quit this preview and rerun the helper so app timing diagnostics can be enabled.' }
    $timingPath = if ($traceArgument.Groups[1].Success) { $traceArgument.Groups[1].Value } else { $traceArgument.Groups[2].Value }
} else {
    $process = Start-Process -FilePath $preview -ArgumentList @('--trace-drag', ('"' + $timingPath + '"')) -WindowStyle Hidden -PassThru
    $null = $process.WaitForInputIdle(5000)
}
if ($process.HasExited) { throw 'The preview exited before recording started.' }
[pscustomobject]@{
    StartedUtc = [DateTime]::UtcNow.ToString('o')
    DurationSeconds = $Seconds
    Glyph = [pscustomobject]@{ Pid=$process.Id; Path=$preview; Version=(Get-Item -LiteralPath $preview).VersionInfo.FileVersion; Sha256=(Get-FileHash -LiteralPath $preview).Hash }
    AppTracePath = $timingPath
    Display = @(Get-CimInstance Win32_VideoController | Select-Object Name,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate)
    Note = 'Frame-pacing profiling build. App CSV contains QPC timing and box geometry, no screenshot pixels or recognized text. GPU/desktop ETL is system-wide; attribute results to this Glyph PID.'
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $captureRoot 'capture.json') -Encoding UTF8
$ownedRecording = $false
try {
    & $wpr -start GPU -start DesktopComposition -filemode
    if ($LASTEXITCODE -ne 0) { throw 'Windows did not permit recording to start.' }
    $ownedRecording = $true
    Write-Host ("Recording {0} seconds. Draw boxes repeatedly; press Esc DURING each drag to cancel without OCR." -f $Seconds)
    Start-Process -FilePath $preview -ArgumentList '--capture' -WindowStyle Hidden -Wait
    Start-Sleep -Seconds $Seconds
} finally {
    if ($ownedRecording) {
        & $wpr -stop (Join-Path $captureRoot 'drag.etl')
        if ($LASTEXITCODE -ne 0) { Write-Warning 'Saving failed. Inspect wpr -status; this script started the recording.' }
    }
}
Write-Host 'Recording finished. Press Esc to cancel any drag, then Esc again to close capture and save paint timings.'
Write-Host ("Saved recording directory: {0}" -f $captureRoot)
Write-Host 'Use capture.json to identify the recorded Glyph PID, binary hash, and app timing CSV when analyzing the ETL.'
