function Read-ContainerSelectedDependencies {
    param([Parameter(Mandatory = $true)][string]$BuildDirectory)

    $noticeDirectory = Join-Path $BuildDirectory 'third_party_licenses'
    $indexPath = Join-Path $noticeDirectory 'selected-dependencies.json'
    if (-not (Test-Path -LiteralPath $indexPath -PathType Leaf)) {
        throw 'The selected dependency notice index is missing; reconfigure the build.'
    }
    $index = Get-Content -LiteralPath $indexPath -Raw | ConvertFrom-Json
    $expectedVersions = @{
        'fmt' = '12.2.0'; 'spdlog' = '1.17.0'; 'glfw3' = '3.5.1'
        'imgui' = '1.93.0'; 'materialx' = '1.39.5'; 'miniz' = '3.1.2'
        'stb' = 'snapshot-4e9b368bfc071e97206ea5c6adba68dd0cbf8897'
        'entt' = '4.0.0'; 'nlohmann-json' = '3.12.0'
        'tinygltf' = '3.1.0'; 'tinyexr' = '3.4.0'
    }
    $rows = @($index.libraries)
    if ($rows.Count -ne $expectedVersions.Count) {
        throw 'The selected dependency notice index has the wrong library count.'
    }
    $names = @{}
    $filenames = @{}
    foreach ($library in $rows) {
        $name = [string]$library.name
        $filename = [string]$library.notice
        if (-not $expectedVersions.ContainsKey($name) -or $names.ContainsKey($name) -or
            [string]$library.version -cne $expectedVersions[$name]) {
            throw "The selected dependency version is missing, duplicate or stale: $name"
        }
        if (-not $filename -or $filename -match '[/\\:]' -or $filename -in @('.', '..') -or
            [IO.Path]::GetFileName($filename) -cne $filename -or $filenames.ContainsKey($filename) -or
            [string]$library.sha256 -cnotmatch '^[0-9a-f]{64}$') {
            throw "The selected dependency notice entry is invalid: $name"
        }
        $noticePath = Join-Path $noticeDirectory $filename
        if (-not (Test-Path -LiteralPath $noticePath -PathType Leaf) -or
            (Get-FileHash -LiteralPath $noticePath -Algorithm SHA256).Hash.ToLowerInvariant() -cne [string]$library.sha256) {
            throw "The selected dependency notice is missing or changed: $name"
        }
        $names[$name] = $true
        $filenames[$filename] = $true
    }
    return $rows
}
