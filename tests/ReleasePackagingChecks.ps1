[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageScript)
$ErrorActionPreference = 'Stop'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('ddc-package-check-' + [Guid]::NewGuid().ToString('N'))
$fixture = Join-Path $testRoot 'source with spaces'
$fixtureTools = Join-Path $fixture 'tools'
$build = Join-Path $fixture 'build'
New-Item -ItemType Directory -Path $fixtureTools, $build | Out-Null
Copy-Item -LiteralPath $PackageScript -Destination (Join-Path $fixtureTools 'package-release.ps1')
# The intentional version mismatch is a sentinel past the build preflight. No
# fixture ever reaches the real build, installation, or source snapshot steps.
Set-Content -LiteralPath (Join-Path $fixture 'CMakeLists.txt') -Value 'project(DietDrCamera VERSION 1.0.0 LANGUAGES CXX)'
Set-Content -LiteralPath (Join-Path $fixture 'vcpkg.json') -Value '{"version-string":"2.0.0"}'
function Check-Preflight([string]$Source, [string]$Deploy, [string]$Checks, [string]$Expected) {
    $cache = "CMAKE_HOME_DIRECTORY:INTERNAL=$Source`nDDC_DEPLOY_TO_MO2:BOOL=$Deploy`nDDC_BUILD_CHECKS:BOOL=$Checks`n"
    Set-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Value $cache
    $failure = $null
    try { & (Join-Path $fixtureTools 'package-release.ps1') -BuildDirectory 'build' }
    catch { $failure = $_.Exception.Message }
    if (-not $failure -or -not $failure.Contains($Expected)) { throw "Unexpected preflight result: $failure" }
    if (Test-Path -LiteralPath (Join-Path $fixture 'release')) { throw 'Rejected build created a release candidate.' }
}
try {
    Check-Preflight (Join-Path $testRoot 'another checkout') OFF ON 'different source tree'
    Check-Preflight '' OFF ON 'different source tree'
    Check-Preflight $fixture ON ON 'DDC_DEPLOY_TO_MO2=OFF'
    Check-Preflight $fixture OFF OFF 'DDC_BUILD_CHECKS=ON'
    Check-Preflight $fixture.Replace('\', '/').ToUpperInvariant() OFF ON 'vcpkg manifest version is out of sync'
    Write-Output 'Release packaging preflight checks passed'
} finally {
    # Delete only the unique fixture directory this test created.
    $resolved = [IO.Path]::GetFullPath($testRoot)
    $tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempParent, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^ddc-package-check-[0-9a-f]{32}$') { throw 'Unsafe test cleanup path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
