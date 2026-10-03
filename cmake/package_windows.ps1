# Package an already-built Windows x64 Release executable and its runtime assets.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v?[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?$')]
    [string]$Version,
    [Parameter(Mandatory = $true)][string]$RuntimeDirectory,
    [Parameter(Mandatory = $true)][string]$VcpkgInstalledDirectory,
    [Parameter(Mandatory = $true)][string]$TinyUsdzSourceDirectory,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../out/packages')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$sourceDirectory = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$runtimeDirectory = (Resolve-Path $RuntimeDirectory).Path
$dependencyDirectory = (Resolve-Path (Join-Path $VcpkgInstalledDirectory 'x64-windows/share')).Path
$tinyDirectory = (Resolve-Path $TinyUsdzSourceDirectory).Path
$outputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$packageName = "VulkanSceneRenderer-$Version-windows-x64"
$stagingDirectory = Join-Path $outputDirectory $packageName
$archivePath = Join-Path $outputDirectory "$packageName.zip"

Push-Location $sourceDirectory
try {
    $sourceCommit = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot determine the source commit.' }
    $sourceStatus = & git status --porcelain
    if ($LASTEXITCODE -ne 0 -or $sourceStatus) {
        throw 'Commit source changes before creating a release package.'
    }
} finally {
    Pop-Location
}

$requiredFiles = @(
    'VulkanSceneRenderer.exe', 'fmt.dll', 'glfw3.dll',
    'MaterialXCore.dll', 'MaterialXFormat.dll', 'miniz.dll',
    'spdlog.dll', 'vulkan-1.dll', 'models/basic_cube.gltf',
    'models/basic_triangle.gltf',
    'models/validation/cornell_box_local_light.gltf',
    'hdr/citrus_orchard_road_puresky_4k.exr'
)
foreach ($relativePath in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $runtimeDirectory $relativePath) -PathType Leaf)) {
        throw "Missing runtime file: $relativePath"
    }
}
$shaders = @(Get-ChildItem -LiteralPath (Join-Path $runtimeDirectory 'spv_shaders') -Filter '*.spv' -File)
if ($shaders.Count -eq 0) { throw 'No compiled shaders were found.' }
foreach ($path in @($stagingDirectory, $archivePath)) {
    if (Test-Path -LiteralPath $path) { throw "Output already exists: $path. Use a fresh output directory." }
}

New-Item -ItemType Directory -Path $stagingDirectory | Out-Null
foreach ($relativePath in ($requiredFiles | Where-Object { $_ -notmatch '/' })) {
    Copy-Item -LiteralPath (Join-Path $runtimeDirectory $relativePath) -Destination $stagingDirectory
}
foreach ($directory in @('spv_shaders', 'materials', 'hdr')) {
    Copy-Item -LiteralPath (Join-Path $runtimeDirectory $directory) -Destination $stagingDirectory -Recurse
}
$modelsDirectory = New-Item -ItemType Directory -Path (Join-Path $stagingDirectory 'models')
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'models/validation') -Destination $modelsDirectory.FullName -Recurse
Get-ChildItem -LiteralPath (Join-Path $runtimeDirectory 'models') -File |
    Where-Object { $_.Name -eq 'basic_cube.gltf' -or $_.Name -eq 'basic_triangle.gltf' -or $_.Name -like 'cube_*.png' } |
    Copy-Item -Destination $modelsDirectory.FullName
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'LICENSE') -Destination $stagingDirectory
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/windows-package-quick-start.md') -Destination (Join-Path $stagingDirectory 'README.md')
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/third-party-notices.md') -Destination (Join-Path $stagingDirectory 'THIRD_PARTY_NOTICES.md')

$licensesDirectory = (New-Item -ItemType Directory -Path (Join-Path $stagingDirectory 'THIRD_PARTY_LICENSES')).FullName
foreach ($package in @('entt', 'fmt', 'glfw3', 'glm', 'imgui', 'MaterialX', 'mikktspace',
        'miniz', 'nlohmann-json', 'shader-slang', 'spdlog', 'stb', 'tinyexr', 'tinygltf',
        'vulkan', 'vulkan-headers', 'vulkan-loader', 'vulkan-memory-allocator')) {
    Copy-Item -LiteralPath (Join-Path $dependencyDirectory "$package/copyright") -Destination (Join-Path $licensesDirectory "$package.txt")
}
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'cmake/licenses/Apache-2.0.txt') -Destination $licensesDirectory
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'cmake/licenses/Boost-1.0.txt') -Destination $licensesDirectory
Copy-Item -LiteralPath (Join-Path $tinyDirectory 'LICENSE') -Destination (Join-Path $licensesDirectory 'tinyusdz.txt')
Copy-Item -LiteralPath (Join-Path $tinyDirectory 'README.md') -Destination (Join-Path $licensesDirectory 'tinyusdz-third-party.md')
$tinyLicenses = Join-Path $licensesDirectory 'tinyusdz-bundled'
$tinySource = Join-Path $tinyDirectory 'src'
Get-ChildItem -LiteralPath $tinySource -File -Recurse |
    Where-Object { $_.Name -match 'LICENSE|COPYING|NOTICE' } |
    ForEach-Object {
        $relativePath = $_.FullName.Substring($tinySource.Length).TrimStart('\', '/')
        $destination = Join-Path $tinyLicenses $relativePath
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $destination
    }
# Several bundled libraries carry their notices in source comments rather
# than separate license files. Preserve those copyright/license blocks too.
$embeddedNotices = New-Object System.Collections.Generic.List[string]
Get-ChildItem -LiteralPath $tinySource -File -Recurse |
    Where-Object { $_.Extension -in '.h', '.hpp', '.c', '.cc', '.cpp' } |
    ForEach-Object {
        $text = [IO.File]::ReadAllText($_.FullName)
        $blocks = @([regex]::Matches($text, '/\*[\s\S]*?\*/|(?m)^[ \t]*//[^\r\n]*(?:\r?\n[ \t]*//[^\r\n]*)*') |
            Where-Object { $_.Value -match '(?i)copyright|license|permission is hereby granted' })
        if ($blocks.Count -gt 0) {
            $embeddedNotices.Add($_.FullName.Substring($tinySource.Length).TrimStart('\', '/'))
            foreach ($block in $blocks) { $embeddedNotices.Add($block.Value) }
        }
    }
$embeddedNotices | Set-Content -LiteralPath (Join-Path $licensesDirectory 'tinyusdz-embedded-notices.txt') -Encoding UTF8

[ordered]@{
    version = $Version
    source_commit = $sourceCommit
    repository = 'https://github.com/semihguresci/VulkanSceneRenderer'
    platform = 'windows-x64'
    configuration = 'Release'
    vulkan_api = '1.4'
    shader_format = 'SPIR-V 1.6'
    compiled_shader_count = $shaders.Count
    packaged_at_utc = [DateTime]::UtcNow.ToString('o')
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stagingDirectory 'build-info.json') -Encoding UTF8

Add-Type -AssemblyName System.IO.Compression.FileSystem
# Include one top-level directory so Extract All keeps the complete runtime together.
[IO.Compression.ZipFile]::CreateFromDirectory($stagingDirectory, $archivePath,
    [IO.Compression.CompressionLevel]::Optimal, $true)
$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $packageName.zip" | Set-Content -LiteralPath (Join-Path $outputDirectory 'SHA256SUMS.txt') -Encoding ASCII
Write-Output "Created $archivePath"
Write-Output "SHA256: $hash"
