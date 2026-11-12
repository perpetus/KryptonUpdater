[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$PayloadRoot,
  [Parameter(Mandatory)][string]$UiSource
)

# This pipeline creates artifacts only. Publishing an update is separate.
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true') { throw 'Use this entry point on the protected signing runner.' }
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$payload = (Resolve-Path -LiteralPath $PayloadRoot).Path
$browser = Join-Path $payload 'browser/out/Default'
$runtime = Join-Path $payload 'ui-runtime'
$output = Join-Path $payload 'dist'
$record = Get-Content -LiteralPath (Join-Path $payload 'browser/browser-build.json') -Raw | ConvertFrom-Json
$uiInfo = Get-Content -LiteralPath (Join-Path $runtime 'build-info.json') -Raw | ConvertFrom-Json
if ((Get-FileHash -LiteralPath (Join-Path $UiSource 'cmake/WindowsAppSDK.cmake') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $uiInfo.sdk_configuration_sha256) {
  throw 'UI SDK configuration differs from the source that built this runtime.'
}
$version = (Get-Item -LiteralPath (Join-Path $browser 'chrome.exe')).VersionInfo.FileVersion
if ($version -ne $record.product_version -or $version -notmatch '^\d+\.\d+\.\d+\.\d+$') {
  throw 'Browser executable version differs from its build record.'
}
$key = if ($env:KRYPTON_RELEASE_KEY_PATH) { $env:KRYPTON_RELEASE_KEY_PATH }
  else { Join-Path $env:USERPROFILE '.krypton/signing/release-key.dpapi' }
if (-not (Test-Path -LiteralPath $key)) { throw 'Provision the existing DPAPI release key for the signing-runner account.' }
$ninja = (Get-Command $(if ($env:KRYPTON_NINJA) { $env:KRYPTON_NINJA } else { 'ninja.exe' }) -ErrorAction Stop).Source
$ninjaVersion = & $ninja --version
if ($LASTEXITCODE -ne 0 -or $ninjaVersion -ne '1.13.2') { throw 'Native Ninja 1.13.2 is required.' }
$online = Join-Path $root 'out/build/ci-windows'
$offline = Join-Path $root 'out/build/ci-offline'
foreach ($graph in @($online, $offline)) {
  if (Test-Path -LiteralPath (Join-Path $graph 'CMakeCache.txt')) {
    foreach ($name in @('.ninja_log', '.ninja_deps')) {
      if (-not (Test-Path -LiteralPath (Join-Path $graph $name))) { throw "Missing $name in $graph; preserve the graph." }
    }
    if ((Get-Content -LiteralPath (Join-Path $graph '.ninja_log') -TotalCount 1) -ne '# ninja log v7') { throw "Incompatible tracking in $graph; no graph was opened." }
    $cached = Get-Content -LiteralPath (Join-Path $graph 'CMakeCache.txt') |
      Where-Object { $_ -like 'CMAKE_MAKE_PROGRAM:FILEPATH=*' }
    if (-not $cached -or $cached.Substring($cached.IndexOf('=') + 1).Replace('/', '\') -ne $ninja.Replace('/', '\')) {
      throw "The Ninja executable changed for $graph; preserve the graph."
    }
  }
}
$installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer'
$visualStudio = & (Join-Path $installer 'vswhere.exe') -latest -prerelease -products '*' `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) { throw 'Visual Studio x64 C++ tools are required.' }
$env:PATH = "$installer;$(Split-Path -Parent $ninja);$env:PATH"
Import-Module (Join-Path $visualStudio 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $visualStudio -SkipAutomaticLocation `
  -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$logs = Join-Path $root 'out/ci'
$work = Join-Path $root "out/packages/ci-$env:GITHUB_RUN_ID-$env:GITHUB_RUN_ATTEMPT"
if ((Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath $work)) { throw 'Output already exists; inspect it before packaging again.' }
New-Item -ItemType Directory -Path $logs, $work, $output -Force | Out-Null
$step = 0
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
  $script:step++
  & $Program @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $logs "package-$env:GITHUB_RUN_ID-$env:GITHUB_RUN_ATTEMPT-$script:step.log")
  if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE." }
}

Push-Location -LiteralPath $root
try {
  $common = @("-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninja", '-DKR_BUILD_TESTS=OFF',
    '-DKR_ALLOW_UNSIGNED=OFF', "-DKR_PRODUCT_VERSION=$version",
    "-DKR_ZUCCHINI_EXECUTABLE=$browser/KryptonPatch.exe")
  Invoke-Checked cmake (@('--preset', 'windows', '-B', $online, '-DKR_OFFLINE_PAYLOAD=',
    '-DKR_OFFLINE_MANIFEST=', '-DKR_OFFLINE_SIGNATURE=') + $common)
  Invoke-Checked cmake @('--build', $online, '--config', 'Release', '--target',
    'KryptonSetup', 'KryptonWebSetup', 'KryptonUpdater', 'KryptonUninstaller')
  $prerequisites = Join-Path $work 'prerequisites'
  Invoke-Checked pwsh @('-NoProfile', 'tools/prepare-prerequisites.ps1', '-BrowserDir', $browser,
    '-OutputDir', $prerequisites, '-KryptonUiDir', $UiSource, '-UiBinary', "$runtime/krypton_ui.dll")
  Invoke-Checked python @('tools/package-browser.py', 'stage', '--browser-dir', $browser,
    '--work-dir', $work, '--uninstaller', "$online/bin/Release/KryptonUninstaller.exe",
    '--updater', "$online/bin/Release/KryptonUpdater.exe", '--prerequisites-dir', $prerequisites,
    '--key-path', $key)
  $packageJson = Join-Path $work 'package.json'
  $package = Get-Content -LiteralPath $packageJson -Raw | ConvertFrom-Json
  if ($package.browser_version -ne $version) { throw 'Packaged browser version mismatch.' }
  Invoke-Checked cmake (@('--preset', 'windows-offline', '-B', $offline,
    "-DKR_OFFLINE_PAYLOAD=$($package.archive)", "-DKR_OFFLINE_MANIFEST=$($package.manifest)",
    "-DKR_OFFLINE_SIGNATURE=$($package.manifest_signature)") + $common)
  Invoke-Checked cmake @('--build', $offline, '--config', 'Release', '--target', 'KryptonSetup')
  $setup = Join-Path $output "KryptonSetup-$version-offline-x64.exe"
  $web = Join-Path $output "KryptonSetup-$version-web-x64.exe"
  Copy-Item -LiteralPath "$offline/bin/Release/KryptonSetup.exe" -Destination $setup
  Copy-Item -LiteralPath "$online/bin/Release/KryptonWebSetup.exe" -Destination $web
  foreach ($file in @($setup, $web)) {
    Invoke-Checked pwsh @('-NoProfile', 'tools/release-signing.ps1', 'sign', '-InputFile', $file, '-KeyPath', $key)
    Invoke-Checked pwsh @('-NoProfile', 'tools/release-signing.ps1', 'verify', '-InputFile', $file)
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($file))" | Set-Content -LiteralPath "$file.sha256" -Encoding ascii
  }
  Invoke-Checked python @('tools/package-browser.py', 'verify-embedded', '--installer', $setup, '--package-json', $packageJson)
  Invoke-Checked python @('tools/package-browser.py', 'verify-web', '--installer', $web)
  foreach ($helper in @('KryptonUpdater.exe', 'KryptonUninstaller.exe')) {
    Copy-Item -LiteralPath "$online/bin/Release/$helper" -Destination (Join-Path $output $helper)
    Copy-Item -LiteralPath "$online/bin/Release/$helper.sig" -Destination (Join-Path $output "$helper.sig")
  }
  Invoke-Checked python @('tools/prepare-release.py', '--package-json', $packageJson,
    '--output-dir', "$output/github", '--key-path', $key, '--zucchini', "$browser/KryptonPatch.exe")
  [ordered]@{ version = $version; browser_commit = $record.commit; ui_commit = $record.ui_commit;
    source_run_id = $record.run_id; updater_commit = (& git rev-parse HEAD) } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'build-info.json') -Encoding utf8NoBOM
}
finally { Pop-Location }
