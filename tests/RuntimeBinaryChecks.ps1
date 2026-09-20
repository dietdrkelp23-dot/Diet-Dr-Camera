[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Dll,
    [Parameter(Mandatory)][string]$Pdb,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$Verifier
)
$ErrorActionPreference = 'Stop'
& $Verifier -Dll $Dll -Pdb $Pdb -Version $Version | Out-Null
$original = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Dll).Path)
$symbols = [Text.Encoding]::ASCII.GetString($original)
$pe = [BitConverter]::ToInt32($original, 0x3C)
$sectionCount = [BitConverter]::ToUInt16($original, $pe + 6)
$sections = $pe + 24 + [BitConverter]::ToUInt16($original, $pe + 20)
function Convert-Rva([uint32]$Rva) {
    for ($index = 0; $index -lt $sectionCount; ++$index) {
        $section = $sections + 40 * $index
        $start = [BitConverter]::ToUInt32($original, $section + 12)
        $size = [Math]::Max([BitConverter]::ToUInt32($original, $section + 8),
                          [BitConverter]::ToUInt32($original, $section + 16))
        if ($Rva -ge $start -and $Rva - $start -lt $size) {
            return [int]([BitConverter]::ToUInt32($original, $section + 20) + $Rva - $start)
        }
    }
    throw 'Export address is outside the PE sections'
}
$exportTable = Convert-Rva ([BitConverter]::ToUInt32($original, $pe + 24 + 112))
$names = Convert-Rva ([BitConverter]::ToUInt32($original, $exportTable + 32))
$exports = @{}
for ($index = 0; $index -lt [BitConverter]::ToUInt32($original, $exportTable + 24); ++$index) {
    $position = Convert-Rva ([BitConverter]::ToUInt32($original, $names + 4 * $index))
    $name = $symbols.Substring($position, $symbols.IndexOf([char]0, $position) - $position)
    $exports[$name] = $position
}
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('ddc-runtime-binary-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $mutant = Join-Path $testRoot 'DietDrCamera.dll'
    foreach ($symbol in @('SKSEPlugin_Query', 'SKSEPlugin_Load', 'SKSEPlugin_Version')) {
        if (-not $exports.ContainsKey($symbol)) { throw "Fixture lacks $symbol" }
        $position = $exports[$symbol]
        $bytes = $original.Clone()
        $bytes[$position] = [byte][char]'X'
        [IO.File]::WriteAllBytes($mutant, $bytes)
        $failure = $null
        try { & $Verifier -Dll $mutant -Pdb $Pdb -Version $Version | Out-Null }
        catch { $failure = $_.Exception.Message }
        if (-not $failure -or -not $failure.Contains('Required SKSE exports are missing')) {
            throw "Missing $symbol was not rejected by the binary verifier: $failure"
        }
    }
    Write-Output 'SE Query, AE Version and shared Load export regression checks passed'
} finally {
    $resolved = [IO.Path]::GetFullPath($testRoot)
    $tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempParent, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^ddc-runtime-binary-[0-9a-f]{32}$') { throw 'Unsafe test cleanup path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
