# Package an already-built Windows x64 Release executable and its runtime assets.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v?[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?$')]
    [string]$Version,
    [Parameter(Mandatory = $true)][string]$RuntimeDirectory,
    [Parameter(Mandatory = $true)][string]$VcpkgInstalledDirectory,
    [string]$TinyUsdzSourceDirectory = '',
    [string]$OpenVdbInstalledDirectory = '',
    [string]$BuildDirectory = '',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../out/packages')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$sourceDirectory = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$runtimeDirectory = (Resolve-Path $RuntimeDirectory).Path
$dependencyDirectory = (Resolve-Path (Join-Path $VcpkgInstalledDirectory 'x64-windows/share')).Path
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
$portableVulkanLoader = Select-String -LiteralPath $cachePath -Pattern '^CONTAINER_VULKAN_LOADER_AVAILABLE:INTERNAL=ON$' -Quiet
$vulkanLoaderRootOption = @(Select-String -LiteralPath $cachePath -Pattern '^CONTAINER_VULKAN_LOADER_ROOT:PATH=(.+)$')
if ($vulkanLoaderRootOption.Count -gt 0 -and -not $portableVulkanLoader) {
    throw 'The selected portable Vulkan loader was not configured successfully. Reconfigure and rebuild before packaging.'
}
$vulkanLoaderVersion = 'unrecorded'
$vulkanLoaderHash = ''
$vulkanLoaderLicense = Join-Path $buildTreeDirectory 'third_party_licenses/VulkanRT-License.txt'
if ($portableVulkanLoader) {
    $vulkanLoaderVersion = '1.4.363.0'
    $vulkanLoaderHash = 'e1fcfc9489beefa6a6d9d11d6c7517f6a2b7f16e0d3fb8f4103bee0210f314a3'
    foreach ($pin in @(
        @{ Path=(Join-Path $runtimeDirectory 'vulkan-1.dll'); Hash=$vulkanLoaderHash },
        @{ Path=$vulkanLoaderLicense; Hash='404deb98dabf3e54bf9070d59676b31d69a3da2f9f6e56bdf09ad315c04c718c' })) {
        if (-not (Test-Path -LiteralPath $pin.Path -PathType Leaf) -or
            (Get-FileHash -LiteralPath $pin.Path -Algorithm SHA256).Hash -ne $pin.Hash) {
            throw "The selected portable Vulkan loader requires its pinned 1.4.363.0 runtime and notice: $($pin.Path)"
        }
    }
}
$portableVulkanSdkCore = Select-String -LiteralPath $cachePath -Pattern '^CONTAINER_VULKAN_SDK_CORE_AVAILABLE:INTERNAL=ON$' -Quiet
$vulkanSdkCoreRootOption = @(Select-String -LiteralPath $cachePath -Pattern '^CONTAINER_VULKAN_SDK_CORE_ROOT:PATH=(.+)$')
if ($vulkanSdkCoreRootOption.Count -gt 0 -and -not $portableVulkanSdkCore) {
    throw 'The selected portable Vulkan SDK core was not configured successfully. Reconfigure and rebuild before packaging.'
}
$vulkanSdkCoreNotices = @()
if ($portableVulkanSdkCore) {
    $sdkVersion = @(Select-String -LiteralPath $cachePath -Pattern '^CONTAINER_VULKAN_SDK_CORE_VERSION:INTERNAL=1\.4\.363\.1$')
    if ($sdkVersion.Count -ne 1) { throw 'The portable Vulkan SDK core version is missing or ambiguous.' }
    foreach ($pin in @(
        @{ Name='Vulkan-Headers-1.4.363-LICENSE.md'; Hash='95ad366d23fadf701d355bc45fb8b82ae2d700239471d35d41286ac3b08ff903' },
        @{ Name='Vulkan-Loader-1.4.363-LICENSE.txt'; Hash='43c0a37e6a0fa7ff3c843b3ec5a4fac84b712558ddac103fbd4c1649662a9ece' })) {
        $notice = Join-Path $buildTreeDirectory ('third_party_licenses/' + $pin.Name)
        if (-not (Test-Path -LiteralPath $notice -PathType Leaf) -or
            (Get-FileHash -LiteralPath $notice -Algorithm SHA256).Hash -ne $pin.Hash) {
            throw "The portable Vulkan SDK core requires its pinned notice: $notice"
        }
        $vulkanSdkCoreNotices += $notice
    }
}
$tinyUsdzOption = @(Select-String -LiteralPath $cachePath -Pattern '^ENABLE_TINYUSDZ_USD_LOADER:BOOL=(.*)$')
if ($tinyUsdzOption.Count -ne 1) {
    throw 'The build does not record ENABLE_TINYUSDZ_USD_LOADER. Reconfigure and rebuild before packaging.'
}
$tinyUsdzValue = $tinyUsdzOption[0].Matches[0].Groups[1].Value
$tinyUsdzEnabled = $tinyUsdzValue -notmatch '^(|0|OFF|NO|FALSE|N|IGNORE|NOTFOUND)$' -and
    $tinyUsdzValue -notmatch '-NOTFOUND$'
$tinyDirectory = ''
if ($tinyUsdzEnabled) {
    if ([string]::IsNullOrWhiteSpace($TinyUsdzSourceDirectory)) {
        throw 'The TinyUSDZ-enabled build requires -TinyUsdzSourceDirectory for its upstream and bundled notices.'
    }
    $tinyDirectory = (Resolve-Path -LiteralPath $TinyUsdzSourceDirectory).Path
    foreach ($notice in @('LICENSE', 'README.md')) {
        if (-not (Test-Path -LiteralPath (Join-Path $tinyDirectory $notice) -PathType Leaf)) {
            throw "The TinyUSDZ-enabled build is missing its upstream notice: $notice"
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $tinyDirectory 'src') -PathType Container)) {
        throw 'The TinyUSDZ-enabled build requires its src directory to collect bundled copyright and license notices.'
    }
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
$referenceOidnNotices = @{}
$referenceOptixEnabled = Select-String -LiteralPath $cachePath -Pattern '^CONTAINER_OPTIX_AVAILABLE:INTERNAL=ON$' -Quiet
$referenceOptixNotices = @()
if ($referenceOptixEnabled) {
    foreach ($noticeName in @('optix-9.1.0-LICENSE.txt', 'optix-9.1.0-BSD-3-Clause.txt', 'optix-9.1.0-LICENSE-INFO.txt')) {
        $noticePath = Join-Path $buildTreeDirectory "third_party_licenses/$noticeName"
        if (-not (Test-Path -LiteralPath $noticePath -PathType Leaf)) {
            throw "The enabled OptiX backend is missing its SDK notice: $noticePath"
        }
        $referenceOptixNotices += $noticePath
    }
}
$fsrEnabled = Select-String -LiteralPath $cachePath -Pattern '^CONTAINER_FSR_AVAILABLE:INTERNAL=ON$' -Quiet
$fsrNotices = @(
    (Join-Path $buildTreeDirectory 'third_party_licenses/AMD-FSR-SDK-2.4.0-LICENSE.txt'),
    (Join-Path $buildTreeDirectory 'third_party_licenses/AMD-FSR-SDK-2.4.0-THIRD-PARTY-NOTICES.md'))
$fsrRuntimeFiles = @(
    @{ Name='amd_fidelityfx_loader_vk.dll'; Hash='38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9' },
    @{ Name='amd_fidelityfx_upscaler_vk.dll'; Hash='4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7' })
if ($fsrEnabled) {
    foreach ($noticePath in $fsrNotices) {
        if (-not (Test-Path -LiteralPath $noticePath -PathType Leaf)) {
            throw "The enabled Vulkan FSR backend is missing its SDK notice: $noticePath"
        }
    }
    foreach ($entry in $fsrRuntimeFiles) {
        $path = Join-Path $runtimeDirectory $entry.Name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -ne $entry.Hash) {
            throw "The enabled Vulkan FSR backend requires the pinned SDK 2.4.0 $($entry.Name)."
        }
    }
}
$dlssEnabled = Select-String -LiteralPath $cachePath -Pattern '^CONTAINER_DLSS_AVAILABLE:INTERNAL=ON$' -Quiet
$dlssRrEnabled = Select-String -LiteralPath $cachePath -Pattern '^CONTAINER_DLSS_RR_AVAILABLE:INTERNAL=ON$' -Quiet
$dlssLicense = Join-Path $buildTreeDirectory 'third_party_licenses/DLSS-310.9.16-LICENSE.txt'
$dlssNotices = Join-Path $sourceDirectory 'cmake/licenses/DLSS-310.9.16-THIRD-PARTY-NOTICES.txt'
if ($dlssEnabled -or $dlssRrEnabled) {
    foreach ($noticePath in @($dlssLicense, $dlssNotices)) {
        if (-not (Test-Path -LiteralPath $noticePath -PathType Leaf)) {
            throw "The enabled Vulkan DLSS backend is missing an SDK notice: $noticePath"
        }
    }
}
function Assert-DlssRuntime([string]$Name, [string]$HashVariable) {
    $hash = @(Select-String -LiteralPath $cachePath -Pattern "^$($HashVariable):INTERNAL=([0-9a-f]{64})$")
    $dll = Join-Path $runtimeDirectory $Name
    if ($hash.Count -ne 1 -or -not (Test-Path -LiteralPath $dll -PathType Leaf) -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $dll).Hash -ne $hash[0].Matches[0].Groups[1].Value) {
        throw "The enabled Vulkan DLSS backend requires the pinned 310.9.16 release $Name. Reconfigure and rebuild before packaging."
    }
}
if ($dlssEnabled) { Assert-DlssRuntime 'nvngx_dlss.dll' 'CONTAINER_DLSS_RUNTIME_SHA256' }
if ($dlssRrEnabled) { Assert-DlssRuntime 'nvngx_dlssd.dll' 'CONTAINER_DLSS_RR_RUNTIME_SHA256' }
if (Select-String -LiteralPath $cachePath -Pattern '^CONTAINER_VK_OIDN_AVAILABLE:INTERNAL=ON$' -Quiet) {
    foreach ($noticeName in @('vk_oidn-LICENSE.txt', 'vk_oidn-NOTICE.txt')) {
        $noticePath = Join-Path $buildTreeDirectory "third_party_licenses/$noticeName"
        if (-not (Test-Path -LiteralPath $noticePath -PathType Leaf)) {
            throw "The enabled Vulkan OIDN backend is missing its SDK notice: $noticePath"
        }
        $referenceOidnNotices[$noticeName] = $noticePath
    }
}
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
    'spv_shaders/screen_reflections.comp.spv',
    'spv_shaders/screen_reflections_ray.comp.spv',
    'spv_shaders/volumetric_fog_inject.comp.spv',
    'spv_shaders/volumetric_fog_integrate.comp.spv',
    'spv_shaders/volumetric_fog_composite.comp.spv',
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
if ($referenceOidnNotices.Count -or $referenceOptixEnabled) {
    $requiredFiles += @('spv_shaders/reference_guided_pathtrace.comp.spv',
        'spv_shaders/reference_guided_background.comp.spv')
}
if ($referenceOidnNotices.Count -or $referenceOptixEnabled -or $dlssRrEnabled) {
    $requiredFiles += 'spv_shaders/reference_denoise_compose.comp.spv'
}
if ($fsrEnabled) {
    $requiredFiles += @($fsrRuntimeFiles | ForEach-Object { $_.Name })
}
if ($dlssEnabled) {
    $requiredFiles += @('nvngx_dlss.dll', 'spv_shaders/dlss_compose.comp.spv',
        'spv_shaders/dlss_transparency.comp.spv')
}
if ($dlssRrEnabled) {
    $requiredFiles += @('nvngx_dlssd.dll', 'spv_shaders/reference_pathtrace_rr.comp.spv',
        'spv_shaders/reference_pathtrace_empty_rr.comp.spv')
}
if ($fsrEnabled -or $dlssEnabled) {
    $requiredFiles += 'spv_shaders/fsr_inputs.comp.spv'
}
foreach ($relativePath in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $runtimeDirectory $relativePath) -PathType Leaf)) {
        throw "Missing runtime file: $relativePath"
    }
}
$shaders = @(Get-ChildItem -LiteralPath (Join-Path $runtimeDirectory 'spv_shaders') -Filter '*.spv' -File |
    Where-Object { $dlssRrEnabled -or $_.Name -notin @('reference_pathtrace_rr.comp.spv', 'reference_pathtrace_empty_rr.comp.spv') })
if ($shaders.Count -eq 0) { throw 'No compiled shaders were found.' }
# These notices come from the actual selected imported-target prefixes, not an
# unrelated aggregate vcpkg share/ directory that may retain older packages.
$coreLibraries = @(
    @{ Name='glm'; Cache='CONTAINER_GLM_VERSION'; Version='1.0.3'; Notice='GLM-1.0.3-LICENSE.txt'; Hash='62d2d642c7d054d4fb4c9b42faad617d6c88fcd91e317f8035aa9f277cc159c3' },
    @{ Name='vma'; Cache='CONTAINER_VMA_VERSION'; Version='3.5.0'; Notice='VMA-3.5.0-LICENSE.txt'; Hash='52df2c03d6cfc9ffec13c9d3626c530fc9ce0cbe41d5ea3d10cd46edeb1aeb38' })
foreach ($coreLibrary in $coreLibraries) {
    $pattern = '^' + $coreLibrary.Cache + ':INTERNAL=(.*)$'
    $selected = @(Select-String -LiteralPath $cachePath -Pattern $pattern)
    $notice = Join-Path $buildTreeDirectory ('third_party_licenses/' + $coreLibrary.Notice)
    if ($selected.Count -ne 1 -or $selected[0].Matches[0].Groups[1].Value -ne $coreLibrary.Version -or
        -not (Test-Path -LiteralPath $notice -PathType Leaf) -or
        (Get-FileHash -LiteralPath $notice -Algorithm SHA256).Hash -ne $coreLibrary.Hash) {
        throw "The selected $($coreLibrary.Name) build requires its verified version and notice: $notice"
    }
}
. (Join-Path $PSScriptRoot 'SelectedDependencyPackage.ps1')
$selectedDependencies = @(Read-ContainerSelectedDependencies -BuildDirectory $buildTreeDirectory)
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
foreach ($guide in @('gltf-texture-ingestion.md', 'texture-image-data.md', 'material-sampler-publication.md',
        'texture-upload-and-ownership.md', 'reference-path-tracing.md', 'reference-calibration.md', 'comfyui-bridge.md',
        'gltf-interactivity.md', 'gltf-opacity-micromaps.md', 'fsr-upscaling.md', 'dlss-upscaling.md',
        'screen-space-reflections.md', 'gltf-ies-lights.md', 'volumetric-fog.md')) {
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
foreach ($captureTool in @('gfxreconstruct.py', 'gfxreconstruct.ps1', 'comfyui_bridge.py')) {
    Copy-Item -LiteralPath (Join-Path $sourceDirectory "tools/$captureTool") -Destination $captureToolsDirectory.FullName
}
$workflowDirectory = New-Item -ItemType Directory -Path (Join-Path $captureToolsDirectory.FullName 'comfyui_workflows')
foreach ($workflow in @('hdri.json', 'restyle.json')) {
    Copy-Item -LiteralPath (Join-Path $sourceDirectory "tools/comfyui_workflows/$workflow") -Destination $workflowDirectory.FullName
}
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'docs/gfxreconstruct.md') -Destination (Join-Path $stagingDirectory 'GFXRECONSTRUCT.md')

$licensesDirectory = (New-Item -ItemType Directory -Path (Join-Path $stagingDirectory 'THIRD_PARTY_LICENSES')).FullName
foreach ($notice in $vulkanSdkCoreNotices) {
    Copy-Item -LiteralPath $notice -Destination $licensesDirectory
}
foreach ($coreLibrary in $coreLibraries) {
    Copy-Item -LiteralPath (Join-Path $buildTreeDirectory ('third_party_licenses/' + $coreLibrary.Notice)) -Destination $licensesDirectory
}
foreach ($library in $selectedDependencies) {
    Copy-Item -LiteralPath (Join-Path $buildTreeDirectory ('third_party_licenses/' + $library.notice)) -Destination $licensesDirectory
}
Copy-Item -LiteralPath (Join-Path $buildTreeDirectory 'third_party_licenses/selected-dependencies.json') -Destination $licensesDirectory
if ($portableVulkanLoader) {
    Copy-Item -LiteralPath $vulkanLoaderLicense -Destination $licensesDirectory
}
if ($fsrEnabled) {
    Copy-Item -LiteralPath $fsrNotices -Destination $licensesDirectory
}
if ($dlssEnabled -or $dlssRrEnabled) {
    Copy-Item -LiteralPath @($dlssLicense, $dlssNotices) -Destination $licensesDirectory
}
foreach ($noticeName in $referenceOidnNotices.Keys) {
    Copy-Item -LiteralPath $referenceOidnNotices[$noticeName] -Destination (Join-Path $licensesDirectory $noticeName)
}
foreach ($noticePath in $referenceOptixNotices) {
    Copy-Item -LiteralPath $noticePath -Destination $licensesDirectory
}
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
foreach ($package in @('mikktspace', 'shader-slang', 'vulkan', 'vulkan-headers', 'vulkan-loader')) {
    Copy-Item -LiteralPath (Join-Path $dependencyDirectory "$package/copyright") -Destination (Join-Path $licensesDirectory "$package.txt")
}
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'cmake/licenses/Apache-2.0.txt') -Destination $licensesDirectory
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'cmake/licenses/Boost-1.0.txt') -Destination $licensesDirectory
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'cmake/licenses/Earcut-ISC.txt') -Destination $licensesDirectory
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'cmake/licenses/Manifold-Apache-2.0.txt') -Destination $licensesDirectory
Copy-Item -LiteralPath (Join-Path $sourceDirectory 'cmake/licenses/Clipper2-Boost-1.0.txt') -Destination $licensesDirectory
if ($tinyUsdzEnabled) {
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
}

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
    portable_vulkan_loader = $portableVulkanLoader
    vulkan_loader_version = $vulkanLoaderVersion
    vulkan_loader_sha256 = $vulkanLoaderHash
    portable_vulkan_sdk_core = $portableVulkanSdkCore
    vulkan_sdk_core = $(if ($portableVulkanSdkCore) { '1.4.363.1' } else { 'unrecorded' })
    vulkan_headers = $(if ($portableVulkanSdkCore) { '1.4.363' } else { 'unrecorded' })
    shader_format = 'SPIR-V 1.6'
    glm = '1.0.3'
    vulkan_memory_allocator = '3.5.0'
    selected_dependencies = $selectedDependencies
    physics_enabled = $physicsEnabled
    physics_engine = $(if ($physicsEnabled) { 'Jolt 5.6.0' } else { 'disabled' })
    gltf_draco_enabled = $gltfCompressionEnabled['DRACO']
    gltf_meshopt_enabled = $gltfCompressionEnabled['MESHOPT']
    texture_ktx_enabled = $textureCodecsEnabled['KTX']
    texture_webp_enabled = $textureCodecsEnabled['WEBP']
    reference_oidn = $(if ($referenceOidnNotices.Count) { '0.2.0' } else { 'disabled' })
    reference_optix = $(if ($referenceOptixEnabled) { '9.1.0' } else { 'disabled' })
    fsr_sdk = $(if ($fsrEnabled) { '2.4.0' } else { 'disabled' })
    fsr_upscaler = $(if ($fsrEnabled) { 'runtime-selected compatible provider' } else { 'disabled' })
    fsr_bundled_portable = $(if ($fsrEnabled) { '3.1.6' } else { 'disabled' })
    dlss_upscaler = $(if ($dlssEnabled) { '310.9.16' } else { 'disabled' })
    dlss_ray_reconstruction = $(if ($dlssRrEnabled) { '310.9.16' } else { 'disabled' })
    openvdb_enabled = $openVdbEnabled
    nanovdb_enabled = $nanoVdbEnabled
    nanovdb_io_enabled = $nanoIoEnabled
    compiled_shader_count = $shaders.Count
    runtime_dll_count = $runtimeDlls.Count
    ltc_asset_file_count = $ltcAssets.Count
    runtime_sha256 = $runtimeHashes
    packaged_at_utc = [DateTime]::UtcNow.ToString('o')
} | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $stagingDirectory 'build-info.json') -Encoding UTF8

Add-Type -AssemblyName System.IO.Compression.FileSystem
# Include one top-level directory so Extract All keeps the complete runtime together.
[IO.Compression.ZipFile]::CreateFromDirectory($stagingDirectory, $archivePath,
    [IO.Compression.CompressionLevel]::Optimal, $true)
$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $packageName.zip" | Set-Content -LiteralPath (Join-Path $outputDirectory 'SHA256SUMS.txt') -Encoding ASCII
Write-Output "Created $archivePath"
Write-Output "SHA256: $hash"
