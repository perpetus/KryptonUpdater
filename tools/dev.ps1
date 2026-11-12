<#
.SYNOPSIS
  KryptonUpdater developer helper. Enters the Visual Studio developer environment and configures vcpkg.
#>
[CmdletBinding()]
param(
  [Parameter(Position = 0)]
  [ValidateSet('configure', 'build', 'test', 'clean', 'preview')]
  [string]$Command = 'build',

  [ValidateSet('Debug', 'Release')]
  [string]$Config = 'Debug',

  [string]$Preset = 'windows',
  [string]$OfflinePayload,
  [string]$OfflineManifest,
  [string]$OfflineSignature,

  [Parameter(ValueFromRemainingArguments)]
  [string[]]$Rest = @()
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "out/build/$Preset"
$buildPreset = "$Preset-$($Config.ToLower())"

# Resolve user's actual vcpkg toolchain before VS environment variables are set
function Get-VcpkgToolchain {
  $vcpkgCmd = Get-Command vcpkg.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
  if ($vcpkgCmd) {
    $vcpkgRoot = Split-Path -Parent $vcpkgCmd
    $toolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    if (Test-Path $toolchain) {
      return @{ Root = $vcpkgRoot; Toolchain = $toolchain }
    }
  }
  return $null
}

$vcpkgInfo = Get-VcpkgToolchain

function Enter-DevShell {
  if ($env:VSCMD_VER) { return }

  $vswhere = Get-Command vswhere.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
  if (-not $vswhere) {
    $candidates = @(
      "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
      "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
    )
    $vswhere = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
  }

  if (-not $vswhere) { throw 'Visual Studio was not found (no vswhere.exe).' }
  $vs = & $vswhere -latest -prerelease -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $vs) { throw 'No Visual Studio installation with the C++ x64 tools was found.' }
  $installerDir = Split-Path -Parent $vswhere
  if ($installerDir -notin ($env:PATH -split ';')) { $env:PATH = "$installerDir;$env:PATH" }
  Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

Enter-DevShell

# Explicitly ensure VCPKG_ROOT points to user's installation with poco
if ($vcpkgInfo) {
  $env:VCPKG_ROOT = $vcpkgInfo.Root
}

$cmakeArgs = @()
if ($PSBoundParameters.ContainsKey('OfflinePayload')) {
  $cmakeArgs += "-DKR_OFFLINE_PAYLOAD=$OfflinePayload"
}
if ($PSBoundParameters.ContainsKey('OfflineManifest')) {
  $cmakeArgs += "-DKR_OFFLINE_MANIFEST=$OfflineManifest"
}
if ($PSBoundParameters.ContainsKey('OfflineSignature')) {
  $cmakeArgs += "-DKR_OFFLINE_SIGNATURE=$OfflineSignature"
}
if ($vcpkgInfo) {
  $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$($vcpkgInfo.Toolchain)"
  $cmakeArgs += "-DVCPKG_TARGET_TRIPLET=x64-windows-static"
}

Push-Location $root
try {
  switch ($Command) {
    'configure' {
      cmake --preset $Preset @cmakeArgs @Rest
    }
    'build' {
      if (-not (Test-Path (Join-Path $buildDir 'build.ninja'))) {
        cmake --preset $Preset @cmakeArgs
      }
      cmake --build --preset $buildPreset @Rest
    }
    'test' {
      if (-not (Test-Path (Join-Path $buildDir 'build.ninja'))) {
        cmake --preset $Preset @cmakeArgs
      }
      cmake --build --preset $buildPreset
      if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
      ctest --preset $buildPreset --output-on-failure @Rest
    }
    'preview' {
      & "$buildDir/bin/$Config/kr_ui_preview.exe" @Rest
    }
    'clean' {
      if (Test-Path 'out') { Remove-Item -Recurse -Force 'out' }
    }
  }
}
finally {
  Pop-Location
}
if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
