[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$BrowserDir,
  [string]$OutputDir = (Join-Path $PSScriptRoot '../out/dist'),
  [string]$KryptonUiDir = (Join-Path $PSScriptRoot '../../KryptonUI'),
  [string]$KeyPath,
  [string[]]$BasePackageJson = @(),
  [switch]$RunTests
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$browser = (Resolve-Path -LiteralPath $BrowserDir).Path
$browserVersion = (Get-Item -LiteralPath (Join-Path $browser 'chrome.exe')).VersionInfo.FileVersion
if ($browserVersion -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw 'The browser file version is invalid.' }
$work = Join-Path $root ('out/packages/offline-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
  & $Program @Arguments
  if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

Push-Location $root
try {
  Invoke-Checked 'pwsh' @('-NoProfile', 'tools/dev.ps1', 'configure',
    '-DKR_OFFLINE_PAYLOAD=', '-DKR_OFFLINE_MANIFEST=', '-DKR_OFFLINE_SIGNATURE=', '-DKR_ALLOW_UNSIGNED=OFF',
    "-DKR_PRODUCT_VERSION=$browserVersion")
  Invoke-Checked 'pwsh' @('-NoProfile', 'tools/dev.ps1', 'build', '-Config', 'Release',
    '--target', 'KryptonSetup', 'KryptonWebSetup', 'KryptonUpdater', 'KryptonUninstaller')
  $webArgs = @('-NoProfile', 'tools/build-web.ps1', '-OutputDir', $OutputDir, '-SkipBuild')
  if ($KeyPath) { $webArgs += @('-KeyPath', $KeyPath) }
  Invoke-Checked 'pwsh' $webArgs
  $prerequisites = Join-Path $work 'prerequisites'
  Invoke-Checked 'pwsh' @('-NoProfile', 'tools/prepare-prerequisites.ps1',
    '-BrowserDir', $browser, '-OutputDir', $prerequisites, '-KryptonUiDir', $KryptonUiDir)
  $stageArgs = @('tools/package-browser.py', 'stage', '--browser-dir', $browser,
    '--work-dir', $work, '--uninstaller', 'out/build/windows/bin/Release/KryptonUninstaller.exe',
    '--updater', 'out/build/windows/bin/Release/KryptonUpdater.exe',
    '--prerequisites-dir', $prerequisites)
  if ($KeyPath) { $stageArgs += @('--key-path', $KeyPath) }
  Invoke-Checked 'python' $stageArgs
  $package = Get-Content -LiteralPath (Join-Path $work 'package.json') -Raw | ConvertFrom-Json
  Invoke-Checked 'pwsh' @('-NoProfile', 'tools/dev.ps1', 'configure', '-Preset', 'windows-offline',
    '-OfflinePayload', $package.archive, '-OfflineManifest', $package.manifest,
    '-OfflineSignature', $package.manifest_signature, '-DKR_ALLOW_UNSIGNED=OFF',
    "-DKR_PRODUCT_VERSION=$browserVersion")
  Invoke-Checked 'pwsh' @('-NoProfile', 'tools/dev.ps1', 'build', '-Preset', 'windows-offline', '-Config', 'Release', '--target', 'KryptonSetup')
  New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
  $output = Join-Path (Resolve-Path -LiteralPath $OutputDir).Path "KryptonSetup-$($package.browser_version)-offline-x64.exe"
  Copy-Item -LiteralPath 'out/build/windows-offline/bin/Release/KryptonSetup.exe' -Destination $output -Force
  Copy-Item -LiteralPath 'out/build/windows/bin/Release/KryptonUpdater.exe' -Destination (Join-Path $OutputDir 'KryptonUpdater.exe') -Force
  Copy-Item -LiteralPath 'out/build/windows/bin/Release/KryptonUninstaller.exe' -Destination (Join-Path $OutputDir 'KryptonUninstaller.exe') -Force
  foreach ($helper in @('KryptonUpdater.exe', 'KryptonUninstaller.exe')) {
    Copy-Item -LiteralPath "out/build/windows/bin/Release/$helper.sig" -Destination (Join-Path $OutputDir "$helper.sig") -Force
  }
  $signArgs = @('-NoProfile', 'tools/release-signing.ps1', 'sign', '-InputFile', $output)
  if ($KeyPath) { $signArgs += @('-KeyPath', $KeyPath) }
  Invoke-Checked 'pwsh' $signArgs
  Invoke-Checked 'python' @('tools/package-browser.py', 'verify-embedded', '--installer', $output,
    '--package-json', (Join-Path $work 'package.json'))
  if ($RunTests) {
    Invoke-Checked 'pwsh' @('-NoProfile', 'tools/dev.ps1', 'test', '-Config', 'Release')
    Invoke-Checked 'out/build/windows/bin/Release/offline_engine_test.exe' @(
      (Join-Path $work 'embedded.7z'), (Join-Path $work 'install-smoke'))
    Invoke-Checked 'out/build/windows/bin/Release/prerequisites_test.exe' @((Join-Path $work 'payload'))
  }
  $releaseArgs = @('tools/prepare-release.py', '--package-json', (Join-Path $work 'package.json'),
    '--output-dir', (Join-Path $OutputDir 'github'))
  if ($KeyPath) { $releaseArgs += @('--key-path', $KeyPath) }
  foreach ($base in $BasePackageJson) { $releaseArgs += @('--base-package-json', $base) }
  Invoke-Checked 'python' $releaseArgs
  $hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
  "$hash  $([IO.Path]::GetFileName($output))" | Set-Content -LiteralPath "$output.sha256" -Encoding ascii
  $alias = Join-Path (Resolve-Path -LiteralPath $OutputDir).Path 'KryptonSetup.exe'
  Copy-Item -LiteralPath $output -Destination $alias -Force
  Copy-Item -LiteralPath "$output.sig" -Destination "$alias.sig" -Force
  "$hash  KryptonSetup.exe" | Set-Content -LiteralPath "$alias.sha256" -Encoding ascii
  [PSCustomObject]@{Installer=$output; WebInstaller=(Join-Path (Resolve-Path -LiteralPath $OutputDir).Path "KryptonSetup-$($package.browser_version)-web-x64.exe"); Bytes=(Get-Item -LiteralPath $output).Length;
    BrowserVersion=$package.browser_version;Files=$package.file_count;VerificationDirectory=$work}
} finally {
  Pop-Location
}
