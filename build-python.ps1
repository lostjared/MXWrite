param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$Python = $env:MXWRITE_PYTHON,
    [string]$BuildDir = "build-python-windows",
    [string]$Triplet = "x64-windows",
    [switch]$WithCuda
)

$ErrorActionPreference = "Stop"
$repo_dir = $PSScriptRoot

if (-not $PSBoundParameters.ContainsKey("VcpkgRoot") -and $VcpkgRoot -and
    -not (Test-Path -LiteralPath (Join-Path $VcpkgRoot "installed\$Triplet\share\ffmpeg")) -and
    (Test-Path -LiteralPath "C:\vcpkg\installed\$Triplet\share\ffmpeg")) {
    Write-Host "Using C:\vcpkg: VCPKG_ROOT has no installed FFmpeg for $Triplet."
    $VcpkgRoot = "C:\vcpkg"
}
if (-not $VcpkgRoot -and (Test-Path -LiteralPath "C:\vcpkg\scripts\buildsystems\vcpkg.cmake")) {
    $VcpkgRoot = "C:\vcpkg"
}
if (-not $VcpkgRoot) {
    throw "Set VCPKG_ROOT or pass -VcpkgRoot to your vcpkg checkout. See README.md."
}
$VcpkgRoot = (Resolve-Path -LiteralPath $VcpkgRoot).Path
$toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path -LiteralPath $toolchain)) {
    throw "vcpkg toolchain not found: $toolchain"
}
if (-not $Python) {
    $vcpkg_python = Join-Path $VcpkgRoot "installed\$Triplet\tools\python3\python.exe"
    if (Test-Path -LiteralPath $vcpkg_python) {
        $Python = $vcpkg_python
    } else {
        $Python = (Get-Command python.exe -ErrorAction Stop).Source
    }
}
$Python = (Get-Command $Python -ErrorAction Stop).Source
& $Python -c "import nanobind, numpy"
if ($LASTEXITCODE -ne 0) {
    throw "Install Python prerequisites first: & '$Python' -m pip install nanobind numpy"
}
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $repo_dir $BuildDir
}
$disable_cuda = if ($WithCuda) { "OFF" } else { "ON" }
Write-Host "Building MXWrite for $Python in $BuildDir"
& cmake -S $repo_dir -B $BuildDir `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DVCPKG_TARGET_TRIPLET=$Triplet" `
    "-DPython_EXECUTABLE=$Python" -DPYTHON_MODULE=ON -DSHARED=OFF `
    "-DCMAKE_DISABLE_FIND_PACKAGE_CUDAToolkit=$disable_cuda" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "MXWrite CMake configuration failed." }
& cmake --build $BuildDir --config Release --target mxwrite_ext --parallel
if ($LASTEXITCODE -ne 0) { throw "MXWrite Python build failed." }
$module_dir = $BuildDir
if (Test-Path -LiteralPath (Join-Path $BuildDir "Release")) {
    $module_dir = Join-Path $BuildDir "Release"
}
& $Python (Join-Path $repo_dir "mxpy.py") --module-dir $module_dir --check
if ($LASTEXITCODE -ne 0) { throw "MXWrite import check failed. See README.md." }
Write-Host "Build complete. Run .\mxpy.cmd --module-dir `"$module_dir`" pattern"
