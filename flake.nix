{
  description = "msga: a fast native Slack client";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: rec {
        msga = pkgs.callPackage ./nix/package.nix { };
        default = msga;
      });

      overlays.default = final: _prev: {
        msga = final.callPackage ./nix/package.nix { };
      };

      # `nix develop`: the package's toolchain and libraries, for scripts/build.sh.
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.msga ];
          packages = [ pkgs.python3 ];
        };
      });
    };
}
