{
  description = "toy-mysql-storage-engine dev environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
        isLinux = pkgs.stdenv.isLinux;
      in
      {
        # https://nixos.wiki/wiki/Using_Clang_instead_of_GCC
        devShells.default = pkgs.mkShell.override { stdenv = pkgs.llvmPackages_21.stdenv; } {
          buildInputs =
            with pkgs;
            [
              cmake
              gnumake
              bison
              pkg-config
              openssl
              zlib
              ncurses
              clang-tools
              mise
            ]
            ++ lib.optionals isLinux [
              libtirpc
            ];

          # shellHook = ''
          #   mise run mysql:gen-error
          # '';

          CMAKE_PREFIX_PATH = "${pkgs.openssl.dev}";
        };

        apps.lint = flake-utils.lib.mkApp {
          drv = pkgs.writeShellScriptBin "lint" ''
            find src -name "*.cc" -o -name "*.h" | xargs ${pkgs.clang-tools}/bin/clang-tidy -p mysql-server/build "$@"
          '';
        };

        apps.lint-fix = flake-utils.lib.mkApp {
          drv = pkgs.writeShellScriptBin "lint-fix" ''
            find src -name "*.cc" -o -name "*.h" | xargs ${pkgs.clang-tools}/bin/clang-tidy --fix -p mysql-server/build "$@"
          '';
        };

        formatter = pkgs.nixfmt-tree;
      }
    );
}
