[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$BrowserDir,
  [Parameter(Mandatory)][string]$OutputDir,
  [string]$KryptonUiDir = (Join-Path $PSScriptRoot '../../KryptonUI'),
  [string]$UiBinary
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$browser = (Resolve-Path -LiteralPath $BrowserDir).Path
$ui = (Resolve-Path -LiteralPath $KryptonUiDir).Path
$pins = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'prerequisites.json') -Raw | ConvertFrom-Json
$runtimeSource = Get-Content -LiteralPath (Join-Path $ui 'cmake/WindowsAppSDK.cmake') -Raw
if ($runtimeSource -notmatch 'set\(KR_WINAPPSDK_RUNTIME_MAJOR_MINOR\s+(0x[0-9a-fA-F]+)\)') {
  throw 'The KryptonUI Windows App Runtime release could not be determined.'
}
$release = [Convert]::ToUInt32($Matches[1].Substring(2), 16)
if ($release -ne $pins.windows_app_runtime.release) {
  throw 'The bundled Windows App Runtime does not match KryptonUI. Update tools/prerequisites.json.'
}
$module = Join-Path $browser 'krypton_ui.dll'
$builtModule = if ($UiBinary) { (Resolve-Path -LiteralPath $UiBinary).Path }
  else { Join-Path $ui 'out/build/windows/bin/Release/krypton_ui.dll' }
if ((Get-FileHash -LiteralPath $module -Algorithm SHA256).Hash -ne
    (Get-FileHash -LiteralPath $builtModule -Algorithm SHA256).Hash) {
  throw 'The browser UI module differs from the Release build used to determine prerequisites.'
}

$minimum = [version]'0.0.0.0'
foreach ($filename in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
  $info = (Get-Item -LiteralPath (Join-Path $browser $filename)).VersionInfo
  $version = [version]::new($info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart)
  if ($version -gt $minimum) { $minimum = $version }
}
if ([version]$pins.visual_cpp.version -lt $minimum) {
  throw "The bundled Visual C++ Redistributable is older than the browser requires ($minimum). Update tools/prerequisites.json."
}

$cache = Join-Path $PSScriptRoot '../.deps/prerequisites'
New-Item -ItemType Directory -Path $cache, $OutputDir -Force | Out-Null
foreach ($pin in $pins.visual_cpp, $pins.windows_app_runtime) {
  $cached = Join-Path $cache $pin.filename
  if (-not (Test-Path -LiteralPath $cached)) {
    $download = "$cached.download"
    Invoke-WebRequest -Uri $pin.url -OutFile $download
    if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne $pin.sha256) {
      throw "Downloaded prerequisite has an unexpected SHA-256: $($pin.filename)"
    }
    Move-Item -LiteralPath $download -Destination $cached
  }
  if ((Get-FileHash -LiteralPath $cached -Algorithm SHA256).Hash -ne $pin.sha256) {
    throw "Cached prerequisite has an unexpected SHA-256: $($pin.filename)"
  }
  $signature = Get-AuthenticodeSignature -FilePath $cached
  if ($signature.Status -ne 'Valid' -or
      $signature.SignerCertificate.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false) -ne 'Microsoft Corporation') {
    throw "Prerequisite is not signed by Microsoft: $($pin.filename)"
  }
  Copy-Item -LiteralPath $cached -Destination (Join-Path $OutputDir $pin.filename) -Force
}

@"
[WindowsAppRuntime]
MajorMinor=$release
MinimumVersion=$($pins.windows_app_runtime.version)
[VisualCpp]
MinimumVersion=$minimum
"@ | Set-Content -LiteralPath (Join-Path $OutputDir 'requirements.manifest') -Encoding ascii
Write-Host "Verified offline prerequisites: Windows App Runtime $($pins.windows_app_runtime.version); Visual C++ x64 $minimum"
