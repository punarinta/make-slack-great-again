# Shared by build.ps1 and release.ps1 (dot-sourced): the MSYS2 mingw64
# toolchain msga builds with on Windows, and deploying the DLLs it links.
# MSVC is not supported: the build relies on GCC/Clang flags (see CMakeLists.txt).

$ErrorActionPreference = 'Stop'

function die($msg) { Write-Host "error: $msg" -ForegroundColor Red; exit 1 }

$MsysRoot   = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT.TrimEnd('\', '/') } else { 'C:\msys64' }
$Mingw      = Join-Path $MsysRoot 'mingw64'
$MingwBin   = Join-Path $Mingw 'bin'
# FreeType and HarfBuzz are built by msga's own CMake (src/text/font_libs.cmake).
$MsysPkgs   = 'mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja'
$PacmanHint = "$MsysRoot\usr\bin\bash -lc 'pacman -S --needed $MsysPkgs'"

# Puts mingw64 first on PATH (this process only) and checks every tool and
# library the build needs, so a missing piece fails here with the fix rather
# than halfway through CMake.
function Use-Mingw64 {
    if (-not (Test-Path "$MsysRoot\usr\bin\bash.exe")) {
        die "MSYS2 not found at $MsysRoot (install it from https://www.msys2.org, or set MSYS2_ROOT)."
    }
    foreach ($exe in 'g++.exe', 'cmake.exe', 'ninja.exe', 'objdump.exe') {
        if (-not (Test-Path (Join-Path $MingwBin $exe))) {
            die "$exe missing from $MingwBin. Install the toolchain with:`n  $PacmanHint"
        }
    }
    $env:PATH = "$MingwBin;$env:PATH"
}

# The DLL names an exe/DLL imports (objdump's "DLL Name:" lines).
function Get-DllImports($file) {
    & (Join-Path $MingwBin 'objdump.exe') -p $file |
        Select-String -Pattern '^\s*DLL Name:\s*(\S+)' |
        ForEach-Object { $_.Matches[0].Groups[1].Value }
}

# Copies every DLL the exe needs from mingw64\bin next to it, transitively, so
# it starts from Explorer without MSYS2 on PATH. Windows' own DLLs (not in
# mingw64\bin) are left alone.
function Copy-MingwDlls($exe) {
    $dest  = Split-Path -Parent $exe
    $seen  = @{}
    $queue = New-Object System.Collections.Queue
    $queue.Enqueue($exe)
    while ($queue.Count -gt 0) {
        foreach ($dll in Get-DllImports $queue.Dequeue()) {
            $key = $dll.ToLowerInvariant()
            if ($seen.ContainsKey($key)) { continue }
            $seen[$key] = $true
            $src = Join-Path $MingwBin $dll
            if (-not (Test-Path $src)) { continue } # a system DLL
            Copy-Item $src -Destination $dest -Force
            $queue.Enqueue($src)
        }
    }
}
