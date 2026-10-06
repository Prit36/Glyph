# Run explicitly in an elevated PowerShell. This signs the local package, trusts
# that certificate, and installs Glyph for the current Windows user.
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Open PowerShell as Administrator under your own account, then run this script. It needs elevation to trust the local signing certificate.'
}
$packageRoot = if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'Glyph.msix')) { $PSScriptRoot } else { Join-Path $PSScriptRoot 'dist' }
$unsignedPackage = Join-Path $packageRoot 'Glyph.msix'
if (!(Test-Path -LiteralPath $unsignedPackage)) { throw 'Build Glyph first with .\build.ps1.' }
$sdkBinRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$sdkDir = Get-ChildItem -LiteralPath $sdkBinRoot -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'x64\signtool.exe') } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$sdkDir) { throw 'The Windows SDK SignTool is required for local development installation.' }
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=Glyph Local' -and $_.FriendlyName -eq 'Glyph development signing' -and $_.NotAfter -gt (Get-Date).AddDays(1) } | Select-Object -First 1
if (!$cert) {
    $cert = New-SelfSignedCertificate -Type Custom -Subject 'CN=Glyph Local' -FriendlyName 'Glyph development signing' -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(2) -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3','2.5.29.19={text}')
}
$signedPackage = Join-Path $packageRoot 'Glyph.signed.msix'
Copy-Item -LiteralPath $unsignedPackage -Destination $signedPackage -Force
& (Join-Path $sdkDir.FullName 'x64\signtool.exe') sign /fd SHA256 /sha1 $cert.Thumbprint $signedPackage
if ($LASTEXITCODE -ne 0) { throw 'Package signing failed.' }
$certFile = Join-Path $packageRoot 'Glyph.cer'
Export-Certificate -Cert $cert -FilePath $certFile | Out-Null
Import-Certificate -FilePath $certFile -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null
Add-AppxPackage -Path $signedPackage
Write-Host 'Installed Glyph. Launch it from Start, then press Ctrl+Alt+T.'
Write-Host 'Start at login is optional and can be enabled from the tray menu.'
