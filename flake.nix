{
  description = "MEP-wm: tiling window manager (native on Linux/X11, overlay on macOS)";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  inputs.systems.url = "github:nix-systems/default";
  inputs.flake-utils = {
    url = "github:numtide/flake-utils";
    inputs.systems.follows = "systems";
  };

  outputs =
    { self, nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        inherit (pkgs) lib stdenv;

        # Libraries the X11/Wayland backends link against. macOS builds need
        # none of these: the overlay backend uses the system frameworks
        # (AppKit/Carbon/ApplicationServices) that stdenv's Apple SDK provides.
        linuxBuildDeps = with pkgs; [
          wlroots
          wayland
          wayland-protocols
          libxcb
          libffi
          libx11
          libXinerama
          libXft
          fontconfig
          imlib2
          lua
          dbus
          libxkbcommon
          libxdmcp
          # wlroots' headers (wlr_output.h) pull in pixman.h and libdrm at
          # compile time, and libxcb needs libXau; none of the three are
          # pulled in transitively, so they must be listed explicitly.
          pixman
          libdrm
          xorg.libXau
        ];

        # Extra tools for hacking on the Linux backends (nested Xephyr
        # session, benchmarks); meaningless on darwin.
        linuxDevTools = with pkgs; [
          gcc
          xorg-server
          xkbcomp
          xkeyboard_config
          xdotool
          xterm
        ];
      in
      {
        packages.default = stdenv.mkDerivation {
          pname = "mepwm";
          version = "0.1.0";
          src = self;
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.pkg-config
          ];
          buildInputs = lib.optionals stdenv.isLinux linuxBuildDeps;
          meta = {
            description = "MEP-wm tiling window manager";
            homepage = "https://github.com/jordanschupbach/mep-wm";
            mainProgram = "mepwm";
          };
        };

        devShells.default = pkgs.mkShell (
          {
            packages = [
              pkgs.cmake
              pkgs.pkg-config
              pkgs.just
              pkgs.clang-tools
              pkgs.cppcheck
            ]
            ++ lib.optionals stdenv.isLinux (linuxBuildDeps ++ linuxDevTools);
          }
          // lib.optionalAttrs stdenv.isLinux {
            XKB_CONFIG_ROOT = "${pkgs.xkeyboard_config}/share/X11/xkb";
          }
        );
      }
    );
}
