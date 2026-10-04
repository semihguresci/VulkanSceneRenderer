# Optional developer tools; Python 3 is also required by the SDK capture scripts.
$ErrorActionPreference = 'Stop'
$capturePython = if ($env:CONTAINER_PYTHON) { $env:CONTAINER_PYTHON } else { 'python' }
& $capturePython (Join-Path $PSScriptRoot 'gfxreconstruct.py') @args
exit $LASTEXITCODE
