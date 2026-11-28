#Requires -Version 5.1
# Install what building msga needs on Windows, then check it: MSYS2 with its
# mingw64 GCC toolchain (MSVC is not supported, see msys2.ps1), git, and
# clang-format. FreeType/HarfBuzz are built by msga's own CMake.
# Run as Administrator from the repository root (MSYS2 installs to C:\msys64,
# or $env:MSYS2_ROOT):
#   powershell -ExecutionPolicy Bypass -File scripts\configure-windows.ps1
#
# The shipped Windows release is cross-compiled on Linux in Docker
# (scripts/release-windows.sh); scripts\release.ps1 builds the same exe here.

$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
. (Join-Path $ScriptDir 'msys2.ps1') # $MsysRoot, $MsysPkgs, die

$CMAKE_MIN_VER          = [version]'3.21'
# The tree is formatted with clang-format >= 22; older releases produce DIFFERENT
# output from the same .clang-format (see scripts/clang-format-env.sh).
$CLANG_FORMAT_MIN_MAJOR = 22

function ok($msg)   { Write-Host ("  ok    " + $msg) -ForegroundColor Green }
function miss($msg) { Write-Host ("  miss  " + $msg) -ForegroundColor Yellow }
function info($msg) { Write-Host ("  info  " + $msg) -ForegroundColor Cyan }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { die "Run this script as Administrator." }

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    die "winget not found. Update Windows or install 'App Installer' from the Microsoft Store."
}
ok "winget $(winget --version)"

# --- Git ----------------------------------------------------------------------
Write-Host ""
Write-Host "--- Git"
if (Get-Command git -ErrorAction SilentlyContinue) {
    ok "$(git --version)"
} else {
    miss "git - installing..."
    winget install --id Git.Git --silent --accept-package-agreements --accept-source-agreements
    $env:PATH += ";$env:ProgramFiles\Git\cmd"
    ok "git installed"
}

# --- MSYS2 + mingw64 toolchain ------------------------------------------------
Write-Host ""
Write-Host "--- MSYS2 (mingw64 GCC, cmake, ninja, python)"
if (-not (Test-Path "$MsysRoot\usr\bin\bash.exe")) {
    miss "MSYS2 not found - downloading the installer..."
    $installer = "$env:TEMP\msys2-installer.exe"
    Invoke-WebRequest `
        -Uri "https://github.com/msys2/msys2-installer/releases/latest/download/msys2-x86_64-latest.exe" `
        -OutFile $installer -UseBasicParsing
    Write-Host "  Installing MSYS2 to $MsysRoot..."
    & $installer in --confirm-command --accept-messages --default-answer --root $MsysRoot
    if (-not (Test-Path "$MsysRoot\usr\bin\bash.exe")) {
        die "MSYS2 installation failed (exit $LASTEXITCODE)."
    }
    ok "MSYS2 installed at $MsysRoot"
} else {
    ok "MSYS2 at $MsysRoot"
}

$bash = "$MsysRoot\usr\bin\bash.exe"
# Twice: the first pass may update pacman itself and stop there.
Write-Host "  Updating MSYS2 packages..."
& $bash -lc "pacman --noconfirm --noprogressbar -Syuu" 2>&1 | Out-Null
& $bash -lc "pacman --noconfirm --noprogressbar -Syuu" 2>&1 | Out-Null
# python: the icon/translation generators and the tests' fake servers.
$pkgs = "$MsysPkgs mingw-w64-x86_64-python"
Write-Host "  Installing $pkgs..."
& $bash -lc "pacman --noconfirm --noprogressbar -S --needed $pkgs"
if ($LASTEXITCODE -ne 0) { die "MSYS2 package installation failed." }

Use-Mingw64 # checks every tool the build needs, puts mingw64 first on PATH
$cmakeVer = (cmake --version | Select-String '\d+\.\d+\.\d+').Matches[0].Value
if ([version]$cmakeVer -lt $CMAKE_MIN_VER) { die "cmake $cmakeVer is too old (>= $CMAKE_MIN_VER required)." }
ok "g++ $((& g++ -dumpversion))"
ok "cmake $cmakeVer (>= $CMAKE_MIN_VER required)"
ok "ninja $(ninja --version)"

# --- clang-format -------------------------------------------------------------
# Visual Studio's bundled clang-format is too old, and the pre-commit hook
# rejects files formatted by anything below the floor. The LLVM winget package
# tracks the current release and puts clang-format.exe on PATH.
Write-Host ""
Write-Host "--- clang-format"
function Get-ClangFormatMajor {
    $cf = Get-Command clang-format -ErrorAction SilentlyContinue
    if (-not $cf) { return $null }
    $m = (& $cf.Source --version 2>$null | Select-String 'version (\d+)\.').Matches
    if ($m.Count -gt 0) { return [int]$m[0].Groups[1].Value }
    return $null
}
$cfMajor = Get-ClangFormatMajor
if ($cfMajor -and $cfMajor -ge $CLANG_FORMAT_MIN_MAJOR) {
    ok "clang-format $cfMajor (>= $CLANG_FORMAT_MIN_MAJOR required)"
} else {
    if ($cfMajor) { miss "clang-format $cfMajor is too old (>= $CLANG_FORMAT_MIN_MAJOR required) - installing LLVM..." }
    else          { miss "clang-format - installing LLVM..." }
    winget install --id LLVM.LLVM --silent --accept-package-agreements --accept-source-agreements
    $env:PATH += ";$env:ProgramFiles\LLVM\bin"
    $cfMajor = Get-ClangFormatMajor
    if ($cfMajor -and $cfMajor -ge $CLANG_FORMAT_MIN_MAJOR) {
        ok "clang-format $cfMajor installed"
    } else {
        info "clang-format >= $CLANG_FORMAT_MIN_MAJOR still not first on PATH - put $env:ProgramFiles\LLVM\bin ahead of older copies"
        info "(or: pip install clang-format). Do NOT format with an older release: its output differs."
    }
}

# --- Repository ---------------------------------------------------------------
Write-Host ""
Write-Host "--- Repository"
git -C $ProjectRoot config core.hooksPath .githooks
ok "git hooks (.githooks/pre-commit)"
if (-not (Test-Path (Join-Path $ProjectRoot 'credentials.cmake'))) {
    miss "credentials.cmake - builds work without it, but sign-in needs the Slack app keys"
    info "(copy credentials.cmake.example and fill it in)"
}

Write-Host ""
Write-Host "Done. Build and test with:"
Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\build.ps1 --test"
Write-Host "Release exe (build-release\msga.exe):"
Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\release.ps1"
