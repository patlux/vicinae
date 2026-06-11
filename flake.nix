{
  description = "A flake for Vicinae, a high-performance native launcher for Linux.";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs?ref=nixos-unstable";
    nixpkgs-darwin.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    systems.url = "github:nix-systems/default";
  };

  nixConfig = {
    extra-substituters = ["https://vicinae.cachix.org"];
    extra-trusted-public-keys = ["vicinae.cachix.org-1:1kDrfienkGHPYbkpNj1mWTr7Fm1+zcenzgTizIcI3oc="];
  };

  outputs = {
    self,
    nixpkgs,
    nixpkgs-darwin,
    systems,
  }: let
    inherit (nixpkgs-darwin) lib;
    allSystems = import systems;
    linuxSystems = lib.filter (system: lib.hasSuffix "-linux" system) allSystems;
    pkgsFor = system:
      if lib.hasSuffix "-darwin" system
      then nixpkgs-darwin.legacyPackages.${system}
      else nixpkgs.legacyPackages.${system};
    forEachPkgs = targetSystems: f: lib.genAttrs targetSystems (system: f (pkgsFor system));
  in {
    packages = forEachPkgs linuxSystems (pkgs: {
      default = pkgs.callPackage ./nix/vicinae.nix {gcc15Stdenv = pkgs.gcc15Stdenv;};
      nix-update-script = pkgs.writeShellScriptBin "nix-update-script" ''
        OLD_API_DEPS_HASH=$(${pkgs.lib.getExe pkgs.nix} eval --raw .#packages.x86_64-linux.default.apiDeps.hash)
        OLD_EXT_MAN_DEPS_HASH=$(${pkgs.lib.getExe pkgs.nix} eval --raw .#packages.x86_64-linux.default.extensionManagerDeps.hash)

        cd src/typescript/api
        NEW_API_DEPS_HASH=$(${pkgs.lib.getExe pkgs.prefetch-npm-deps} package-lock.json)
        cd ../extension-manager
        NEW_EXT_MAN_DEPS_HASH=$(${pkgs.lib.getExe pkgs.prefetch-npm-deps} package-lock.json)
        cd ..

        [[ "$OLD_API_DEPS_HASH" == "$NEW_API_DEPS_HASH" ]] || { echo -e "\e[31mHash mismatch for API npm deps, please replace the value in vicinae.nix with '$NEW_API_DEPS_HASH'.\e[0m" >&2; exit 1;}

        [[ "$OLD_EXT_MAN_DEPS_HASH" == "$NEW_EXT_MAN_DEPS_HASH" ]] || { echo -e "\e[31mHash mismatch for extension-manager npm deps, please replace the value in vicinae.nix with '$NEW_EXT_MAN_DEPS_HASH'.\e[0m" >&2; exit 1;}
      '';
    });
    lib = forEachPkgs linuxSystems (pkgs: {
      mkVicinaeExtension = pkgs.callPackage ./nix/mkVicinaeExtension.nix {};
      mkRayCastExtension = pkgs.callPackage ./nix/mkRayCastExtension.nix {};
    });
    devShells = forEachPkgs allSystems (
      pkgs: let
        qtEnv = pkgs.qt6.env "qt-custom-${pkgs.qt6.qtbase.version}" [
          pkgs.qt6.qtdeclarative
          pkgs.qt6.qtwayland
          pkgs.qt6.qtsvg
        ];
        linuxShell = pkgs.mkShell {
          stdenv = pkgs.gcc15Stdenv;

          # automatically pulls nativeBuildInputs + buildInputs
          inputsFrom = [(pkgs.callPackage ./nix/vicinae.nix {gcc15Stdenv = pkgs.gcc15Stdenv;})];

          packages = with pkgs; [
            ccache
            catch2_3
            qtEnv
            clang-tools
          ];

          shellHook = ''
            export CC=${pkgs.gcc15}/bin/gcc
            export CXX=${pkgs.gcc15}/bin/g++
            export CMAKE_C_COMPILER=$CC
            export CMAKE_CXX_COMPILER=$CXX
          '';
        };
      in {
        default =
          if pkgs.stdenv.isDarwin
          then pkgs.callPackage ./nix/darwin-dev-shell.nix {}
          else linuxShell;
      }
    );
    overlays.default = final: prev: {
      vicinae = final.callPackage ./nix/vicinae.nix {};
      mkVicinaeExtension = prev.callPackage ./nix/mkVicinaeExtension.nix {};
      mkRayCastExtension = prev.callPackage ./nix/mkRayCastExtension.nix {};
    };
    homeManagerModules.default = import ./nix/module.nix self;

    nixosModules.default = {pkgs, ...}: {
      security.wrappers.vicinae-input-server = {
        source = "${self.packages.${pkgs.stdenv.hostPlatform.system}.default}/libexec/vicinae/vicinae-input-server";
        capabilities = "cap_dac_override+ep";
        owner = "root";
        group = "root";
      };
    };
  };
}
