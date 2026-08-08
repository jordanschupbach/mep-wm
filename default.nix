{ inputs ? {}, pkgs ? inputs.nixpkgs.legacyPackages.x86_64-linux }:
pkgs.stdenv.mkDerivation {

  pname = "mep-wm";
  version = "0.1.0";
  src = ./.;

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.pkg-config
  ];

  buildInputs = [
    pkgs.libx11
    pkgs.libXinerama
    pkgs.libXft
    pkgs.lua
    pkgs.dbus
    pkgs.wlroots
    pkgs.wayland
    pkgs.wayland-protocols
    pkgs.libffi
    pkgs.pixman
    pkgs.libxkbcommon
  ];

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin $out/share/mep-wm
    cp mepwm $out/bin/
    cp -r assets $out/share/mep-wm/assets
    runHook postInstall
  '';

  meta = with pkgs.lib; {
    description = "MEP-wm (Mise En Place window manager)";
    homepage = "";
    license = licenses.mit;
    maintainers = [ ];
  };
}
