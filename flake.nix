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
              clang-tools
              bison
              pkg-config
              openssl
              zlib
              ncurses
              mise
            ]
            ++ lib.optionals isLinux [
              libtirpc
            ];

          # shellHook = ''
          #   mise run mysql:gen-error
          # '';

          # Expose libc++ include path for clang-tidy.
          # The Nix CC wrapper implicitly adds libc++ headers when compiling, but compile_commands.json only records
          # the explicit flags passed to the wrapper.
          # clang-tidy uses its own frontend (not the wrapper), so it cannot discover these implicit paths.
          # We pass this via --extra-arg in the lint task (see mise.toml).
          # https://nixos.org/manual/nixpkgs/stable/#bintools-wrapper
          LIBCXX_INCLUDE_PATH = "${pkgs.llvmPackages_21.libcxx.dev}/include/c++/v1";
        };

        formatter = pkgs.nixfmt-tree;
      }
    );
}
