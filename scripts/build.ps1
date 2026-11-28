#Requires -Version 5.1
# Build msga on Windows with MSYS2's mingw64 GCC (the Windows twin of build.sh).
# Usage:
#   scripts\build.ps1           # MinSizeRel -> build\msga.exe
#   scripts\build.ps1 --debug   # Debug      -> build-debug\msga.exe
#   scripts\build.ps1 --test    # also build and run the test suite (combines with --debug)
#   scripts\build.ps1 --demo    # also compile the demo workspace (msga --demo demo)
# --test and --demo switch the option on in that build dir; it stays on there.
# Needs MSYS2 (C:\msys64, or set MSYS2_ROOT) with the packages msys2.ps1 lists.

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
. (Join-Path $ScriptDir 'msys2.ps1')

$DebugBuild = $false
$Test       = $false
$Options    = @()
foreach ($arg in $args) {
    switch ($arg) {
        { $_ -in '--debug', '-debug' } { $DebugBuild = $true }
        { $_ -in '--test', '-test' }   { $Test = $true; $Options += '-DMSGA_BUILD_TESTS=ON' }
        { $_ -in '--demo', '-demo' }   { $Options += '-DMSGA_DEMO=ON' }
        default { die "unknown argument: $arg" }
    }
}

Use-Mingw64
$Nproc = $env:NUMBER_OF_PROCESSORS

# Size is the default build: the release flags are what the size budgets
# measure. Debug gets its own dir so the two never share objects.
if ($DebugBuild) {
    $BuildDir  = Join-Path $ProjectRoot 'build-debug'
    $BuildType = 'Debug'
} else {
    $BuildDir  = Join-Path $ProjectRoot 'build'
    $BuildType = 'MinSizeRel'
}

# Only configure when not already a Ninja build. Wipe a directory that exists
# without build.ninja (stale or wrong generator).
if (-not (Test-Path (Join-Path $BuildDir 'build.ninja'))) {
    if (Test-Path $BuildDir) { Remove-Item $BuildDir -Recurse -Force }
    cmake -S $ProjectRoot -B $BuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$BuildType" @Options
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} elseif ($Options.Count -gt 0) {
    cmake $BuildDir @Options
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

cmake --build $BuildDir --parallel $Nproc
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    ctest --test-dir $BuildDir --output-on-failure -j $Nproc
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# The GCC runtime DLLs next to the exe, so it runs from Explorer or a shell
# without mingw64 on PATH.
$Exe = Join-Path $BuildDir 'msga.exe'
Copy-MingwDlls $Exe
Write-Host "Built: $Exe"
