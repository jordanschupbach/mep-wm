{
  description = "A basic flake";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  inputs.systems.url = "github:nix-systems/default";
  inputs.flake-utils = {
    url = "github:numtide/flake-utils";
    inputs.systems.follows = "systems";
  };

  outputs =
    { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        devShells.default = pkgs.mkShell { 
          XKB_CONFIG_ROOT = "${pkgs.xkeyboard_config}/share/X11/xkb";
          packages = [
            pkgs.hello
            pkgs.cmake
            pkgs.gcc
            pkgs.pkg-config
            pkgs.wlroots
            pkgs.wayland
            pkgs.wayland-protocols
            pkgs.libxcb
            pkgs.libffi
            pkgs.libx11
            pkgs.libXinerama
            pkgs.libXft
            pkgs.lua
            pkgs.libxkbcommon
            pkgs.libxdmcp
            pkgs.xorg-server
            pkgs.xkbcomp
            pkgs.xkeyboard_config
            pkgs.xdotool
            pkgs.xterm
          ]; 
        };
      }
    );
}
