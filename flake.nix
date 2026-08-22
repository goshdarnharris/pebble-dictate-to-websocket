{
  description = "A new Pebble app";

  inputs = {
    pebble.url = "github:pebble-dev/pebble.nix";
    flake-utils.url = "github:numtide/flake-utils";
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { pebble, flake-utils, nixpkgs, ... }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [ pebble.overlays.default ];
        };
      in
      {
        devShell = pebble.pebbleEnv.${system} {
          packages = with pkgs; [
            llvmPackages_21.clangNoLibc.cc
            bear
          ];
        };
      }
    );
}