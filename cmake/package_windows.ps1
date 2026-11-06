# Package an already-built Windows x64 Release executable and its runtime assets.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v?[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?$')]
    [string]$Version,
    [Parameter(Mandatory = $true)][string]$RuntimeDirectory,
    [Parameter(Mandatory = $true)][string]$VcpkgInstalledDirectory,
    [Parameter(Mandatory = $true)][string]$TinyUsdzSourceDirectory,
    [string]$OpenVdbInstalledDirectory = '',
    [string]$BuildDirectory = '',
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

# Native Visual Studio builds stage executables in Release/, while single-
# configuration builds keep them at the build root. Read the actual cache so
# a disabled backend cannot accidentally inherit a stale enabled-build notice.
$buildTreeDirectory = ''
if ($BuildDirectory) {
    $buildTreeDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
} else {
    foreach ($candidate in @($runtimeDirectory, (Split-Path -Parent $runtimeDirectory))) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'CMakeCache.txt') -PathType Leaf) {
            $buildTreeDirectory = $candidate
            break
        }
    }
}
if (-not $buildTreeDirectory) {
    throw 'Cannot locate the build cache. Pass -BuildDirectory for a separately staged runtime.'
}
$cachePath = Join-Path $buildTreeDirectory 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "Missing build cache: $cachePath"
}
$physicsOption = @(Select-String -LiteralPath $cachePath -Pattern '^ENABLE_PHYSICS:BOOL=(.*)$')
if ($physicsOption.Count -ne 1) {
    throw 'The build does not record ENABLE_PHYSICS. Reconfigure and rebuild before packaging.'
}
$physicsValue = $physicsOption[0].Matches[0].Groups[1].Value
# CMake treats every value except its documented false constants as enabled;
# values such as Y or 2 must not omit the notice from an enabled binary.
$physicsEnabled = $physicsValue -notmatch '^(|0|OFF|NO|FALSE|N|IGNORE|NOTFOUND)$' -and
    $physicsValue -notmatch '-NOTFOUND$'
$joltLicense = Join-Path $buildTreeDirectory 'third_party_licenses/Jolt-MIT.txt'
if ($physicsEnabled -and -not (Test-Path -LiteralPath $joltLicense -PathType Leaf)) {
    throw "The physics-enabled build is missing its Jolt MIT notice: $joltLicense"
}

$gltfCompressionEnabled = @{}
$gltfCompressionNotices = @{}
foreach ($backend in @('DRACO', 'MESHOPT')) {
    $compressionOption = @(Select-String -LiteralPath $cachePath -Pattern "^ENABLE_GLTF_${backend}:BOOL=(.*)$")
    $enabled = $compressionOption.Count -eq 1 -and
        $compressionOption[0].Matches[0].Groups[1].Value -notmatch '^(|0|OFF|NO|FALSE|N|IGNORE|NOTFOUND)$' -and
        $compressionOption[0].Matches[0].Groups[1].Value -notmatch '-NOTFOUND$'
    $gltfCompressionEnabled[$backend] = $enabled
    if ($enabled) {
        $noticeName = $(if ($backend -eq 'DRACO') { 'Draco-Apache-2.0.txt' } else { 'meshoptimizer-MIT.txt' })
        $noticePath = Join-Path $buildTreeDirectory "third_party_licenses/$noticeName"
        if (-not (Test-Path -LiteralPath $noticePath -PathType Leaf)) {
            throw "The enabled glTF $backend decoder is missing its upstream notice: $noticePath"
        }
        $gltfCompressionNotices[$noticeName] = $noticePath
    }
}

$textureCodecsEnabled = @{}
$textureCodecNotices = @{}
foreach ($backend in @('KTX', 'WEBP')) {
    $codecOption = @(Select-String -LiteralPath $cachePath -Pattern "^ENABLE_TEXTURE_${backend}:BOOL=(.*)$")
    $enabled = $codecOption.Count -eq 1 -and
        $codecOption[0].Matches[0].Groups[1].Value -notmatch '^(|0|OFF|NO|FALSE|N|IGNORE|NOTFOUND)$' -and
        $codecOption[0].Matches[0].Groups[1].Value -notmatch '-NOTFOUND$'
    $textureCodecsEnabled[$backend] = $enabled
    if ($enabled) {
        $noticeRoot = Join-Path $buildTreeDirectory 'third_party_licenses'
        $packageNotice = Join-Path $noticeRoot "${backend}-package-notices.txt"
        if (Test-Path -LiteralPath $packageNotice -PathType Leaf) {
            $textureCodecNotices["${backend}-package-notices.txt"] = $packageNotice
        } elseif ($backend -eq 'KTX') {
            $ktxNotices = Join-Path $noticeRoot 'KTX'
            foreach ($required in @('LICENSE.md', 'NOTICE.md', 'external-basisu-LICENSE', 'external-basisu-zstd-LICENSE', 'external-dfdutils-LICENSE.adoc', 'uthash-notice.txt')) {
                if (-not (Test-Path -LiteralPath (Join-Path $ktxNotices $required) -PathType Leaf)) {
                    throw "The enabled KTX codec is missing its upstream notice: $required"
                }
            }
            $textureCodecNotices['KTX'] = $ktxNotices
        } else {
            foreach ($name in @('WebP-COPYING.txt', 'WebP-PATENTS.txt')) {
                $notice = Join-Path $noticeRoot $name
                if (-not (Test-Path -LiteralPath $notice -PathType Leaf)) {
                    throw "The enabled WebP codec is missing its upstream notice: $notice"
                }
                $textureCodecNotices[$name] = $notice
            }
        }
    }
}

$openVdbOption = @(Select-String -LiteralPath $cachePath -Pattern '^ENABLE_OPENVDB:BOOL=(.*)$')
$openVdbEnabled = $false
if ($openVdbOption.Count -eq 1) {
    $openVdbValue = $openVdbOption[0].Matches[0].Groups[1].Value
    $openVdbEnabled = $openVdbValue -notmatch '^(|0|OFF|NO|FALSE|N|IGNORE|NOTFOUND)$' -and
        $openVdbValue -notmatch '-NOTFOUND$'
}
$openVdbDependencyDirectory = $dependencyDirectory
$nanoVdbOption = @(Select-String -LiteralPath $cachePath -Pattern '^ENABLE_NANOVDB:BOOL=(.*)$')
$nanoVdbEnabled = $openVdbEnabled -and $nanoVdbOption.Count -eq 1 -and
    $nanoVdbOption[0].Matches[0].Groups[1].Value -notmatch '^(|0|OFF|NO|FALSE|N|IGNORE|NOTFOUND)$' -and
    $nanoVdbOption[0].Matches[0].Groups[1].Value -notmatch '-NOTFOUND$'
$nanoIoOption = @(Select-String -LiteralPath $cachePath -Pattern '^ENABLE_NANOVDB_IO:BOOL=(.*)$')
$nanoIoEnabled = $nanoIoOption.Count -eq 1 -and
    $nanoIoOption[0].Matches[0].Groups[1].Value -notmatch '^(|0|OFF|NO|FALSE|N|IGNORE|NOTFOUND)$' -and
    $nanoIoOption[0].Matches[0].Groups[1].Value -notmatch '-NOTFOUND$'
$nanoVdbEnabled = $nanoVdbEnabled -or $nanoIoEnabled
$nanoCodecNotices = @()
if ($nanoIoEnabled) {
    if (Select-String -LiteralPath $cachePath -Pattern '^CONTAINER_NANOVDB_ZIP_ENABLED:INTERNAL=1$' -Quiet) {
        $nanoCodecNotices += 'zlib'
    }
    if (Select-String -LiteralPath $cachePath -Pattern '^CONTAINER_NANOVDB_BLOSC_ENABLED:INTERNAL=1$' -Quiet) {
        $nanoCodecNotices += 'blosc'
    }
}
$nanoVdbLicense = $null
if ($nanoVdbEnabled) {
    $licenseEntry = @(Select-String -LiteralPath $cachePath -Pattern '^NANOVDB_LICENSE_FILE:FILEPATH=(.*)$')
    if ($licenseEntry.Count -eq 1) { $nanoVdbLicense = $licenseEntry[0].Matches[0].Groups[1].Value }
    if (-not $nanoVdbLicense -or -not (Test-Path -LiteralPath $nanoVdbLicense -PathType Leaf)) {
        throw 'The NanoVDB-enabled build needs its upstream license. Configure NANOVDB_LICENSE_FILE before packaging.'
    }
}
if ($OpenVdbInstalledDirectory) {
    $openVdbDependencyDirectory = (Resolve-Path -LiteralPath (
        Join-Path $OpenVdbInstalledDirectory 'x64-windows/share')).Path
}
if ($openVdbEnabled) {
    foreach ($dependency in @('openvdb', 'tbb', 'blosc', 'imath', 'openexr')) {
        $notice = Join-Path $openVdbDependencyDirectory "$dependency/copyright"
        if (-not (Test-Path -LiteralPath $notice -PathType Leaf)) {
            throw "The OpenVDB-enabled build is missing a dependency notice: $notice. Pass -OpenVdbInstalledDirectory when installed separately."
        }
    }
}
foreach ($dependency in $nanoCodecNotices) {
    $notice = Join-Path $openVdbDependencyDirectory "$dependency/copyright"
    if (-not (Test-Path -LiteralPath $notice -PathType Leaf)) {
        throw "The native NanoVDB build is missing its codec notice: $notice. Pass -OpenVdbInstalledDirectory when providers are installed separately."
    }
}

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
    'VulkanSceneRenderer.exe', 'OpenVdbTransportRender.exe', 'fmt.dll', 'glfw3.dll',
    'MaterialXCore.dll', 'MaterialXFormat.dll', 'miniz.dll',
    'spdlog.dll', 'vulkan-1.dll', 'manifold.dll', 'models/basic_cube.gltf',
    'models/basic_triangle.gltf',
    'models/validation/cornell_box_local_light.gltf',
    'models/validation/taa_scene.gltf',
    'models/validation/taa_equivalent.gltf',
    'models/validation/taa_equivalent.bim',
    'models/validation/taa_equivalent.usda',
    'models/validation/taa_object.json',
    'spv_shaders/temporal_velocity.vert.spv',
    'spv_shaders/temporal_velocity.frag.spv',
    'spv_shaders/temporal_compose.comp.spv',
    'spv_shaders/temporal_resolve.comp.spv',
    'spv_shaders/openvdb_transport_reference.comp.spv',
    'spv_shaders/ray_shadow_trace.comp.spv',
    'spv_shaders/ray_shadow_filter.comp.spv',
    'spv_shaders/ddgi_trace.comp.spv',
    'spv_shaders/ddgi_update.comp.spv',
    'spv_shaders/ddgi_debug.vert.spv',
    'spv_shaders/ddgi_debug.frag.spv',
    'spv_shaders/deferred_directional_no_ddgi.frag.spv',
    'spv_shaders/forward_opaque_no_ddgi.frag.spv',
    'spv_shaders/forward_opaque_ray_no_ddgi.frag.spv',
    'spv_shaders/point_light_no_ddgi.frag.spv',
    'spv_shaders/tiled_lighting_no_ddgi.frag.spv',
    'materials/ltc/matrix.bin', 'materials/ltc/amplitude.bin',
    'materials/ltc/LICENSE.txt',
    'hdr/citrus_orchard_road_puresky_4k.exr'
)
if ($openVdbEnabled -or $nanoIoEnabled) {
    $requiredFiles += 'OpenVdbTool.exe'
    $requiredFiles += 'spv_shaders/openvdb_volume.comp.spv'
}
if ($gltfCompressionEnabled['DRACO']) {
    $requiredFiles += 'ContainerDracoWorker.exe'
}
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
# Follow the actual PE imports of every shipped executable. A fixed DLL list
# omits optional backends such as OpenVDB and their compression dependencies.
$runtimeExecutables = @((Join-Path $runtimeDirectory 'VulkanSceneRenderer.exe'),
    (Join-Path $runtimeDirectory 'OpenVdbTransportRender.exe'))
if ($openVdbEnabled -or $nanoIoEnabled) {
    $runtimeExecutables += Join-Path $runtimeDirectory 'OpenVdbTool.exe'
}
if ($gltfCompressionEnabled['DRACO']) {
    $runtimeExecutables += Join-Path $runtimeDirectory 'ContainerDracoWorker.exe'
}
$runtimeStageArguments = @(
    "-DEXECUTABLES=$($runtimeExecutables -join '|')",
    "-DDESTINATION=$stagingDirectory"
)
$linkerCacheEntry = @(Select-String -LiteralPath $cachePath -Pattern '^CMAKE_LINKER:FILEPATH=(.*)$')
if ($linkerCacheEntry.Count -eq 1) {
    $dumpbinPath = Join-Path (Split-Path -Parent $linkerCacheEntry[0].Matches[0].Groups[1].Value) 'dumpbin.exe'
    if (Test-Path -LiteralPath $dumpbinPath -PathType Leaf) {
        $runtimeStageArguments += "-DDUMPBIN=$dumpbinPath"
    }
}
$runtimeStageArguments += @('-P', (Join-Path $PSScriptRoot 'StageWindowsPackageRuntime.cmake'))
& cmake @runtimeStageArguments
if ($LASTEXITCODE -ne 0) {
    throw 'Cannot stage the complete package runtime dependency closure.'
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
foreach ($guide in @('gltf-texture-ingestion.md', 'texture-image-data.md', 'material-sampler-publication.md', 'texture-upload-and-ownership.md')) {
    Copy-Item -LiteralPath (Join-Path $sourceDirectory "docs/$guide") -Destination $stagingDirectory
}
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/ltc-area-lighting.md') -Destination (Join-Path $stagingDirectory 'ltc-area-lighting.md')
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/ddgi-probe-foundation.md') -Destination (Join-Path $stagingDirectory 'ddgi-probe-foundation.md')
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/physics-integration.md') -Destination (Join-Path $stagingDirectory 'physics-integration.md')
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/physics-joints.md') -Destination (Join-Path $stagingDirectory 'physics-joints.md')
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/physics-scene-diversity.md') -Destination (Join-Path $stagingDirectory 'physics-scene-diversity.md')
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/physics-moving-supports.md') -Destination (Join-Path $stagingDirectory 'physics-moving-supports.md')
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/physics-scale-validation.md') -Destination (Join-Path $stagingDirectory 'physics-scale-validation.md')
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/physics-pending-edits.md') -Destination (Join-Path $stagingDirectory 'physics-pending-edits.md')
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/openvdb.md') -Destination (Join-Path $stagingDirectory 'openvdb.md')
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/openvdb-feature-status.md') -Destination (Join-Path $stagingDirectory 'openvdb-feature-status.md')
$captureToolsDirectory = New-Item -ItemType Directory -Path (Join-Path $stagingDirectory 'tools')
foreach ($captureTool in @('gfxreconstruct.py', 'gfxreconstruct.ps1')) {
    Copy-Item -LiteralPath (Join-Path $sourceDirectory "tools/$captureTool") -Destination $captureToolsDirectory.FullName
}
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/gfxreconstruct.md') -Destination (Join-Path $stagingDirectory 'GFXRECONSTRUCT.md')

$licensesDirectory = (New-Item -ItemType Directory -Path (Join-Path $stagingDirectory 'THIRD_PARTY_LICENSES')).FullName
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/tone-curves-Apache-2.0.txt') -Destination (Join-Path $licensesDirectory 'ToneCurves-Apache-2.0.txt')
foreach ($noticeName in $gltfCompressionNotices.Keys) {
    Copy-Item -LiteralPath $gltfCompressionNotices[$noticeName] -Destination (Join-Path $licensesDirectory $noticeName)
}
foreach ($noticeName in $textureCodecNotices.Keys) {
    Copy-Item -LiteralPath $textureCodecNotices[$noticeName] -Destination (Join-Path $licensesDirectory $noticeName) -Recurse
}
if ($nanoVdbEnabled) {
    Copy-Item -LiteralPath $nanoVdbLicense -Destination (Join-Path $licensesDirectory 'NanoVDB-Apache-2.0.txt')
}
if ($physicsEnabled) {
    Copy-Item -LiteralPath $joltLicense -Destination (Join-Path $licensesDirectory 'Jolt-MIT.txt')
}
if ($openVdbEnabled -or $nanoCodecNotices.Count -gt 0) {
    # Preserve the notices of all transitive vcpkg dependencies, including the
    # Boost and compression components selected by the installed OpenVDB port.
    Get-ChildItem -LiteralPath $openVdbDependencyDirectory -Directory |
        ForEach-Object {
            $notice = Join-Path $_.FullName 'copyright'
            if (Test-Path -LiteralPath $notice -PathType Leaf) {
                Copy-Item -LiteralPath $notice -Destination (
                    Join-Path $licensesDirectory "openvdb-dependency-$($_.Name).txt")
            }
        }
}
foreach ($package in @('entt', 'fmt', 'glfw3', 'glm', 'imgui', 'MaterialX', 'mikktspace',
        'miniz', 'nlohmann-json', 'shader-slang', 'spdlog', 'stb', 'tinyexr', 'tinygltf',
        'vulkan', 'vulkan-headers', 'vulkan-loader', 'vulkan-memory-allocator')) {
    Copy-Item -LiteralPath (Join-Path $dependencyDirectory "$package/copyright") -Destination (Join-Path $licensesDirectory "$package.txt")
}
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'cmake/licenses/Apache-2.0.txt') -Destination $licensesDirectory
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'cmake/licenses/Boost-1.0.txt') -Destination $licensesDirectory
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'cmake/licenses/Earcut-ISC.txt') -Destination $licensesDirectory
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'cmake/licenses/Manifold-Apache-2.0.txt') -Destination $licensesDirectory
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'cmake/licenses/Clipper2-Boost-1.0.txt') -Destination $licensesDirectory
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

# Bind the build manifest to the staged payload that is actually archived.
$runtimeDlls = @(Get-ChildItem -LiteralPath $stagingDirectory -Filter '*.dll' -File)
$ltcAssets = @(Get-ChildItem -LiteralPath (Join-Path $stagingDirectory 'materials/ltc') -File)
$hashedRuntimeFiles = @(
    Get-Item -LiteralPath (Join-Path $stagingDirectory 'VulkanSceneRenderer.exe')
    Get-Item -LiteralPath (Join-Path $stagingDirectory 'OpenVdbTransportRender.exe')
    if ($openVdbEnabled -or $nanoIoEnabled) { Get-Item -LiteralPath (Join-Path $stagingDirectory 'OpenVdbTool.exe') }
    if ($gltfCompressionEnabled['DRACO']) { Get-Item -LiteralPath (Join-Path $stagingDirectory 'ContainerDracoWorker.exe') }
    $runtimeDlls
    Get-ChildItem -LiteralPath (Join-Path $stagingDirectory 'spv_shaders') -Filter '*.spv' -File
    $ltcAssets
)
$runtimeHashes = [ordered]@{}
foreach ($file in ($hashedRuntimeFiles | Sort-Object FullName)) {
    $relativePath = $file.FullName.Substring($stagingDirectory.Length + 1).Replace([char]92, [char]47)
    $runtimeHashes[$relativePath] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}

[ordered]@{
    version = $Version
    source_commit = $sourceCommit
    repository = 'https://github.com/semihguresci/VulkanSceneRenderer'
    platform = 'windows-x64'
    configuration = 'Release'
    vulkan_api = '1.4'
    shader_format = 'SPIR-V 1.6'
    physics_enabled = $physicsEnabled
    physics_engine = $(if ($physicsEnabled) { 'Jolt 5.6.0' } else { 'disabled' })
    gltf_draco_enabled = $gltfCompressionEnabled['DRACO']
    gltf_meshopt_enabled = $gltfCompressionEnabled['MESHOPT']
    texture_ktx_enabled = $textureCodecsEnabled['KTX']
    texture_webp_enabled = $textureCodecsEnabled['WEBP']
    openvdb_enabled = $openVdbEnabled
    nanovdb_enabled = $nanoVdbEnabled
    nanovdb_io_enabled = $nanoIoEnabled
    compiled_shader_count = $shaders.Count
    runtime_dll_count = $runtimeDlls.Count
    ltc_asset_file_count = $ltcAssets.Count
    runtime_sha256 = $runtimeHashes
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
