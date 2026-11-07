# Synthetic notice bytes exercise index validation only, never package staging.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$TestWorkDirectory)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../cmake/SelectedDependencyPackage.ps1')
$root = Join-Path ([IO.Path]::GetFullPath($TestWorkDirectory)) ([guid]::NewGuid().ToString('N'))
$versions = @{
    'fmt' = '12.2.0'; 'spdlog' = '1.17.0'; 'glfw3' = '3.5.1'
    'imgui' = '1.93.0'; 'materialx' = '1.39.5'; 'miniz' = '3.1.2'
    'stb' = 'snapshot-4e9b368bfc071e97206ea5c6adba68dd0cbf8897'
    'entt' = '4.0.0'; 'nlohmann-json' = '3.12.0'; 'tinygltf' = '3.1.0'; 'tinyexr' = '3.4.0'
}
$checks = 0

function Test-Index([string]$Name, [scriptblock]$Mutate, [switch]$Accept) {
    $directory = Join-Path $root $Name
    $notices = (New-Item -ItemType Directory -Path (Join-Path $directory 'third_party_licenses') -Force).FullName
    $rows = @($versions.Keys | Sort-Object | ForEach-Object {
        $filename = "$_-test.txt"
        $path = Join-Path $notices $filename
        [IO.File]::WriteAllText($path, "synthetic index test: $_")
        [ordered]@{ name = $_; version = $versions[$_]; notice = $filename
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $document = @{ libraries = $rows }
    & $Mutate $document $notices
    $indexPath = Join-Path $notices 'selected-dependencies.json'
    $document | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $indexPath -Encoding UTF8
    if ($Name -eq 'missing-index') { Remove-Item -LiteralPath $indexPath }
    $failure = $null
    try {
        $actual = @(Read-ContainerSelectedDependencies -BuildDirectory $directory)
        if ($actual.Count -ne 11) { throw 'Validated rows were lost.' }
        if ($Accept -and @($actual | Where-Object name -eq 'tinygltf')[0].interface -ne 'deprecated C++ compatibility API') {
            throw 'The interface declaration was lost.'
        }
    } catch { $failure = $_.Exception.Message }
    if (($Accept -and $failure) -or (-not $Accept -and -not $failure)) {
        throw "Index case '$Name' returned the wrong outcome: $failure"
    }
    $script:checks++
}

Test-Index 'valid' { param($d, $n) ($d.libraries | Where-Object name -eq 'tinygltf').interface = 'deprecated C++ compatibility API' } -Accept
Test-Index 'stale-version' { param($d, $n) $d.libraries[0].version = 'old' }
Test-Index 'missing-library' { param($d, $n) $d.libraries = @($d.libraries | Select-Object -Skip 1) }
Test-Index 'duplicate-library' { param($d, $n) $d.libraries[1].name = $d.libraries[0].name }
Test-Index 'duplicate-notice' { param($d, $n) $d.libraries[1].notice = $d.libraries[0].notice; $d.libraries[1].sha256 = $d.libraries[0].sha256 }
Test-Index 'parent-path' { param($d, $n) $d.libraries[0].notice = '../notice.txt' }
Test-Index 'backslash-path' { param($d, $n) $d.libraries[0].notice = 'nested\notice.txt' }
Test-Index 'absolute-path' { param($d, $n) $d.libraries[0].notice = 'C:\notice.txt' }
Test-Index 'missing-notice' { param($d, $n) Remove-Item -LiteralPath (Join-Path $n $d.libraries[0].notice) }
Test-Index 'changed-notice' { param($d, $n) [IO.File]::WriteAllText((Join-Path $n $d.libraries[0].notice), 'changed') }
Test-Index 'wrong-hash' { param($d, $n) $d.libraries[0].sha256 = '0' * 64 }
Test-Index 'malformed-hash' { param($d, $n) $d.libraries[0].sha256 = 'not-a-digest' }
Test-Index 'missing-index' { param($d, $n) }
Write-Output "PASS $checks selected-dependency package index cases"
