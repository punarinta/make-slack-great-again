#Requires -Version 5.1
# Release build of msga on Windows -> build-release\msga.exe, one self-contained
# exe like scripts/release-windows.sh cross-compiles (MSGA_STATIC: the GCC
# runtime linked in; FreeType/HarfBuzz built by msga's CMake). The Windows twin
# of release.sh. MSYS2 mingw64 GCC, see msys2.ps1.
# Usage:
#   scripts\release.ps1        # MinSizeRel, stripped
#
# Always a clean configure of the release flags: no tests, no demo workspace
# (the fake backend must never ship), whatever an earlier build dir had cached.

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
. (Join-Path $ScriptDir 'msys2.ps1')

if ($args.Count -gt 0) { die "unknown argument: $($args[0])" }

Use-Mingw64
$BuildDir = Join-Path $ProjectRoot 'build-release'
$Nproc    = $env:NUMBER_OF_PROCESSORS

# Wipe a directory that exists without build.ninja (stale or wrong generator).
if ((Test-Path $BuildDir) -and -not (Test-Path (Join-Path $BuildDir 'build.ninja'))) {
    Remove-Item $BuildDir -Recurse -Force
}

# Reconfigure every time: MSGA_DEMO / MSGA_BUILD_TESTS are cached options, so a
# hand-run `cmake -DMSGA_DEMO=ON build-release` would otherwise stick.
cmake -S $ProjectRoot -B $BuildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=MinSizeRel `
    -DMSGA_STATIC=ON `
    -DMSGA_DEMO=OFF `
    -DMSGA_BUILD_TESTS=OFF `
    -DMSGA_SIZE_MAP=ON
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $BuildDir --target msga --parallel $Nproc
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# mingw links with ld.bfd and no -s (see CMakeLists.txt), so strip here.
$Exe = Join-Path $BuildDir 'msga.exe'
& (Join-Path $MingwBin 'strip.exe') $Exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Copy-MingwDlls $Exe # nothing left to copy in a static link; kept as a guard

# The size report needs an lld map; ld.bfd builds have none (yet).
$Map = Join-Path $BuildDir 'msga.map'
if ((Test-Path $Map) -and (Get-Command python3 -ErrorAction SilentlyContinue)) {
    python3 (Join-Path $ProjectRoot 'src\tools\size_report.py') $Map
    Write-Host ''
}

Write-Host "Binary:  $Exe"
Get-ChildItem $BuildDir -File | Where-Object { $_.Extension -in '.exe', '.dll' } |
    Sort-Object Name | Format-Table Name, @{ Label = 'KB'; Expression = { [math]::Round($_.Length / 1KB) } } -AutoSize
