{
  pkgs,
  source,
  upstreamRevision,
  libplaceboSource,
  overlaySource,
  guide ? null,
}:

let
  inherit (pkgs) lib;
  qt = pkgs.qt6;
  cjkFonts = "${pkgs.noto-fonts-cjk-sans}/share/fonts/opentype/noto-cjk";
  upstreamVersion = lib.replaceStrings [ "-UNKNOWN" ] [ "-dev" ] (
    lib.trim (builtins.readFile "${source}/MPV_VERSION")
  );
in
pkgs.stdenv.mkDerivation {
  pname = "waguri-mpv";
  version = "${upstreamVersion}-g${builtins.substring 0 12 upstreamRevision}";

  src = source;

  nativeBuildInputs = [
    pkgs.addDriverRunpath
    pkgs.meson
    pkgs.ninja
    pkgs.pkg-config
    (pkgs.python3.withPackages (ps: [ ps.jinja2 ps.markupsafe ]))
    pkgs.wayland-scanner
    qt.wrapQtAppsHook
  ];

  buildInputs = [
    pkgs.alsa-lib
    pkgs.curl
    pkgs.ffmpeg
    pkgs.freetype
    pkgs.lcms2
    pkgs.libass
    pkgs.libdisplay-info
    pkgs.libdrm
    pkgs.libgbm
    pkgs.libGL
    pkgs.libjpeg_turbo
    pkgs.libpulseaudio
    pkgs.libva
    pkgs.libx11
    pkgs.libxext
    pkgs.libxkbcommon
    pkgs.libxpresent
    pkgs.libxrandr
    pkgs.libxscrnsaver
    pkgs.libxv
    pkgs.luajit
    pkgs.pipewire
    pkgs.wayland
    pkgs.wayland-protocols
    qt.qtbase
    qt.qtwayland
  ];

  postPatch = ''
    mkdir -p waguri subprojects
    cp -R --no-preserve=mode ${overlaySource}/. waguri/
    cp -R --no-preserve=mode ${libplaceboSource} subprojects/libplacebo
    python3 waguri/apply.py
    # Meson runs upstream generators directly inside the Nix sandbox.
    patchShebangs --build TOOLS
    printf '%s\n' "$version" > MPV_VERSION
  '';

  mesonAutoFeatures = "auto";
  mesonBuildType = "release";
  mesonFlags = [
    "-Ddefault_library=shared"
    "-Dlibmpv=true"
    "-Dcplayer=true"
    "-Dplain-gl=enabled"
    "-Dx11=enabled"
    "-Dwayland=enabled"
    "-Dlibplacebo:vulkan=disabled"
    "-Dlibplacebo:opengl=enabled"
    "-Dlibplacebo:demos=false"
  ];

  # Wrap the Qt frontend only and use the NixOS host's graphics drivers.
  dontWrapQtApps = true;
  postFixup = ''
    addDriverRunpath "$out/bin/mpv"
    addDriverRunpath "$out/bin/waguri-mpv"
    addDriverRunpath "$(readlink -f "$out/lib/libmpv.so")"
    wrapQtApp "$out/bin/waguri-mpv" \
      --prefix LD_LIBRARY_PATH : "${lib.makeLibraryPath [ pkgs.libGL ]}:/run/opengl-driver/lib" \
      --prefix __EGL_VENDOR_LIBRARY_DIRS : "/run/opengl-driver/share/glvnd/egl_vendor.d:${pkgs.libGL}/share/glvnd/egl_vendor.d" \
      --set WAGURI_CJK_FONTS "${cjkFonts}" \
      --set-default FONTCONFIG_FILE "$out/etc/waguri/fonts.conf"
  '';

  postInstall = ''
    mkdir -p "$out/share/doc/waguri-mpv" "$out/etc/waguri"
    substitute ${pkgs.fontconfig.out}/etc/fonts/fonts.conf "$out/etc/waguri/fonts.conf" \
      --replace-fail /etc/fonts/conf.d ${pkgs.fontconfig.out}/etc/fonts/conf.d \
      --replace-fail '</fontconfig>' '<dir>/run/current-system/sw/share/fonts</dir><dir>${cjkFonts}</dir></fontconfig>'
    printf '%s\n' ${lib.escapeShellArg upstreamRevision} > "$out/share/doc/waguri-mpv/upstream-commit"
    install -m 644 ${guide} "$out/share/doc/waguri-mpv/WAGURI-NIXOS.md"
  '';

  meta = {
    description = "Waguri Qt media player built from upstream mpv";
    mainProgram = "waguri-mpv";
    platforms = [ "x86_64-linux" "aarch64-linux" ];
  };
}
