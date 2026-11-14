# Exercise package preflight only; no executable, SDK source or release is staged.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$TestWorkDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$packageScript = (Resolve-Path (Join-Path $PSScriptRoot '../../cmake/package_windows.ps1')).Path
$workDirectory = Join-Path ([IO.Path]::GetFullPath($TestWorkDirectory)) ([guid]::NewGuid().ToString('N'))
$runtimeDirectory = Join-Path $workDirectory 'runtime'
$dependencyDirectory = Join-Path $workDirectory 'dependencies'
$outputDirectory = Join-Path $workDirectory 'must-not-stage'
New-Item -ItemType Directory -Path @($runtimeDirectory,
    (Join-Path $dependencyDirectory 'x64-windows/share')) -Force | Out-Null
$caseCount = 0

function Assert-Preflight([string[]]$CacheLines, [string]$ExpectedError, [switch]$InvalidSource) {
    $CacheLines | Set-Content -LiteralPath (Join-Path $workDirectory 'CMakeCache.txt') -Encoding UTF8
    $arguments = @{
        Version = '0.0.0-test'
        RuntimeDirectory = $runtimeDirectory
        VcpkgInstalledDirectory = $dependencyDirectory
        BuildDirectory = $workDirectory
        OutputDirectory = $outputDirectory
    }
    if ($InvalidSource) {
        $arguments.TinyUsdzSourceDirectory = Join-Path $workDirectory 'missing-tinyusdz-source'
    }
    $failure = $null
    try { & $packageScript @arguments | Out-Null } catch { $failure = $_.Exception.Message }
    if ($failure -ne $ExpectedError) {
        throw "Expected '$ExpectedError', received '$failure'. Cache: $($CacheLines -join ', ')"
    }
    if (Test-Path -LiteralPath $outputDirectory) {
        throw 'A preflight rejection must not create staging or archive output.'
    }
    $script:caseCount++
}

$missingOption = 'The build does not record ENABLE_TINYUSDZ_USD_LOADER. Reconfigure and rebuild before packaging.'
Assert-Preflight @('# no TinyUSDZ option') $missingOption
Assert-Preflight @('ENABLE_TINYUSDZ_USD_LOADER:BOOL=OFF',
    'ENABLE_TINYUSDZ_USD_LOADER:BOOL=ON') $missingOption

# Reaching the next mandatory cache check proves TinyUSDZ's disabled path did
# not prompt for, resolve, or read a source directory. Invalid stale paths are
# harmless too, and all CMake false constants have the same behavior.
$nextOption = 'The build does not record ENABLE_PHYSICS. Reconfigure and rebuild before packaging.'
foreach ($value in @('', '0', 'OFF', 'No', 'false', 'N', 'IGNORE', 'NOTFOUND', 'source-NOTFOUND')) {
    Assert-Preflight @("ENABLE_TINYUSDZ_USD_LOADER:BOOL=$value") $nextOption
}
Assert-Preflight @('ENABLE_TINYUSDZ_USD_LOADER:BOOL=OFF') $nextOption -InvalidSource

$enabledError = 'The TinyUSDZ-enabled build requires -TinyUsdzSourceDirectory for its upstream and bundled notices.'
foreach ($value in @('ON', 'Y', '2')) {
    Assert-Preflight @("ENABLE_TINYUSDZ_USD_LOADER:BOOL=$value") $enabledError
}

$minimalCache = @('ENABLE_TINYUSDZ_USD_LOADER:BOOL=OFF', 'ENABLE_PHYSICS:BOOL=OFF')
$shaderDirectory = Join-Path $runtimeDirectory 'spv_shaders'
New-Item -ItemType Directory -Path $shaderDirectory | Out-Null

& {
    # Exercise the runtime-file gate independently of the checkout's state.
    function git {
        $global:LASTEXITCODE = 0
        switch ($args -join ' ') {
            'rev-parse HEAD' { '0000000000000000000000000000000000000000' }
            'status --porcelain' { }
            default { throw "Unexpected git invocation: $args" }
        }
    }

    foreach ($shader in @('brdf_lut.comp.spv', 'equirect_to_cubemap.comp.spv',
            'irradiance_convolution.comp.spv', 'prefilter_specular.comp.spv')) {
        Assert-Preflight $minimalCache "Missing runtime file: spv_shaders/$shader"
        # These files are sentinels. The preflight never loads shader binaries.
        New-Item -ItemType File -Path (Join-Path $shaderDirectory $shader) | Out-Null
    }
    Assert-Preflight $minimalCache 'Missing runtime file: VulkanSceneRenderer.exe'
}

"PASS $caseCount Windows packaging preflight cases"
