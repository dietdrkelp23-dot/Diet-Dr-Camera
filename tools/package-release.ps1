[CmdletBinding()]
param([string]$BuildDirectory = 'build/release-candidate')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = [IO.Path]::GetFullPath((Join-Path $repo $BuildDirectory))
$cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Raw
$configuredSource = [regex]::Match($cache, '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)\r?$').Groups[1].Value.Trim()
if (-not $configuredSource -or
    -not [IO.Path]::GetFullPath($configuredSource).Equals([IO.Path]::GetFullPath($repo), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Build directory belongs to a different source tree. Configure this checkout before packaging.'
}
if ($cache -notmatch '(?m)^DDC_DEPLOY_TO_MO2:BOOL=OFF\r?$' -or
    $cache -notmatch '(?m)^DDC_BUILD_CHECKS:BOOL=ON\r?$') {
    throw 'Configure with DDC_DEPLOY_TO_MO2=OFF and DDC_BUILD_CHECKS=ON first (see README.md).'
}
$cmakeText = Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Raw
$version = [regex]::Match($cmakeText, '(?s)project\(\s*DietDrCamera\s+VERSION\s+(\d+\.\d+\.\d+)').Groups[1].Value
if (-not $version) { throw 'Could not read the project version.' }
$vcpkg = Get-Content -LiteralPath (Join-Path $repo 'vcpkg.json') -Raw | ConvertFrom-Json
if ($vcpkg.'version-string' -ne $version) { throw 'vcpkg manifest version is out of sync.' }
foreach ($document in @('RELEASE-NOTES.md', 'Diet Dr Camera - Requirements and Credits.txt')) {
    $firstLine = Get-Content -LiteralPath (Join-Path $repo $document) -TotalCount 1
    if (-not $firstLine.Contains($version)) { throw "Release text version is out of sync: $document" }
}
$output = Join-Path $repo ('release/candidate-' + $version + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
if (Test-Path -LiteralPath $output) { throw 'Candidate output already exists.' }
$stage = Join-Path $output 'stage'
New-Item -ItemType Directory -Path $stage | Out-Null

function Invoke-Checked([string]$Program, [string[]]$Arguments, [string]$Log) {
    # Windows PowerShell 5 turns ordinary native stderr (including CMake status
    # messages) into ErrorRecords. Judge native tools by their exit code while
    # preserving both streams in the log; a status message is not a build failure.
    $application = (Get-Command $Program -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $application @Arguments 2>&1 | ForEach-Object { $_.ToString() } | Tee-Object -FilePath $Log
        $exitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    if ($exitCode -ne 0) { throw "$Program failed with exit code $exitCode (see $Log)." }
}
function Get-Records([string]$Root) {
    @(Get-ChildItem -LiteralPath $Root -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = $_.FullName.Substring($Root.Length + 1).Replace('\', '/');
                    bytes = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
}
function New-VerifiedZip([string]$Root, [string]$Destination, $Records) {
    $zip = [IO.Compression.ZipFile]::Open($Destination, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($record in $Records) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $Root $record.path),
                $record.path, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $zip.Dispose() }
    $zip = [IO.Compression.ZipFile]::OpenRead($Destination)
    try {
        if ($zip.Entries.Count -ne $Records.Count) { throw 'ZIP entry count mismatch.' }
        foreach ($record in $Records) {
            $entry = $zip.GetEntry($record.path)
            if (-not $entry -or $entry.Length -ne $record.bytes) { throw "ZIP entry mismatch: $($record.path)" }
            $stream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
            finally { $sha.Dispose(); $stream.Dispose() }
            if ($hash -ne $record.sha256) { throw "ZIP hash mismatch: $($record.path)" }
        }
    } finally { $zip.Dispose() }
}

Push-Location $repo
try {
    # Snapshot the working source, including new files, using an explicit allowlist.
    $roots = @('src', 'include', 'cmake', 'tests', 'tools', 'dist', 'licenses', 'nexus', 'extern/CommonLibSSE-NG', 'extern/minhook')
    $docs = @('CMakeLists.txt', 'vcpkg.json', 'README.md', 'BUILDING.md', 'RUNTIME-TESTING.md', 'SOURCE-PROVENANCE.md', 'LICENSE.txt', 'LICENSING.md', 'EXCEPTIONS.md', 'PRESET-COMPAT.md', 'PRESET-AUTHORS.md', 'RELEASE-NOTES.md', 'RUNTIME-SUPPORT.md',
              'RELEASE-CHECKLIST.md', 'RUNTIME-MATRIX.md', 'RUNTIME-COMPATIBILITY-AUDIT.md',
              'SUPPORT-LOGGING.md', 'DAMAGE-REACTION-COVERAGE.md', 'CINEMATIC-VIEWS.md',
              'REQUIREMENTS.txt', 'CREDITS.txt', 'THIRD-PARTY-NOTICES.txt',
              'Diet Dr Camera - Requirements and Credits.txt')
    $hasGit = (Test-Path -LiteralPath (Join-Path $repo '.git')) -and ($null -ne (Get-Command git -ErrorAction SilentlyContinue))
    $hasVendorGit = $hasGit -and (Test-Path -LiteralPath (Join-Path $repo 'extern/CommonLibSSE-NG/.git'))
    if ($hasGit) {
        $paths = @(& git -c core.quotepath=false ls-files --cached --others --exclude-standard -- @roots @docs |
            Sort-Object -Unique | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
        if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate the working source.' }
        # Development uses a gitlink; the public repository tracks the vendor
        # files directly. Never mistake the parent repository for vendor Git.
        if ($hasVendorGit) {
            $vendorPaths = @(& git -C extern/CommonLibSSE-NG -c core.quotepath=false ls-files --cached --others --exclude-standard)
            if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate bundled CommonLib source.' }
            $paths += @($vendorPaths | ForEach-Object { 'extern/CommonLibSSE-NG/' + $_ } |
                Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
        }
        $paths = @($paths | Sort-Object -Unique)
    } else {
        # The distributed source ZIP has no Git metadata; it can still rebuild/package.
        $paths = @($docs) + @(foreach ($root in $roots) {
            Get-ChildItem -LiteralPath (Join-Path $repo $root) -Recurse -File | ForEach-Object {
                $_.FullName.Substring($repo.Length + 1).Replace('\', '/')
            }
        })
        $paths = @($paths | Sort-Object -Unique)
    }
    # Running source tools can create Python bytecode beside their scripts.
    # Omit caches in both a working checkout and an extracted source release.
    $paths = @($paths | Where-Object { $_ -notmatch '(?i)(^|/)__pycache__/|\.py[co]$' })
    if ($paths.Count -lt 100) { throw 'Source inventory is incomplete.' }
    if ($paths | Where-Object { $_ -match '(?i)(^|/)(\.git|\.codex|\.agents|build|out|vcpkg_installed)/|\.(exe|dll|pdb|obj|lib|zip|log)$' }) {
        throw 'Source inventory contains build artifacts or private tool state.'
    }
    $source = Join-Path $output 'source'
    foreach ($path in $paths) {
        $destination = Join-Path $source $path
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $path -Destination $destination
    }
    $workingSourceRecords = Get-Records $source
    $installed = [regex]::Match($cache, '(?m)^VCPKG_INSTALLED_DIR:PATH=(.+)\r?$').Groups[1].Value.Trim()
    $toolchain = [regex]::Match($cache, '(?m)^CMAKE_TOOLCHAIN_FILE:FILEPATH=(.+)\r?$').Groups[1].Value.Trim()
    if (-not $installed -or -not $toolchain) { throw 'vcpkg source locations are missing from the build cache.' }
    $vcpkgRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $toolchain))
    Invoke-Checked 'python' @((Join-Path $PSScriptRoot 'package-dependency-sources.py'),
        '--installed', $installed, '--vcpkg', $vcpkgRoot, '--output', (Join-Path $source 'dependency-sources')) (Join-Path $output 'dependency-sources.log')
    $sourceRecords = Get-Records $source
    Invoke-Checked 'cmake' @('--build', $build, '--config', 'Release', '--parallel', '4') (Join-Path $output 'build.log')
    Invoke-Checked 'ctest' @('--test-dir', $build, '-C', 'Release', '--output-on-failure', '--no-tests=error') (Join-Path $output 'checks.log')
    Invoke-Checked 'cmake' @('--install', $build, '--config', 'Release', '--component', 'DDC', '--prefix', $stage) (Join-Path $output 'install.log')
    $symbols = Join-Path $output 'symbols'
    Invoke-Checked 'cmake' @('--install', $build, '--config', 'Release', '--component', 'DDCSymbols', '--prefix', $symbols) (Join-Path $output 'symbols-install.log')
    foreach ($record in $workingSourceRecords) {
        if ((Get-FileHash -LiteralPath (Join-Path $repo $record.path) -Algorithm SHA256).Hash -ne $record.sha256) {
            throw "Source changed during build: $($record.path). Run packaging again."
        }
    }
    $binaryCheck = & (Join-Path $PSScriptRoot 'verify-release-binary.ps1') -Dll (Join-Path $stage 'SKSE/Plugins/DietDrCamera.dll') -Pdb (Join-Path $symbols 'DietDrCamera.pdb') -Version $version
    $binaryCheck | Set-Content -LiteralPath (Join-Path $output 'binary-check.txt') -Encoding UTF8
    $records = Get-Records $stage
    $expected = @('SKSE/Plugins/DietDrCamera.dll',
                  'meshes/actors/character/animations/staggercamera.hkx')
    foreach ($path in $expected) {
        if ($records.path -notcontains $path) { throw "Required release file missing: $path" }
    }
    foreach ($record in $records) {
        if ($expected -notcontains $record.path) {
            throw "Unexpected release payload: $($record.path)"
        }
    }
    # Verify notices in the passive PE resource without loading/executing the
    # plugin. All original license text must survive verbatim except newlines.
    $licenseBundle = [DdcRelease.BinaryCheck]::ReadLicenseNotices((Join-Path $stage 'SKSE/Plugins/DietDrCamera.dll')).Replace("`r`n", "`n")
    $licenseInputs = @('LICENSING.md', 'LICENSE.txt', 'EXCEPTIONS.md', 'THIRD-PARTY-NOTICES.txt') +
        @(Get-ChildItem -LiteralPath (Join-Path $repo 'licenses') -File | Where-Object Extension -In '.txt', '.md' | ForEach-Object FullName)
    foreach ($licenseInput in $licenseInputs) {
        $notice = [IO.File]::ReadAllText([IO.Path]::GetFullPath($licenseInput)).Replace("`r`n", "`n")
        if (-not $licenseBundle.Contains($notice)) { throw "Embedded license notice missing or changed: $licenseInput" }
    }
    foreach ($name in @('DietDrCamera.dll', 'DietDrCamera.pdb')) {
        $packagedBinary = if ($name.EndsWith('.pdb')) { Join-Path $symbols $name } else { Join-Path $stage "SKSE/Plugins/$name" }
        if ((Get-FileHash -LiteralPath (Join-Path $build "Release/$name")).Hash -ne
            (Get-FileHash -LiteralPath $packagedBinary).Hash) {
            throw "Staged binary differs from build: $name"
        }
    }
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $mainZip = Join-Path $output "Diet Dr Camera-$version.zip"
    $sourceZip = Join-Path $output "Diet Dr Camera - Source-$version.zip"
    New-VerifiedZip $stage $mainZip $records
    New-VerifiedZip $source $sourceZip $sourceRecords
    $revision = $null
    $vendorRevision = $null
    $vendorStatus = @()
    $status = @()
    if ($hasGit) {
        $revision = & git rev-parse HEAD
        if ($LASTEXITCODE -ne 0) { throw 'Could not record Git revision.' }
        $status = @(& git status --short -- @roots @docs)
        if ($LASTEXITCODE -ne 0) { throw 'Could not record working tree changes.' }
    }
    if ($hasVendorGit) {
        $vendorRevision = & git -C extern/CommonLibSSE-NG rev-parse HEAD
        if ($LASTEXITCODE -ne 0) { throw 'Could not record CommonLib revision.' }
        $vendorStatus = @(& git -C extern/CommonLibSSE-NG status --short)
        if ($LASTEXITCODE -ne 0) { throw 'Could not record CommonLib changes.' }
    }
    $manifest = [ordered]@{
        version = $version; status = 'automated release checks passed - see RELEASE-CHECKLIST.md for gameplay acceptance';
        license = 'GPL-3.0-or-later with modding/linking exceptions; see LICENSING.md';
        createdUtc = [DateTime]::UtcNow.ToString('o'); baseCommit = $revision;
        sourceIncludesWorkingChanges = ($status.Count -gt 0); workingChanges = $status;
        commonLibCommit = $vendorRevision; commonLibWorkingChanges = $vendorStatus;
        commonLibSourceMode = $(if ($hasVendorGit) { 'vendor-git' } else { 'bundled-files; see sourceFiles hashes and documented pin' });
        binaryCheck = $binaryCheck; files = $records; sourceFiles = $sourceRecords;
        archives = @($mainZip, $sourceZip | ForEach-Object {
            [ordered]@{ name = [IO.Path]::GetFileName($_); sha256 = (Get-FileHash -LiteralPath $_).Hash }
        })
    }
    $manifestPath = Join-Path $output 'manifest.json'
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    @($mainZip, $sourceZip, $manifestPath | ForEach-Object {
        (Get-FileHash -LiteralPath $_).Hash + '  ' + [IO.Path]::GetFileName($_)
    }) | Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ASCII
    Copy-Item -LiteralPath RELEASE-CHECKLIST.md -Destination $output
    Write-Host "Verified release candidate: $output"
    Write-Host 'See RELEASE-CHECKLIST.md for recorded gameplay acceptance and remaining coverage.'
} finally { Pop-Location }
