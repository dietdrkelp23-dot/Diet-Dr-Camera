[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$RuntimeImage,
    [Parameter(Mandatory=$true)][string]$AddressLibrary,
    [string]$Checker = 'build/release-candidate/Release/RuntimeLayoutChecks.exe',
    [string]$ReportDirectory = 'build/diagnostics/runtime-tests'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$imagePath = (Resolve-Path -LiteralPath $RuntimeImage).Path
$libraryPath = (Resolve-Path -LiteralPath $AddressLibrary).Path
$checkerPath = (Resolve-Path -LiteralPath $Checker).Path
$reportRoot = [IO.Path]::GetFullPath($ReportDirectory)
New-Item -ItemType Directory -Path $reportRoot -Force | Out-Null
$version = [Diagnostics.FileVersionInfo]::GetVersionInfo($imagePath).ProductVersion
if ($version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw 'No recognized runtime version in the executable.' }
$name = 'runtime-' + $version + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
$log = Join-Path $reportRoot ($name + '.log')
$previousPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    & $checkerPath --image $imagePath $libraryPath 2>&1 | ForEach-Object { $_.ToString() } | Tee-Object -FilePath $log
    $checkerExit = $LASTEXITCODE
} finally { $ErrorActionPreference = $previousPreference }
$passed = $checkerExit -eq 0 -and (Select-String -LiteralPath $log -SimpleMatch 'Complete hook preflight passed' -Quiet)
$record = [ordered]@{
    testedAtUtc = [DateTime]::UtcNow.ToString('o')
    runtime = $version
    result = $(if ($passed) { 'preflight-pass' } else { 'preflight-fail' })
    gameplay = 'not-run'
    exitCode = $checkerExit
    image = [IO.Path]::GetFileName($imagePath)
    imageSha256 = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash
    addressLibrary = [IO.Path]::GetFileName($libraryPath)
    addressLibrarySha256 = (Get-FileHash -LiteralPath $libraryPath -Algorithm SHA256).Hash
    checkerSha256 = (Get-FileHash -LiteralPath $checkerPath -Algorithm SHA256).Hash
    log = [IO.Path]::GetFileName($log)
}
$record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $reportRoot ($name + '.json')) -Encoding UTF8
if (-not $passed) { throw "Runtime preflight failed; see $log" }
Write-Output "Saved offline preflight evidence to $reportRoot. Gameplay was not tested."
