[CmdletBinding()]
param([string]$KryptonRoot = (Join-Path $PSScriptRoot '../../Krypton'))
$ErrorActionPreference = 'Stop'
$browser = (Resolve-Path -LiteralPath $KryptonRoot).Path
$graph = Join-Path $browser 'build/src/out/Default'
foreach ($name in @('build.ninja', '.ninja_log', '.ninja_deps')) {
  if (-not (Test-Path -LiteralPath (Join-Path $graph $name))) {
    throw "The existing browser build graph is missing $name. Build the browser first."
  }
}
$ninja = (Get-Command ninja.exe -ErrorAction Stop).Source
$version = (& $ninja --version).Trim()
$header = Get-Content -LiteralPath (Join-Path $graph '.ninja_log') -TotalCount 1
if ($header -ne '# ninja log v7' -or $version -ne '1.13.2') {
  throw "Ninja/log compatibility requires review: $ninja $version; $header"
}
$owned = Join-Path $browser 'chromium_src/chrome/browser/ui/krypton_ui/BUILD.gn'
$prepared = Join-Path $browser 'build/src/chrome/browser/ui/krypton_ui/BUILD.gn'
if ((Get-FileHash -LiteralPath $owned).Hash -ne (Get-FileHash -LiteralPath $prepared).Hash) {
  throw 'Stage the current Krypton build definitions in the browser checkout first; this helper does not overwrite prepared sources.'
}
$generator = 'chrome/browser/ui/krypton_ui/publisher_delta_main.cc'
if ((Get-FileHash -LiteralPath (Join-Path $browser "chromium_src/$generator")).Hash -ne
    (Get-FileHash -LiteralPath (Join-Path $browser "build/src/$generator")).Hash) {
  throw 'Stage the current publisher generator source in the prepared tree first.'
}
Push-Location -LiteralPath $browser
try {
  $plan = & $ninja -C $graph -n KryptonPatch.exe KryptonDeltaGenerator.exe 2>&1
  if ($LASTEXITCODE -ne 0) { throw ($plan -join "`n") }
  if (@($plan | Where-Object { $_ -match '^\[\d+/\d+\]' }).Count -gt 32) {
    throw 'The patch-helper plan includes unexpectedly broad compilation. Preserve the graph and inspect the plan before proceeding.'
  }
  if ((& $ninja --version).Trim() -ne '1.13.2' -or
      (Get-Content -LiteralPath (Join-Path $graph '.ninja_log') -TotalCount 1) -ne '# ninja log v7') {
    throw 'Ninja/log compatibility changed; no build was started.'
  }
  python -c "import build,os,sys; os.chdir('build/src'); build._run_build_process(sys.argv[1],'-C','out/Default','KryptonPatch.exe','KryptonDeltaGenerator.exe')" $ninja
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
  Pop-Location
}
