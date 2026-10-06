param([switch]$RemoveSettings, [switch]$RemoveSigningCertificate)
$ErrorActionPreference = 'Stop'
$running = Get-Process -Name Glyph -ErrorAction SilentlyContinue
if ($running) { throw 'Quit Glyph from its tray menu before uninstalling.' }
Get-AppxPackage -Name Glyph.Desktop | Remove-AppxPackage
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
Remove-ItemProperty -LiteralPath $runKey -Name Glyph -ErrorAction SilentlyContinue
if ($RemoveSettings) {
    $settingsDir = [IO.Path]::GetFullPath((Join-Path $env:APPDATA 'Glyph'))
    $expectedDir = [IO.Path]::GetFullPath($env:APPDATA).TrimEnd('\') + '\Glyph'
    if ($settingsDir -ne $expectedDir) { throw 'Unexpected settings path.' }
    if (Test-Path -LiteralPath $settingsDir) { Remove-Item -LiteralPath $settingsDir -Recurse -Force }
}
if ($RemoveSigningCertificate) {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Removing the machine signing certificate requires an elevated PowerShell.' }
    $owned = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=Glyph Local' -and $_.FriendlyName -eq 'Glyph development signing' }
    foreach ($cert in $owned) {
        $trustedPath = 'Cert:\LocalMachine\TrustedPeople\' + $cert.Thumbprint
        if (Test-Path -LiteralPath $trustedPath) { Remove-Item -LiteralPath $trustedPath }
        Remove-Item -LiteralPath $cert.PSPath
    }
}
Write-Host 'Glyph uninstalled. Settings are retained unless -RemoveSettings was specified.'
