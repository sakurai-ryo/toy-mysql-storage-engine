{
  description = "toy-mysql-storage-engine dev environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      {
        # https://nixos.wiki/wiki/Using_Clang_instead_of_GCC
        devShells.default = pkgs.mkShell.override { stdenv = pkgs.llvmPackages_22.stdenv; } {
          buildInputs = with pkgs; [
            cmake
            gnumake
            bison
            pkg-config
            openssl
            zlib
            ncurses
            clang-tools
            libtirpc
          ];

          shellHook = ''
            echo "toy-mysql-storage-engine dev shell"
          '';

          CMAKE_PREFIX_PATH = "${pkgs.openssl.dev}";
        };
      }
    );
}
