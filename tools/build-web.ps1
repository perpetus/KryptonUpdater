[CmdletBinding()]
param(
  [string]$OutputDir = (Join-Path $PSScriptRoot '../out/dist'),
  [string]$KeyPath,
  [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
  & $Program @Arguments
  if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
Push-Location -LiteralPath $root
try {
  if (-not $SkipBuild) {
    if (-not (Test-Path -LiteralPath 'out/build/windows/build.ninja')) {
      throw 'Configure the Windows build graph with tools/dev.ps1 configure first.'
    }
    Invoke-Checked 'pwsh' @('-NoProfile', 'tools/dev.ps1', 'build', '-Config', 'Release',
      '--target', 'KryptonWebSetup')
  }
  $source = (Resolve-Path -LiteralPath 'out/build/windows/bin/Release/KryptonWebSetup.exe').Path
  $version = (Get-Item -LiteralPath $source).VersionInfo.FileVersion
  if ($version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw 'The web installer version is invalid.' }
  New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
  $output = Join-Path (Resolve-Path -LiteralPath $OutputDir).Path "KryptonSetup-$version-web-x64.exe"
  Copy-Item -LiteralPath $source -Destination $output -Force
  $sign = @('-NoProfile', 'tools/release-signing.ps1', 'sign', '-InputFile', $output)
  if ($KeyPath) { $sign += @('-KeyPath', $KeyPath) }
  Invoke-Checked 'pwsh' $sign
  Invoke-Checked 'pwsh' @('-NoProfile', 'tools/release-signing.ps1', 'verify', '-InputFile', $output)
  Invoke-Checked 'python' @('tools/package-browser.py', 'verify-web', '--installer', $output)
  $hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
  "$hash  $([IO.Path]::GetFileName($output))" | Set-Content -LiteralPath "$output.sha256" -Encoding ascii
  $alias = Join-Path (Resolve-Path -LiteralPath $OutputDir).Path 'KryptonSetup-web.exe'
  Copy-Item -LiteralPath $output -Destination $alias -Force
  Copy-Item -LiteralPath "$output.sig" -Destination "$alias.sig" -Force
  "$hash  KryptonSetup-web.exe" | Set-Content -LiteralPath "$alias.sha256" -Encoding ascii
  [pscustomobject]@{ WebInstaller=$output; Bytes=(Get-Item -LiteralPath $output).Length;
    Version=$version; Repository='perpetus/krypton-windows' }
} finally {
  Pop-Location
}
