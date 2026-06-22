#Requires -Version 5.1
# Install build dependencies for msga on Windows.
# Tries MSVC (VS Build Tools 2022) first.
# If blocked by organizational policy, installs MSYS2 + MinGW automatically.
# Must be run as Administrator from the repository root.

$ErrorActionPreference = 'Stop'

$CMAKE_MIN_VER  = [version]'3.21'
$QT_VERSION     = '6.9.1'
$PYTHON_VERSION = '3.12.10'
$MSYS2_ROOT     = 'C:\msys64'

function ok($msg)   { Write-Host ("  ok    " + $msg) -ForegroundColor Green }
function miss($msg) { Write-Host ("  miss  " + $msg) -ForegroundColor Yellow }
function info($msg) { Write-Host ("  info  " + $msg) -ForegroundColor Cyan }
function die($msg)  { Write-Host ("  error " + $msg) -ForegroundColor Red; exit 1 }

# --- Elevation check ----------------------------------------------------------
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    die "This script must be run as Administrator."
}

# --- winget -------------------------------------------------------------------
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    die "winget not found. Update Windows or install 'App Installer' from the Microsoft Store."
}
ok "winget $(winget --version)"

# --- Git ----------------------------------------------------------------------
Write-Host ""
Write-Host "--- Git"
if (Get-Command git -ErrorAction SilentlyContinue) {
    ok "git $(git --version)"
} else {
    miss "git - installing..."
    winget install --id Git.Git --silent --accept-package-agreements --accept-source-agreements
    $env:PATH += ";$env:ProgramFiles\Git\cmd"
    ok "git installed"
}

# --- CMake --------------------------------------------------------------------
Write-Host ""
Write-Host "--- CMake"
$cmakeOk = $false
if (Get-Command cmake -ErrorAction SilentlyContinue) {
    $cmakeVerStr = (cmake --version | Select-String '\d+\.\d+\.\d+').Matches[0].Value
    if ([version]$cmakeVerStr -ge $CMAKE_MIN_VER) {
        ok "cmake $cmakeVerStr (>= $CMAKE_MIN_VER required)"
        $cmakeOk = $true
    } else {
        miss "cmake $cmakeVerStr is too old (>= $CMAKE_MIN_VER required) - upgrading..."
    }
}
if (-not $cmakeOk) {
    winget install --id Kitware.CMake --silent --accept-package-agreements --accept-source-agreements
    $env:PATH += ";$env:ProgramFiles\CMake\bin"
    $cmakeVerStr = (cmake --version | Select-String '\d+\.\d+\.\d+').Matches[0].Value
    ok "cmake $cmakeVerStr"
}

# --- Ninja --------------------------------------------------------------------
Write-Host ""
Write-Host "--- Ninja"
if (Get-Command ninja -ErrorAction SilentlyContinue) {
    ok "ninja $(ninja --version)"
} else {
    miss "ninja - installing..."
    winget install --id Ninja-build.Ninja --silent --accept-package-agreements --accept-source-agreements
    $ninjaExe = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Ninja-build.Ninja_*\ninja.exe" `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($ninjaExe) { $env:PATH += ";$($ninjaExe.DirectoryName)" }
    ok "ninja installed"
}

# --- C++ compiler: try MSVC, fall back to MSYS2/MinGW ----------------------
Write-Host ""
Write-Host "--- C++ compiler"

$msvcOk  = $false
$msys2Ok = $false

$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vsWhere) {
    $vsInfo = & $vsWhere -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -latest -format json | ConvertFrom-Json
    if ($vsInfo) {
        ok "MSVC ($($vsInfo.displayName))"
        $msvcOk = $true
    }
}

if (-not $msvcOk) {
    miss "MSVC not found - attempting VS Build Tools 2022 via winget..."
    winget install --id Microsoft.VisualStudio.2022.BuildTools `
        --silent --accept-package-agreements --accept-source-agreements `
        --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    if ($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq -1978335189) {
        ok "VS Build Tools 2022 installed"
        $msvcOk = $true
    } elseif ($LASTEXITCODE -eq -1978334961) {
        info "VS Build Tools blocked by org policy - switching to MSYS2/MinGW"
    } else {
        info "VS Build Tools failed (exit $LASTEXITCODE) - switching to MSYS2/MinGW"
    }
}

# --- MSYS2 + MinGW (fallback when MSVC is unavailable) ----------------------
if (-not $msvcOk) {
    Write-Host ""
    Write-Host "--- MSYS2 + MinGW"

    if (-not (Test-Path "$MSYS2_ROOT\usr\bin\bash.exe")) {
        miss "MSYS2 not found - downloading installer..."
        $installer = "$env:TEMP\msys2-installer.exe"
        Invoke-WebRequest `
            -Uri "https://github.com/msys2/msys2-installer/releases/latest/download/msys2-x86_64-latest.exe" `
            -OutFile $installer -UseBasicParsing
        Write-Host "  Installing MSYS2 to $MSYS2_ROOT..."
        & $installer in --confirm-command --accept-messages --default-answer --root $MSYS2_ROOT
        if ($LASTEXITCODE -ne 0 -and -not (Test-Path "$MSYS2_ROOT\usr\bin\bash.exe")) {
            die "MSYS2 installation failed (exit $LASTEXITCODE)."
        }
        ok "MSYS2 installed at $MSYS2_ROOT"
    } else {
        ok "MSYS2 already at $MSYS2_ROOT"
    }

    $bash = "$MSYS2_ROOT\usr\bin\bash.exe"

    # Update package databases (run twice - first pass may self-update pacman)
    Write-Host "  Updating MSYS2 packages (first pass)..."
    & $bash -lc "pacman --noconfirm --noprogressbar -Syuu" 2>&1 | Out-Null
    Write-Host "  Updating MSYS2 packages (second pass)..."
    & $bash -lc "pacman --noconfirm --noprogressbar -Syuu" 2>&1 | Out-Null

    # Install MinGW-w64 toolchain + Qt6 modules needed by the project
    $pkgs = "mingw-w64-x86_64-gcc " +
            "mingw-w64-x86_64-cmake " +
            "mingw-w64-x86_64-ninja " +
            "mingw-w64-x86_64-qt6-base " +
            "mingw-w64-x86_64-qt6-websockets " +
            "mingw-w64-x86_64-qt6-svg " +
            "mingw-w64-x86_64-qt6-tools " +
            "mingw-w64-x86_64-python"
    Write-Host "  Installing MinGW toolchain + Qt6 (may take several minutes)..."
    & $bash -lc "pacman --noconfirm --noprogressbar -S --needed $pkgs"
    if ($LASTEXITCODE -ne 0) { die "MSYS2 package installation failed." }

    # Verify Qt6 landed
    $qtCheck = & $bash -lc "test -f /mingw64/lib/cmake/Qt6/Qt6Config.cmake && echo found || echo missing"
    if ($qtCheck -match 'found') {
        ok "Qt6 installed at $MSYS2_ROOT\mingw64"
    } else {
        die "Qt6 not found in MSYS2 after package install."
    }
    $msys2Ok = $true
}

# --- Python + Qt (MSVC path only; MSYS2 already has both) -------------------
if ($msvcOk) {
    Write-Host ""
    Write-Host "--- Python"

    # Find the real python.exe - never trust PATH resolution because the Windows
    # Store stub (Microsoft\WindowsApps\python.exe) shadows real installs in PATH
    # and exits non-zero. Always resolve by file path.
    function Find-PythonExe {
        $candidates = @(
            "C:\Program Files\Python3*\python.exe",
            "C:\Python3*\python.exe",
            "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe"
        )
        foreach ($pat in $candidates) {
            $hit = Get-Item $pat -ErrorAction SilentlyContinue |
                Sort-Object FullName -Descending | Select-Object -First 1
            if ($hit) { return $hit.FullName }
        }
        return $null
    }

    $pyExe = Find-PythonExe
    if ($pyExe) {
        $pyVer = & $pyExe --version 2>$null
        ok "python $pyVer at $pyExe"
    } else {
        miss "real Python not found - downloading installer..."
        $pyInst = "$env:TEMP\python-installer.exe"
        Invoke-WebRequest "https://www.python.org/ftp/python/$PYTHON_VERSION/python-$PYTHON_VERSION-amd64.exe" `
            -OutFile $pyInst -UseBasicParsing
        & $pyInst /quiet InstallAllUsers=1 PrependPath=1 Include_test=0
        if ($LASTEXITCODE -ne 0) { die "Python installation failed (exit $LASTEXITCODE)." }
        $pyExe = Find-PythonExe
        if (-not $pyExe) { die "Python installed but python.exe not found in expected locations." }
        ok "Python $PYTHON_VERSION installed at $pyExe"
    }

    Write-Host ""
    Write-Host "--- Qt $QT_VERSION (MSVC)"
    $qtRoot  = "C:\Qt\$QT_VERSION\msvc2022_64"
    $wsCmake = "$qtRoot\lib\cmake\Qt6WebSockets\Qt6WebSocketsConfig.cmake"
    $needBase = -not (Test-Path "$qtRoot\lib\cmake\Qt6\Qt6Config.cmake")
    $needWS   = -not (Test-Path $wsCmake)
    if (-not $needBase -and -not $needWS) {
        ok "Qt $QT_VERSION at $qtRoot"
    } else {
        & $pyExe -m pip install --quiet --upgrade pip
        & $pyExe -m pip install --quiet aqtinstall
        if ($needBase) {
            miss "Qt $QT_VERSION - installing base + websockets via aqtinstall..."
            Write-Host "  Downloading Qt $QT_VERSION (win64_msvc2022_64) to C:\Qt..."
            & $pyExe -m aqt install-qt windows desktop $QT_VERSION win64_msvc2022_64 `
                --outputdir C:\Qt --modules qtwebsockets
        } else {
            miss "Qt $QT_VERSION base present but WebSockets missing - adding module..."
            & $pyExe -m aqt install-qt windows desktop $QT_VERSION win64_msvc2022_64 `
                --outputdir C:\Qt --modules qtwebsockets
        }
        if (-not (Test-Path "$qtRoot\lib\cmake\Qt6\Qt6Config.cmake")) {
            die ("Qt $QT_VERSION install failed. " +
                 "Re-run or install manually: https://www.qt.io/download-qt-installer")
        }
        ok "Qt $QT_VERSION at $qtRoot"
    }
}

# --- git hooks ----------------------------------------------------------------
Write-Host ""
Write-Host "--- git hooks"
git config core.hooksPath .githooks
ok "git hooks (.githooks/pre-commit)"

# --- Summary ------------------------------------------------------------------
$toolchain = if ($msvcOk) { 'msvc' } else { 'msys2' }
Write-Host ""
Write-Host "All dependencies satisfied (toolchain: $toolchain)."
Write-Host ""
Write-Host "Build:"
Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\build.ps1"
if ($msvcOk) {
    $qtRoot = "C:\Qt\$QT_VERSION\msvc2022_64"
    Write-Host ""
    Write-Host "Or manually:"
    Write-Host "  cmake -B build -S . -G ""Visual Studio 17 2022"" -A x64 -DCMAKE_PREFIX_PATH=""$qtRoot"""
} else {
    Write-Host ""
    Write-Host "Or manually:"
    Write-Host "  cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:\msys64\mingw64"
}
