[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ObjectDirectory,
    [Parameter(Mandatory)][string]$Linker
)
$ErrorActionPreference = 'Stop'

# Check the compiled production callbacks, not a separate mock implementation.
# Native SE/AE slot BD callers test/dereference the returned ImpactData pointer.
# The old objects reproduce three void callbacks; BeamImpact already matched.
$expected = @{
    ArcheryHitShakeController = @('AddImpact')
    MagicHitShakeController = @('AddImpact', 'ConeImpact', 'BeamImpact')
}
$checked = 0
foreach ($module in $expected.Keys) {
    $objects = @(Get-ChildItem -LiteralPath $ObjectDirectory -Recurse -File |
        Where-Object { $_.Name -match ('^' + $module + '(\.cpp)?\.obj$') })
    if ($objects.Count -ne 1) { throw "Expected one compiled $module object in $ObjectDirectory" }
    # This check also runs from a GUI-hosted agent with no inherited console.
    # PowerShell's native invocation can create a visible console in that case.
    # Start the linker explicitly without a window and drain both pipes together.
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Linker
    $startInfo.Arguments = '/DUMP /SYMBOLS "' + $objects[0].FullName + '"'
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $symbolProcess = [System.Diagnostics.Process]::Start($startInfo)
    try {
        $stdout = $symbolProcess.StandardOutput.ReadToEndAsync()
        $stderr = $symbolProcess.StandardError.ReadToEndAsync()
        $symbolProcess.WaitForExit()
        $symbols = $stdout.GetAwaiter().GetResult() -split '\r?\n'
        $errorText = $stderr.GetAwaiter().GetResult()
        if ($symbolProcess.ExitCode -ne 0) { throw "Cannot inspect $module object symbols: $errorText" }
    } finally {
        $symbolProcess.Dispose()
    }
    foreach ($callback in $expected[$module]) {
        $callbacks = @($symbols | Where-Object {
            $_ -match ('notype \(\).*\| \?' + $callback + '@.*@' + $module + '@DietDrCamera@@')
        })
        if ($callbacks.Count -ne 1 -or $callbacks[0] -notmatch '\(struct RE::Projectile::ImpactData \* __cdecl') {
            throw "${module}::$callback does not return the engine's ImpactData pointer"
        }
        ++$checked
    }
}
Write-Output "$checked compiled production projectile impact callbacks preserve the native return-type contract"
