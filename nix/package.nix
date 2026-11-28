# msga as a Nix package: `nix build` from the flake, or from another
# configuration `pkgs.callPackage ./nix/package.nix { }`.
#
# A dynamically linked build against nixpkgs' libraries, with the release
# toolchain (clang + lld, MinSizeRel). The static musl binary of the releases
# comes from scripts/release-linux-static.sh, not from here.
{
  lib,
  llvmPackages,
  fetchurl,
  cmake,
  ninja,
  pkg-config,
  wayland-scanner,
  makeWrapper,
  dbus,
  freetype,
  harfbuzz,
  libxcb,
  libxcb-cursor,
  libxkbcommon,
  wayland,
  wayland-protocols,
  inter,
  dejavu_fonts,
  noto-fonts-color-emoji,
  # msga has its own font index (no fontconfig) over the XDG data dirs' fonts/,
  # which on NixOS are empty: these are the fonts it then starts with.
  fonts ? [
    inter
    dejavu_fonts
    noto-fonts-color-emoji
  ],
}:

let
  # set(MSGA_VERSION 39) in version.cmake.
  version = lib.head (
    builtins.match ".*MSGA_VERSION ([0-9]+).*" (builtins.readFile ../version.cmake)
  );

  # The tarball src/net/posix/posix.cmake fetches, same hash: the sandbox has
  # no network, so it is handed over through FETCHCONTENT_SOURCE_DIR.
  mbedtls = fetchurl {
    url = "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2";
    sha256 = "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6";
  };
in
llvmPackages.stdenv.mkDerivation {
  pname = "msga";
  inherit version;

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../CMakeLists.txt
      ../version.cmake
      (lib.fileset.maybeMissing ../credentials.cmake)
      ../gfx
      ../sfx
      ../src
    ];
  };

  postUnpack = ''
    mkdir mbedtls
    tar -xf ${mbedtls} -C mbedtls --strip-components=1
  '';

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    wayland-scanner
    makeWrapper
    # ld.lld behind nixpkgs' linker wrapper (it adds the libraries' rpaths).
    llvmPackages.bintools
  ];

  buildInputs = [
    dbus
    freetype
    harfbuzz
    libxcb
    libxcb-cursor
    libxkbcommon
    wayland
    wayland-protocols
  ];

  preConfigure = ''
    cmakeFlagsArray+=("-DFETCHCONTENT_SOURCE_DIR_MSGA_MBEDTLS=$NIX_BUILD_TOP/mbedtls")
  '';
  cmakeBuildType = "MinSizeRel";
  cmakeFlags = [
    (lib.cmakeBool "MSGA_SIZE_MAP" false)
    # Nix installs the launcher entry and the updates.
    (lib.cmakeBool "MSGA_SELF_UPDATE" false)
    (lib.cmakeBool "MSGA_INSTALL_LAUNCHER" false)
  ];
  # Not the galleries and size probes of the default target.
  ninjaFlags = [ "msga" ];

  # The project has no install rules: build/msga is the whole program.
  installPhase = ''
    runHook preInstall
    install -Dm755 msga $out/bin/msga
    install -Dm644 $src/gfx/msga.desktop $out/share/applications/msga.desktop
    install -Dm644 $src/gfx/icon_256.png $out/share/icons/hicolor/256x256/apps/msga.png
    install -Dm644 $src/gfx/icon.svg $out/share/icons/hicolor/scalable/apps/msga.svg
    mkdir -p $out/share/fonts
    for f in ${lib.escapeShellArgs fonts}; do
      ln -s $f/share/fonts $out/share/fonts/$(basename $f)
    done
    runHook postInstall
  '';

  # The package's share/ (the fonts above; msga.desktop, the msga:// handler)
  # and, with fonts.fontDir.enable, NixOS's fonts join the data dirs; the
  # profiles' Hunspell dictionaries (hunspellDicts.*) join $DICPATH.
  postFixup = ''
    wrapProgram $out/bin/msga \
      --suffix XDG_DATA_DIRS : $out/share:/run/current-system/sw/share/X11 \
      --run 'export DICPATH="''${DICPATH:+$DICPATH:}$HOME/.nix-profile/share/hunspell:/etc/profiles/per-user/$USER/share/hunspell:/run/current-system/sw/share/hunspell"'
  '';

  meta = {
    description = "Fast native Slack client";
    homepage = "https://github.com/punarinta/make-slack-great-again";
    license = lib.licenses.gpl3Plus;
    mainProgram = "msga";
    platforms = lib.platforms.linux;
  };
}
