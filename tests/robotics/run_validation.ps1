[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][ValidateSet('full', 'physics-only', 'urdf-only', 'minimal')][string]$Profile,
    [ValidateSet('Build', 'Test', 'All')][string]$Phase = 'All',
    [string]$Configuration = 'Release',
    [ValidateRange(1, 64)][int]$Parallel = 2
)

$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')
$physics = $Profile -in @('full', 'physics-only')
$urdf = $Profile -in @('full', 'urdf-only')

function Assert-CMakeSwitch([string]$Name, [bool]$Expected) {
    $lines = @($cache | Where-Object { $_.StartsWith("${Name}:BOOL=", [System.StringComparison]::Ordinal) })
    if ($lines.Count -ne 1) {
        throw "Expected one $Name switch in the configured CMake cache."
    }
    $value = $lines[0].Split('=', 2)[1]
    $enabled = $value -in @('ON', 'TRUE', 'YES', 'Y', '1')
    if ($enabled -ne $Expected) {
        throw "The $Profile profile requires $Name=$Expected. The cache has $value."
    }
}

Assert-CMakeSwitch ENABLE_TESTS $true
Assert-CMakeSwitch ENABLE_PHYSICS $physics
Assert-CMakeSwitch ENABLE_ROBOTICS_URDF $urdf

$tests = @(
    'robotics_module_boundaries',
    'robotics_gpu_smoke_runner_tests',
    'bim_robotics_benchmark_runner_tests',
    'bim_twin_cli',
    'bim_twin_ingest_cli',
    'twin_replay_tests',
    'twin_ingest_tests',
    'drive_actuation_tests',
    'drive_sensor_tests',
    'raycast_lidar_tests',
    'sensor_camera_tests',
    'sensor_camera_dataset_tests',
    'dataset_simulation_report_tests',
    'robotics_dataset_reader_tests',
    'bim_sensor_label_tests',
    'bim_robotics_scene_tests',
    'bim_robotics_usd_tests',
    'bim_collision_geometry_tests',
    'bim_asset_cache_tests',
    'dotbim_loader_tests',
    'ifc_tessellated_loader_tests',
    'ifcx_loader_tests',
    'bim_schedule_extractor_tests',
    'bim_model_compare_tests'
)
$scriptTests = @('robotics_module_boundaries', 'robotics_gpu_smoke_runner_tests', 'bim_robotics_benchmark_runner_tests',
                 'bim_twin_cli', 'bim_twin_ingest_cli')
$tools = @('BimRoboticsScene', 'BimRoboticsUsd', 'BimRoboticsAssets', 'BimRoboticsDataset', 'BimRoboticsTwin')

if ($physics) {
    $tests += @(
        'physics_world_tests',
        'differential_drive_tests',
        'bim_collision_world_tests',
        'bim_physics_determinism_tests',
        'articulated_grasp_tests',
        'bim_drive_actuation_tests',
        'bim_drive_sensor_tests',
        'bim_lidar_tests',
        'bim_fixed_step_replay',
        'bim_fleet_missions'
    )
    $scriptTests += @('bim_fixed_step_replay', 'bim_fleet_missions')
    $tools += @('BimRoboticsCollision', 'BimRoboticsSim', 'BimRoboticsFleet')
}
if ($urdf) {
    $tests += @('urdf_runtime_tests', 'urdf_asset_roundtrip')
    $scriptTests += 'urdf_asset_roundtrip'
    $tools += 'BimRoboticsUrdf'
}
if ($physics -and $urdf) {
    $tests += @('urdf_collision_tests', 'bim_robotics_frame_integration_tests',
                'bim_frame_draw_routing_planner_tests', 'render_graph_tests')
}

$targets = $tools + @($tests | Where-Object { $_ -notin $scriptTests })
if ($Phase -in @('Build', 'All')) {
    cmake --build $build --config $Configuration --parallel $Parallel --target @targets
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($Phase -in @('Test', 'All')) {
    $filter = '^(' + ($tests -join '|') + ')$'
    $evidence = Join-Path $build 'robotics-validation'
    New-Item -ItemType Directory -Force -Path $evidence | Out-Null
    $listing = ctest --test-dir $build -C $Configuration -R $filter --show-only=json-v1
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $registered = @(($listing -join "`n" | ConvertFrom-Json).tests.name)
    $missing = @($tests | Where-Object { $_ -notin $registered })
    if ($missing.Count) { throw "Missing CTest registrations: $($missing -join ', ')." }

    $inventory = [ordered]@{
        profile = $Profile
        configuration = $Configuration
        physics = $physics
        urdf = $urdf
        targets = $targets
        tests = $tests
    }
    $inventory | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $evidence 'profile.json') -Encoding utf8
    $junit = Join-Path $evidence 'ctest.junit.xml'
    ctest --test-dir $build -C $Configuration -R $filter --no-tests=error --output-on-failure --output-junit $junit
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    [xml]$report = Get-Content -Raw -LiteralPath $junit
    $cases = @($report.SelectNodes('/testsuite/testcase'))
    $ran = @($cases | Where-Object { $_.GetAttribute('status') -eq 'run' -and -not $_.SelectSingleNode('skipped') } |
        ForEach-Object { $_.GetAttribute('name') })
    $omitted = @($tests | Where-Object { $_ -notin $ran })
    if ($omitted.Count) { throw "Expected tests did not run: $($omitted -join ', ')." }
}
