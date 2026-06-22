#Requires -Version 5.1
# Build msga on Windows.
# Auto-detects MSVC or MSYS2/MinGW toolchain (whichever configure-windows.ps1 installed).
#
# Usage:
#   scripts\build.ps1           - Debug build into .\build\
#   scripts\build.ps1 --clean   - Wipe build dir then rebuild

param([switch]$Clean)

$ErrorActionPreference = 'Stop'

function die($msg) { Write-Host "error: $msg" -ForegroundColor Red; exit 1 }

# Refresh PATH from machine + user environment so tools installed in the same
# session as configure-windows.ps1 are visible without restarting the shell.
$machinePath = [System.Environment]::GetEnvironmentVariable('PATH', 'Machine')
$userPath    = [System.Environment]::GetEnvironmentVariable('PATH', 'User')
$env:PATH    = ($machinePath, $userPath | Where-Object { $_ }) -join ';'

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$BuildDir    = Join-Path $ProjectRoot 'build'
$Nproc       = $env:NUMBER_OF_PROCESSORS
$MSYS2_ROOT  = 'C:\msys64'

# --- Detect toolchain ---------------------------------------------------------
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$Toolchain = $null

if (Test-Path $vsWhere) {
    $vs = & $vsWhere -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -latest -format json | ConvertFrom-Json
    if ($vs) { $Toolchain = 'msvc' }
}
if (-not $Toolchain -and (Test-Path "$MSYS2_ROOT\usr\bin\bash.exe")) {
    $Toolchain = 'msys2'
}
if (-not $Toolchain) {
    die "No C++ toolchain found. Run scripts\configure-windows.ps1 first."
}
Write-Host "Toolchain: $Toolchain"

# --- Locate Qt6 ---------------------------------------------------------------
function Find-Qt {
    if ($env:QT_PREFIX) { return $env:QT_PREFIX }
    if ($Toolchain -eq 'msys2') {
        $p = "$MSYS2_ROOT\mingw64"
        if (Test-Path "$p\lib\cmake\Qt6\Qt6Config.cmake") { return $p }
    } else {
        if (Test-Path 'C:\Qt') {
            $dirs = @(Get-ChildItem 'C:\Qt' -Directory | Sort-Object Name -Descending)
            foreach ($dir in $dirs) {
                foreach ($arch in 'msvc2022_64', 'msvc2019_64') {
                    $p = Join-Path $dir.FullName $arch
                    if (Test-Path "$p\lib\cmake\Qt6\Qt6Config.cmake") { return $p }
                }
            }
        }
    }
}

$QtRoot = Find-Qt
if (-not $QtRoot) {
    die "Qt6 not found. Run scripts\configure-windows.ps1 or set QT_PREFIX."
}

# --- Toolchain-specific cmake args --------------------------------------------
if ($Toolchain -eq 'msvc') {
    $GeneratorArgs   = @('-G', 'Visual Studio 17 2022', '-A', 'x64')
    $CompilerArgs    = @()
    $BuildConfigArgs = @('--config', 'Debug')
    $FreshnessFile   = 'CMakeCache.txt'
} else {
    $mingw64Bin    = "$MSYS2_ROOT\mingw64\bin"
    $env:PATH      = "$mingw64Bin;$env:PATH"
    $GeneratorArgs = @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug')
    $CompilerArgs  = @(
        "-DCMAKE_C_COMPILER=$mingw64Bin\gcc.exe",
        "-DCMAKE_CXX_COMPILER=$mingw64Bin\g++.exe",
        "-DCMAKE_MAKE_PROGRAM=$mingw64Bin\ninja.exe"
    )
    $BuildConfigArgs = @()
    $FreshnessFile   = 'build.ninja'
}

# --- Wipe if --Clean or stale -------------------------------------------------
$freshnessPath = Join-Path $BuildDir $FreshnessFile
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Wiping $BuildDir..."
    Remove-Item $BuildDir -Recurse -Force
}
if (-not (Test-Path $freshnessPath)) {
    if (Test-Path $BuildDir) {
        Write-Host "Stale build dir detected - wiping..."
        Remove-Item $BuildDir -Recurse -Force
    }
    Write-Host "Configuring ($Toolchain)..."
    cmake -S $ProjectRoot -B $BuildDir `
        @GeneratorArgs `
        "-DCMAKE_PREFIX_PATH=$QtRoot" `
        @CompilerArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# --- Build --------------------------------------------------------------------
Write-Host "Building (Debug, $Nproc jobs)..."
cmake --build $BuildDir --target msga --parallel $Nproc @BuildConfigArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# --- Deploy Qt DLLs -----------------------------------------------------------
# windeployqt copies all required Qt + runtime DLLs next to the exe so it runs
# without PATH changes or a Qt installation on the target machine.
if ($Toolchain -eq 'msvc') {
    $ExeDir  = Join-Path $BuildDir 'Debug'
    $Exe     = Join-Path $ExeDir 'msga.exe'
    $WinDeploy = Join-Path $QtRoot 'bin\windeployqt.exe'
} else {
    $ExeDir  = $BuildDir
    $Exe     = Join-Path $ExeDir 'msga.exe'
    $WinDeploy = "$MSYS2_ROOT\mingw64\bin\windeployqt6.exe"
    if (-not (Test-Path $WinDeploy)) {
        $WinDeploy = "$MSYS2_ROOT\mingw64\bin\windeployqt.exe"
    }
}

if ((Test-Path $Exe) -and (Test-Path $WinDeploy)) {
    Write-Host "Deploying Qt DLLs to $ExeDir..."
    & $WinDeploy --debug $Exe
    if ($LASTEXITCODE -ne 0) { Write-Host "warning: windeployqt exited $LASTEXITCODE" -ForegroundColor Yellow }
} else {
    if (-not (Test-Path $Exe))       { Write-Host "warning: exe not found at $Exe" -ForegroundColor Yellow }
    if (-not (Test-Path $WinDeploy)) { Write-Host "warning: windeployqt not found at $WinDeploy" -ForegroundColor Yellow }
}

Write-Host "Output: $Exe"
